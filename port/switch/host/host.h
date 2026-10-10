/*
HOST.H

Internals of the Switch port's host (the homebrew program, OpenCE.nro). See
port/switch/README.md for the overall design and
port/android/include/halo_android_abi.h for the guest contract, which is the
Android port's: the guest image is the same program.
*/

#ifndef __HALO_SWITCH_HOST_H
#define __HALO_SWITCH_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "halo_android_abi.h"

/* ---------- logging (host_main.c): nxlink's console, and host.log in the
data folder */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* (Android's log priorities, which the guest passes to host_log) */
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6
/* without the C library's locks or the file system: from the exception
handler */
void host_log_raw(const char *text);
/* guest services also used inside the host */
void host_exit(int code) __attribute__((noreturn));
int host_errno(void);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));
/* shows a message in the system's error dialog */
void host_show_message(const char *title, const char *message);

/* ---------- guest memory (host_memory.c)

All memory the guest can address lies below 4 GB. Horizon lets a process
map its own heap pages again at an address of its choice, as a program
loader does for the modules it loads: the host places the Xbox window, the
image and pools for the guest's other mappings that way. */

/* protections, as the guest's mmap and mprotect give them */
#define HOST_PROT_NONE 0
#define HOST_PROT_READ 1
#define HOST_PROT_WRITE 2
#define HOST_PROT_EXEC 4

/* places the fixed ranges, the image's from memory the caller filled (page
aligned, image_size bytes, which this takes over) with its first data_offset
bytes read-only code; returns 0 on success */
int host_memory_initialize(uint32_t image_base, void *image_memory, uint32_t image_size, uint32_t data_offset);
/* page-granular zeroed allocations below 4 GB; NULL on failure */
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
int host_low_protect(void *address, size_t size, int protection);
/* 1 if [address, address + size) was handed out by host_low_map or is one of
the fixed ranges */
int host_low_owns(uintptr_t address, size_t size);
/* the guest's mmap/munmap/mprotect (host_syscall.c); a result or -errno */
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);
/* 1 if a fault at address was a write to a page the renderer watches, which
is now writable again (the exception handler retries the write) */
int host_memory_watch_fault(uint64_t address);
/* config.toml's debug.memory_watch = false: no page is protected, and the
write tracking compares page contents once a frame instead. Call before the
guest starts. */
void host_memory_watch_use_hashes(void);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

/* maps the image from the ELF file in memory (and with it the Xbox window:
the first of the guest's memory to be placed); returns 0 on success */
int host_load_image(const void *elf, size_t size);
/* fills the image's import table; EGL must have started */
void host_link_image(void);

/* ---------- entering guest code (host_thread.c) */

/* calls the guest function at address with up to four 32-bit arguments on
this thread, which must be running on a guest stack (one made by
host_native_thread_create), giving the thread a guest struct pthread first
if it has none; returns the guest's w0 */
uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
/* starts a thread running function(argument) with its stack in guest
memory, so that it can call guest code; the stack is freed after it exits.
Returns 0 or an errno value */
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
/* runs function(argument) on this thread with a new stack in guest memory;
returns what it returns (host_main.c runs the game this way) */
void *host_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size);
/* runs the guest's __guest_start on the calling thread (on a guest stack);
does not return */
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));
/* a number for the calling thread, as the guest's gettid gives it */
int host_thread_id(void);
/* reads the main thread's priority and the program's cores, for the threads
made afterwards (the main thread calls it first of all) */
void host_thread_initialize(void);
/* gives the calling thread a priority above the game's threads, on the
cores the main thread is not on (the threads that play the sound) */
void host_thread_raise(void);
/* a core for a thread of the host's own: not the main thread's */
int host_thread_other_core(void);
/* the core the render thread starts on, the cores it may run on and its
priority */
void host_thread_render_place(int *core, uint32_t *mask, int *priority);
/* host_thread_asm.S: calls function(argument) with the stack pointer at
stack_top, and returns its result on the caller's stack */
void *host_call_on_stack(void *(*function)(void *), void *argument, void *stack_top);

/* ---------- exceptions (host_exception.c, host_exception_asm.S) */

void host_exception_initialize(void);

/* ---------- debugging (host_debug.c) */

