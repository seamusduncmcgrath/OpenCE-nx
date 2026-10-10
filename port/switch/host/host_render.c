/*
HOST_RENDER.C

OpenGL on a thread of its own (config.toml's display.render_thread).

The game draws on the thread that runs it. With Mesa's driver here about a
third of that thread's time in a busy scene went inside OpenGL's functions,
and it had no time left while two other cores were mostly idle. With the
render thread the guest's OpenGL imports are functions that write the call
into a queue and return (tools/switch_gl_queue.py generates them from the
OpenGL ES headers), and a second thread, which has the context, takes the
calls from the queue and makes them.

- A call that only gives OpenGL values is queued. One that gives it memory
  (a uniform's values, vertices, a texture's pixels) is queued with a copy
  of that memory, unless there is a lot of it: then, and for every call
  that returns something, the game's thread waits until the render thread
  has made the call, and with it every call before it.
- The queue is one block of memory used as a ring, written by one thread
  and read by one. Neither takes a lock to pass a call on: the lock and its
  conditions are for when one of them sleeps (the render thread with
  nothing to do; the game's thread waiting for a call, for room in the
  queue or for a frame). A thread that waits looks for the other's progress
  for a moment first: asleep, it is the kernel that wakes it, later.
- Showing a frame is a call in the queue like any other (host_sdl.c). The
  game's thread goes on to the next frame, and waits only while the render
  thread has yet to show the frame before the one just queued.

Everything else that needs the context goes the same way: it is made on the
render thread (host_sdl.c), and host_gl.c's own calls run there.

The calls come from one thread, the one the game runs on, which is the one
that makes the context. Another thread's (one that stops the program, and
wants the screen for its message: host_video_release) is passed on apart
from the queue.
*/

#include "host.h"

#include <switch.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* the queue's memory, a power of two: a busy frame's calls and the vertices
with them are about 6 MB, and the game's thread is a frame ahead at most */
#define QUEUE_SIZE (16u * 1024 * 1024)
#define QUEUE_MASK ((uint64_t)QUEUE_SIZE - 1)
/* the most memory copied into the queue with one call */
#define RENDER_COPY_MAXIMUM HOST_RENDER_COPY_MAXIMUM
/* how long a thread looks for the other's progress before it sleeps, in
nanoseconds: the render thread for the next call (the game makes them a few
microseconds apart while it draws, and none while it does not), and the
game's thread for a call it waits for */
#define IDLE_SPIN_NANOSECONDS 200000ull
#define CALL_SPIN_NANOSECONDS 100000ull
/* (Mesa compiles shaders on it) */
#define RENDER_STACK_SIZE (8 * 1024 * 1024)
#define CACHE_LINE 64

/* what each call in the queue starts with; all of it is size bytes, a
multiple of 16. One with no function only fills the memory up to its end. */
struct render_command
{
	void (*execute)(const struct render_command *command);
	uint32_t size;
	uint32_t unused;
};

typedef void render_execute(const struct render_command *command);

static struct
{
	uint8_t *memory;
	Mutex lock;
	CondVar work, progress;

	/* the game's thread's: the bytes queued since the start, and the count
	they may reach before the render thread's is looked at again (short of
	the end of the memory, and of what the render thread has yet to take) */
	uint64_t written __attribute__((aligned(CACHE_LINE)));
	uint64_t room;
	/* the count after the call that shows the last frame */
	uint64_t frame_end;
	/* since the last frame, for the profile */
	uint64_t calls, bytes;

	/* the bytes the render thread may take */
	uint64_t published __attribute__((aligned(CACHE_LINE)));
	/* the count the game's thread sleeps until (none: the largest) */
	uint64_t awaited;

	/* the bytes the render thread has taken */
	uint64_t consumed __attribute__((aligned(CACHE_LINE)));
	/* whether it sleeps */
	int sleeping;
} queue;

/* a call from a thread that is not the game's */
static struct
{
	Mutex lock;
	void (*function)(void *context);
	void *context;
	int pending;
} urgent;

static Thread render_thread;
static int render_enabled;
/* the thread whose calls the queue takes */
static int game_thread;
static __thread int render_self;
static uint64_t idle_spin_ticks, call_spin_ticks;

#define LOAD(value) __atomic_load_n(&(value), __ATOMIC_SEQ_CST)
#define STORE(value, to) __atomic_store_n(&(value), (to), __ATOMIC_SEQ_CST)

static inline void pause_briefly(void)
{
#ifdef __aarch64__
	__asm__ __volatile__("yield");
#endif
}

/* ---------- the game's thread */

/* wakes the render thread, which sleeps (or is about to) */
static void render_wake(void)
{
	mutexLock(&queue.lock);
	if (queue.sleeping)
	{
		STORE(queue.sleeping, 0);
		condvarWakeOne(&queue.work);
	}
	mutexUnlock(&queue.lock);
}

