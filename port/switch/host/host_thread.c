/*
HOST_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so every
thread that runs it needs its stack in guest memory. libnx maps the stacks
of the threads it makes high in the address space, even ones made from
memory the caller supplies, so each thread starts on a small stack of
libnx's and moves to one in guest memory itself (host_thread_asm.S). Host
functions the guest calls run on the guest's stack; nothing in Horizon or
libnx minds where a thread's stack is. The guest's thread pointer (its musl
struct pthread) is kept per thread in host TLS.

An application may use three of the console's cores. The main thread, which
runs the game and draws it, is what the frame rate waits for: it has one of
the cores to itself, and every other thread is kept to the other two. The
system does not take a core from a running thread for another of the same
priority, and does not move a thread off the one core it is allowed; a
thread put on the main thread's core would hold the game up for as long as
it ran, and one of a higher priority (the sound's) each time it ran.
*/

#include "host.h"

#include <switch.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_SIZE 0x4000
/* what a thread runs on before and after its guest stack */
#define ENTRY_STACK_SIZE 0x10000
#define DEFAULT_PRIORITY 0x2c

static __thread uint32_t guest_tp;
static __thread int thread_id;
static volatile int next_thread_id = 1;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

int host_thread_id(void)
{
	if (!thread_id)
		thread_id = __sync_fetch_and_add(&next_thread_id, 1);
	return thread_id;
}

/* ---------- stacks */

static void *stack_allocate(size_t size, void **mapping, size_t *mapping_size)
{
	size_t total = size + GUARD_SIZE;
	void *base = host_low_map(total, HOST_PROT_READ | HOST_PROT_WRITE);

	if (!base)
		return NULL;
	/* guard pages at the bottom */
	host_low_protect(base, GUARD_SIZE, HOST_PROT_NONE);
	*mapping = base;
	*mapping_size = total;
	return (char *)base + total;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);

	return sp < 0x100000000ULL;
}

void *host_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size)
{
	void *mapping, *top, *result;
	size_t mapping_size;

	top = stack_allocate(stack_size, &mapping, &mapping_size);
	if (!top)
		return NULL;
	host_debug_thread_started();
	result = host_call_on_stack(function, argument, top);
	host_debug_thread_exited();
	host_low_unmap(mapping, mapping_size);
	return result;
}

/* ---------- calling into the guest */

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
	return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	if (!on_guest_stack())
		host_fatal("the game was started on a thread without a guest stack");
	((void (*)(uint32_t))(uintptr_t)host_image.header->start)(boot);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- guest threads */

struct thread_start
{
	Thread thread;
	void *(*function)(void *);
	void *argument;
	void *stack_top;
	void *mapping;
	size_t mapping_size;
	struct thread_start *next;
};

static Mutex finished_lock;
static struct thread_start *finished_threads;

/* the cores of the threads that are not the main one, and how many */
static u64 other_mask;
static int other_cores[8], other_count;
static s32 thread_priority = DEFAULT_PRIORITY;
static volatile unsigned int thread_count;

/* the cores the program may use and the priority of its threads: the main
thread's, which calls this before any other thread is made */
void host_thread_initialize(void)
{
	u64 mask = 0;
	s32 priority = 0;
	int own = (int)svcGetCurrentProcessorNumber(), core;

	if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !(mask & 0xff))
		mask = 1ull << own;
	mask &= 0xff;
	other_mask = mask & ~(1ull << own);
	/* (a program with one core: all of its threads share it) */
	if (!other_mask)
		other_mask = mask;
	other_count = 0;
	for (core = 0; core < 8; core++)
	{
		if (other_mask & (1ull << core))
			other_cores[other_count++] = core;
	}
	if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
		thread_priority = priority;
	/* (the main thread is on its core alone already: said again) */
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, own, 1u << own);
	host_logf(HOST_LOG_INFO, "threads: the main one on core %d, the others on cores %02x, priority %d", own,
		(unsigned)other_mask, (int)thread_priority);
}

/* the core a thread starts on: each of the other threads' cores in turn */
static int next_core(void)
{
	unsigned int turn = __sync_fetch_and_add(&thread_count, 1);

	if (!other_count)
		host_thread_initialize();
	return other_cores[turn % (unsigned int)other_count];
}

int host_thread_other_core(void)
{
	if (!other_count)
		host_thread_initialize();
	return other_cores[other_count - 1];
}

/* The render thread (host_render.c) starts on the first of the other
threads' cores, where the sound's threads start on the last, and may be
moved to another of them. It goes ahead of the game's other threads, which
would otherwise hold a frame up for as long as one of them ran on its core,
and behind the sound's. */
void host_thread_render_place(int *core, uint32_t *mask, int *priority)
{
	if (!other_count)
		host_thread_initialize();
	*core = other_cores[0];
	*mask = (uint32_t)other_mask;
	*priority = thread_priority - 1 < 28 ? 28 : thread_priority - 1;
}

/* closes the threads that have ended */
static void reap(void)
{
	struct thread_start *list;

	mutexLock(&finished_lock);
	list = finished_threads;
	finished_threads = NULL;
	mutexUnlock(&finished_lock);
	while (list)
	{
		struct thread_start *next = list->next;

		threadWaitForExit(&list->thread);
		threadClose(&list->thread);
		free(list);
		list = next;
	}
}

static void thread_entry(void *context)
{
	struct thread_start *start = context;

	host_debug_thread_started();
	host_call_on_stack(start->function, start->argument, start->stack_top);
	/* (on libnx's stack again) */
	host_debug_thread_exited();
	guest_tp = 0;
	host_low_unmap(start->mapping, start->mapping_size);
	mutexLock(&finished_lock);
	start->next = finished_threads;
	finished_threads = start;
	mutexUnlock(&finished_lock);
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start;
	int core;

	reap();
	start = calloc(1, sizeof(*start));
	if (!start)
		return ENOMEM;
	stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
	start->stack_top = stack_allocate(stack_size, &start->mapping, &start->mapping_size);
	if (!start->stack_top)
	{
		free(start);
		return EAGAIN;
	}
	start->function = function;
	start->argument = argument;
	core = next_core();
	if (R_FAILED(threadCreate(&start->thread, thread_entry, start, NULL, ENTRY_STACK_SIZE, thread_priority, core)))
	{
		host_low_unmap(start->mapping, start->mapping_size);
		free(start);
		return EAGAIN;
	}
	svcSetThreadCoreMask(start->thread.handle, core, (u32)other_mask);
	if (R_FAILED(threadStart(&start->thread)))
	{
		threadClose(&start->thread);
		host_low_unmap(start->mapping, start->mapping_size);
		free(start);
		return EAGAIN;
	}
	return 0;
}

/* The calling thread plays the sound: ahead of the game's other threads,
which it must not wait behind (a buffer that is late is a gap in the sound),
and off the main thread's core, which it would take each time it ran. SDL
makes its audio thread on the main thread's core with the lowest priority
there is. The two threads that play the sound run in turn (host_sdl.c), so
they start on one core. */
void host_thread_raise(void)
{
	Handle self = threadGetCurHandle();
	s32 priority = thread_priority - 2;

	svcSetThreadCoreMask(self, host_thread_other_core(), (u32)other_mask);
	/* (the highest a program may have is 28) */
	svcSetThreadPriority(self, priority < 28 ? 28 : (u32)priority);
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}