/* a thread that runs guest code starts and ends, for the sampler */
void host_debug_thread_started(void);
void host_debug_thread_exited(void);
/* the calling thread's name in the profile (one that runs no guest code is
in the profile once it has a name) */
void host_debug_thread_name(const char *name);
/* config.toml's debug.sample_seconds: seconds between samples of the guest
threads */
void host_debug_start_sampler(double seconds);

/* config.toml's debug.profile: where the time goes, in the log and in
profile.txt in the directory (host_debug.c has what they say) */
void host_profile_start(const char *directory);
extern int host_profile_enabled;

/* what the host counts for the profile */
enum
{
	/* showing a frame: the wait for the display is in it */
	_host_profile_swap,
	/* waiting for the GPU to be done with a frame (host_gl.c), and the maps
	of the buffer of the visibility tests' counts, which wait for it inside
	the driver if it is not */
	_host_profile_gpu_wait,
	_host_profile_gpu_map,
	/* writes to pages the renderer watches (amount: none), and its
	protecting them (amount: pages) */
	_host_profile_fault,
	_host_profile_protect,
	/* reads of files (amount: bytes) */
	_host_profile_read,
	/* waits on a futex */
	_host_profile_futex,
	/* the game's mixer */
	_host_profile_audio,
	/* the render thread (host_render.c): the game's thread's waits for a
	call it needs the result of, for room in the queue and for the frame
	before the one it has just queued; and the frames queued (ticks: their
	calls; amount: the bytes of them) */
	_host_profile_render_wait,
	_host_profile_render_room,
	_host_profile_render_frame,
	_host_profile_render_queued,
	NUMBER_OF_HOST_PROFILE_COUNTS,
};

/* one more of them, which took this long (armGetSystemTick's ticks) */
void host_profile_add(int which, uint64_t ticks, uint64_t amount);
/* a frame shown: the ticks before and after */
void host_profile_frame(uint64_t start, uint64_t end);

/* ---------- import table (generated: host_import_table.c) */

/* the host function for an import name, or NULL */
void *host_resolve_import(const char *name);

/* ---------- the render thread (host_render.c)

With config.toml's display.render_thread, OpenGL's context is a second
thread's, and the calls of the game's thread go to it through a queue. */

/* the most memory a call takes into the queue with it; a call with more
waits for the render thread instead */
#define HOST_RENDER_COPY_MAXIMUM ((size_t)2 * 1024 * 1024)

/* starts the thread: before the image is linked, on the thread the game
will run on. Without it, each function below runs on the caller's thread */
void host_render_enable(void);
/* 1 if OpenGL is another thread's than the caller's */
int host_render_remote(void);
/* the function that queues an OpenGL function, for the guest's import of
it, or NULL */
void *host_render_resolve(const char *name);
/* runs function(context) on the render thread, after everything queued
before it, and waits for it */
void host_render_call(void (*function)(void *context), void *context);
/* the same, giving up after that long (0: never): 1 if it ran */
int host_render_call_within(void (*function)(void *context), void *context, uint64_t nanoseconds);
/* queues function(data) for the render thread, data being size bytes the
caller fills before host_render_send; for the game's thread, where
host_render_remote says 1 */
void *host_render_begin(void (*function)(const void *data), size_t size);
void host_render_send(void);
/* a frame's last call has been queued: waits while the render thread has
yet to show the frame before it */
void host_render_frame(void);
/* where the queue ends now, and a wait until the render thread has made
every call up to there (for the game's thread) */
uint64_t host_render_mark(void);
void host_render_wait_mark(uint64_t mark);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);
/* sets the alpha of the whole window's framebuffer to one, and leaves the
context's state as it was */
void host_gl_make_opaque(void);

/* ---------- files (host_syscall.c) */

/* a file's position is one for all the threads that use its descriptor, and
this C library has no read or write at an offset that leaves it alone: each
call that reads, writes or moves it holds this while it does (the game reads
its maps from several threads at once, each at its own offset) */
void host_file_lock(int descriptor);
void host_file_unlock(int descriptor);

/* ---------- the system (host_main.c) */

/* seconds east of Greenwich of the console's time zone now */
int host_time_zone_offset(void);

#endif