/* until the render thread has taken target bytes: looking for spin ticks,
then asleep, for limit ticks at most (0: for as long as it takes). Returns 0
if it has not by then. */
static int render_wait(uint64_t target, uint64_t spin, uint64_t limit, int counted)
{
	u64 start;
	int reached = 1;

	if (LOAD(queue.consumed) >= target)
		return 1;
	start = armGetSystemTick();
	while (spin)
	{
		int turn;

		for (turn = 0; turn < 32; turn++)
		{
			if (LOAD(queue.consumed) >= target)
				goto done;
			pause_briefly();
		}
		if (armGetSystemTick() - start >= spin)
			break;
	}
	mutexLock(&queue.lock);
	for (;;)
	{
		/* (said before it is looked at: the render thread looks at this
		after each call it has taken) */
		STORE(queue.awaited, target);
		if (LOAD(queue.consumed) >= target)
			break;
		if (!limit)
		{
			condvarWait(&queue.progress, &queue.lock);
		}
		else
		{
			u64 waited = armGetSystemTick() - start;

			if (waited >= limit)
			{
				reached = 0;
				break;
			}
			condvarWaitTimeout(&queue.progress, &queue.lock, armTicksToNs(limit - waited));
		}
	}
	STORE(queue.awaited, UINT64_MAX);
	mutexUnlock(&queue.lock);
done:
	host_profile_add(counted, armGetSystemTick() - start, 0);
	return reached;
}

static inline void render_publish(void)
{
	STORE(queue.published, queue.written);
	if (__builtin_expect(LOAD(queue.sleeping), 0))
		render_wake();
}

/* room for a call of size bytes in one piece, which the next does not have */
static void render_make_room(size_t size)
{
	for (;;)
	{
		uint64_t end = (queue.written & ~QUEUE_MASK) + QUEUE_SIZE;
		uint64_t needed = queue.written + size > end ? end : queue.written + size;
		uint64_t limit = LOAD(queue.consumed) + QUEUE_SIZE;

		if (needed > limit)
		{
			/* full: until the render thread has taken as much */
			render_wait(needed - QUEUE_SIZE, 0, 0, _host_profile_render_room);
			continue;
		}
		if (queue.written + size > end)
		{
			/* the call would run past the end of the memory: one that
			only fills the rest of it, and the call at the start */
			struct render_command *filler = (struct render_command *)(queue.memory + (queue.written & QUEUE_MASK));

			filler->execute = NULL;
			filler->size = (uint32_t)(end - queue.written);
			queue.written = end;
			render_publish();
			continue;
		}
		queue.room = limit < end ? limit : end;
		return;
	}
}

/* a call of size bytes, which begin with a struct render_command: the
caller fills the rest, and render_end passes it on */
static inline void *render_begin(size_t size, render_execute *execute)
{
	struct render_command *command;

	size = (size + 15) & ~(size_t)15;
	if (__builtin_expect(queue.written + size > queue.room, 0))
		render_make_room(size);
	command = (struct render_command *)(queue.memory + (queue.written & QUEUE_MASK));
	command->execute = execute;
	command->size = (uint32_t)size;
	queue.written += size;
	queue.calls++;
	queue.bytes += size;
	return command;
}

static inline void render_end(void)
{
	render_publish();
}

struct call_command
{
	struct render_command command;
	void (*function)(void *context);
	void *context;
};

static void call_execute(const struct render_command *command)
{
	const struct call_command *call = (const struct call_command *)command;

	call->function(call->context);
}

/* runs function(context) on the render thread, after the calls queued
before it, and waits for it */
static void render_call(void (*function)(void *context), void *context)
{
	struct call_command *call = render_begin(sizeof(*call), call_execute);

	call->function = function;
	call->context = context;
	render_end();
	render_wait(queue.written, call_spin_ticks, 0, _host_profile_render_wait);
}

/* ---------- what the generated calls ask about the memory they copy */

/* the bytes of count things, or none */
static inline size_t copy_count(long long count, size_t each)
{
	return count > 0 ? (size_t)count * each : 0;
}

/* how glPixelStorei has left the reading of pixels: the padding of each
row, and whether any of the other settings is not zero */
static GLint unpack_alignment = 4;
static unsigned int unpack_others;

/* the bytes OpenGL reads of pixels given so, or more than any copy is if
that cannot be told here (the game gives bytes of one to four channels, in
rows without gaps; nothing binds a buffer to read them from) */
static size_t copy_pixels(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type)
{
	size_t texel, row;

	if (width <= 0 || height <= 0 || depth <= 0)
		return 0;
	if (unpack_others || type != GL_UNSIGNED_BYTE)
		return SIZE_MAX;
	switch (format)
	{
	case GL_RGBA:
	case GL_BGRA_EXT:
		texel = 4;
		break;
	case GL_RGB:
		texel = 3;
		break;
	case GL_RG:
	case GL_LUMINANCE_ALPHA:
		texel = 2;
		break;
	case GL_RED:
	case GL_ALPHA:
	case GL_LUMINANCE:
		texel = 1;
		break;
	default:
		return SIZE_MAX;
	}
	row = ((size_t)width * texel + (size_t)unpack_alignment - 1) & ~((size_t)unpack_alignment - 1);
	/* (the last row is read without its padding) */
	return row * ((size_t)height * (size_t)depth - 1) + (size_t)width * texel;
}

#include "host_gl_queue.inc"

static void q_glPixelStorei(GLenum name, GLint value)
{
	static const GLenum others[] =
	{
		GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES,
	};
	unsigned int index;

	if (name == GL_UNPACK_ALIGNMENT && (value == 1 || value == 2 || value == 4 || value == 8))
		unpack_alignment = value;
	for (index = 0; index < sizeof(others) / sizeof(others[0]); index++)
	{
		if (name == others[index])
			unpack_others = value ? unpack_others | 1u << index : unpack_others & ~(1u << index);
	}
	a_glPixelStorei(name, value);
}

/* A texture the game has not drawn before is made in the middle of a frame,
and its name is all that would have the game's thread wait there: names are
made a number at a time, ahead of their use (one is the texture's from when
it is made to when it is deleted, bound or not). */
#define TEXTURE_NAMES 64

static void q_glGenTextures(GLsizei count, GLuint *names)
{
	static GLuint spare[TEXTURE_NAMES];
	static int spare_count;
	GLsizei index;

	if (count <= 0 || count > TEXTURE_NAMES)
	{
		s_glGenTextures(count, names);
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (!spare_count)
		{
			memset(spare, 0, sizeof(spare));
			s_glGenTextures(TEXTURE_NAMES, spare);
			spare_count = TEXTURE_NAMES;
		}
		names[index] = spare[--spare_count];
	}
}

/* ---------- the render thread */

static void urgent_run(void)
{
	mutexLock(&queue.lock);
	if (urgent.pending)
	{
		void (*function)(void *context) = urgent.function;
		void *context = urgent.context;

		mutexUnlock(&queue.lock);
		function(context);
		mutexLock(&queue.lock);
		STORE(urgent.pending, 0);
		condvarWakeAll(&queue.progress);
	}
	mutexUnlock(&queue.lock);
}

/* nothing to take: looks for the next call for a moment, then sleeps */
static void render_idle(uint64_t consumed)
{
	u64 start = armGetSystemTick();

	do
	{
		int turn;

		for (turn = 0; turn < 32; turn++)
		{
			if (LOAD(queue.published) != consumed || LOAD(urgent.pending))
				return;
			pause_briefly();
		}
	} while (armGetSystemTick() - start < idle_spin_ticks);
	mutexLock(&queue.lock);
	for (;;)
	{
		/* (said before it is looked at: the game's thread looks at this
		after each call it has queued) */
		STORE(queue.sleeping, 1);
		if (LOAD(queue.published) != consumed || LOAD(urgent.pending))
			break;
		condvarWait(&queue.work, &queue.lock);
	}
	STORE(queue.sleeping, 0);
	mutexUnlock(&queue.lock);
}

static void render_main(void *unused)
{
	uint64_t consumed = 0;

	(void)unused;
	render_self = 1;
	host_debug_thread_name("render");
	for (;;)
	{
		uint64_t published = LOAD(queue.published);

		if (__builtin_expect(LOAD(urgent.pending), 0))
			urgent_run();
		if (consumed == published)
		{
			render_idle(consumed);
			continue;
		}
		while (consumed != published)
		{
			const struct render_command *command =
				(const struct render_command *)(queue.memory + (consumed & QUEUE_MASK));

			if (command->execute)
				command->execute(command);
			consumed += command->size;
			STORE(queue.consumed, consumed);
			if (__builtin_expect(consumed >= LOAD(queue.awaited), 0))
			{
				mutexLock(&queue.lock);
				if (consumed >= queue.awaited)
				{
					STORE(queue.awaited, UINT64_MAX);
					condvarWakeAll(&queue.progress);
				}
				mutexUnlock(&queue.lock);
			}
		}
	}
}

/* ---------- for the rest of the host */

void host_render_enable(void)
{
	int core, priority;
	uint32_t mask;

	queue.memory = memalign(0x1000, QUEUE_SIZE);
	if (!queue.memory)
	{
		host_logf(HOST_LOG_WARN, "render thread: no memory for its queue; the game's thread draws");
		return;
	}
	mutexInit(&queue.lock);
	condvarInit(&queue.work);
	condvarInit(&queue.progress);
	mutexInit(&urgent.lock);
	queue.awaited = UINT64_MAX;
	idle_spin_ticks = armNsToTicks(IDLE_SPIN_NANOSECONDS);
	call_spin_ticks = armNsToTicks(CALL_SPIN_NANOSECONDS);
	game_thread = host_thread_id();
	/* (the profile's first thread is the game's: before the render thread
	gives its name) */
	host_debug_thread_started();
	host_thread_render_place(&core, &mask, &priority);
	if (R_FAILED(threadCreate(&render_thread, render_main, NULL, NULL, RENDER_STACK_SIZE, priority, core)))
	{
		host_logf(HOST_LOG_WARN, "render thread: it cannot be made; the game's thread draws");
		free(queue.memory);
		queue.memory = NULL;
		return;
	}
	svcSetThreadCoreMask(render_thread.handle, core, mask);
	if (R_FAILED(threadStart(&render_thread)))
	{
		host_logf(HOST_LOG_WARN, "render thread: it cannot be started; the game's thread draws");
		threadClose(&render_thread);
		free(queue.memory);
		queue.memory = NULL;
		return;
	}
	render_enabled = 1;
	host_logf(HOST_LOG_INFO, "render thread: on core %d (of cores %02x), priority %d, a queue of %u MB", core,
		(unsigned)mask, priority, (unsigned)(QUEUE_SIZE >> 20));
}

int host_render_remote(void)
{
	return render_enabled && !render_self;
}

void *host_render_resolve(const char *name)
{
	unsigned long index;

	for (index = 0; index < sizeof(render_functions) / sizeof(render_functions[0]); index++)
	{
		if (!strcmp(render_functions[index].name, name))
		{
			void *function = (void *)eglGetProcAddress(name);

			if (!function)
				return NULL;
			memcpy(render_functions[index].real, &function, sizeof(function));
			return render_functions[index].queued;
		}
	}
	return NULL;
}

int host_render_call_within(void (*function)(void *context), void *context, uint64_t nanoseconds)
{
	u64 limit = nanoseconds ? armNsToTicks(nanoseconds) : 0;

	if (!host_render_remote())
	{
		function(context);
		return 1;
	}
	if (host_thread_id() == game_thread)
	{
		struct call_command *call = render_begin(sizeof(*call), call_execute);

		call->function = function;
		call->context = context;
		render_end();
		return render_wait(queue.written, call_spin_ticks, limit, _host_profile_render_wait);
	}
	/* another thread's: not in the queue, which is the game's thread's to
	write, and so not in its turn */
	{
		u64 start = armGetSystemTick();
		int done = 0;

		mutexLock(&urgent.lock);
		mutexLock(&queue.lock);
		urgent.function = function;
		urgent.context = context;
		STORE(urgent.pending, 1);
		if (queue.sleeping)
		{
			STORE(queue.sleeping, 0);
			condvarWakeOne(&queue.work);
		}
		for (;;)
		{
			u64 waited = armGetSystemTick() - start;

			if (!urgent.pending)
			{
				done = 1;
				break;
			}
			if (limit && waited >= limit)
				break;
			if (limit)
				condvarWaitTimeout(&queue.progress, &queue.lock, armTicksToNs(limit - waited));
			else
				condvarWait(&queue.progress, &queue.lock);
		}
		/* (one not run in time is not run later either, unless it is running now) */
		if (!done)
			STORE(urgent.pending, 0);
		mutexUnlock(&queue.lock);
		mutexUnlock(&urgent.lock);
		return done;
	}
}

void host_render_call(void (*function)(void *context), void *context)
{
	host_render_call_within(function, context, 0);
}

struct post_command
{
	struct render_command command;
	void (*function)(const void *data);
};

static void post_execute(const struct render_command *command)
{
	const struct post_command *post = (const struct post_command *)command;

	post->function(post + 1);
}

void *host_render_begin(void (*function)(const void *data), size_t size)
{
	struct post_command *post = render_begin(sizeof(*post) + size, post_execute);

	post->function = function;
	return post + 1;
}

void host_render_send(void)
{
	render_end();
}

uint64_t host_render_mark(void)
{
	return queue.written;
}

void host_render_wait_mark(uint64_t mark)
{
	render_wait(mark, call_spin_ticks, 0, _host_profile_render_wait);
}

void host_render_frame(void)
{
	uint64_t before = queue.frame_end;

	queue.frame_end = queue.written;
	/* (the calls of the frame as the count's time, for want of a field) */
	host_profile_add(_host_profile_render_queued, queue.calls, queue.bytes);
	queue.calls = 0;
	queue.bytes = 0;
	if (before)
		render_wait(before, 0, 0, _host_profile_render_frame);
}
