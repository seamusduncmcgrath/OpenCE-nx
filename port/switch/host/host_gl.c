/*
HOST_GL.C

OpenGL ES for the guest. Its generated entry points (guest_gl.c) import
hostgl_<function>, resolved here to Mesa's function; the arguments already
have host types by then. Only strings need copying back.

Mesa is linked into the program, and EGL hands out its functions by name:
the ones this file calls itself are found the same way, so nothing depends
on which of Mesa's libraries has which.

With the render thread (host_render.c) the context is that thread's: the
guest's imports are then the functions that queue a call for it, and each
function here runs there, in its turn among those calls. The ones the game
needs an answer from wait for it; the others are queued.
*/

#include "host.h"

#include <switch.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <stdlib.h>
#include <string.h>

static void *mesa(const char *name)
{
	return (void *)eglGetProcAddress(name);
}

void *host_gl_resolve(const char *name)
{
	return host_render_remote() ? host_render_resolve(name) : mesa(name);
}

static struct
{
	int loaded;
	const GLubyte *(*GetString)(GLenum name);
	const GLubyte *(*GetStringi)(GLenum name, GLuint index);
	void (*GetIntegerv)(GLenum name, GLint *data);
	void (*BindBuffer)(GLenum target, GLuint buffer);
	void *(*MapBufferRange)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
	GLboolean (*UnmapBuffer)(GLenum target);
	void (*BufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
	GLsync (*FenceSync)(GLenum condition, GLbitfield flags);
	void (*DeleteSync)(GLsync sync);
	GLenum (*ClientWaitSync)(GLsync sync, GLbitfield flags, GLuint64 timeout);
	void (*GetBooleanv)(GLenum name, GLboolean *data);
	void (*GetFloatv)(GLenum name, GLfloat *data);
	GLboolean (*IsEnabled)(GLenum capability);
	void (*Enable)(GLenum capability);
	void (*Disable)(GLenum capability);
	void (*BindFramebuffer)(GLenum target, GLuint framebuffer);
	void (*ColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
	void (*ClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
	void (*Clear)(GLbitfield mask);
} gl;

static void load(void)
{
	if (gl.loaded)
		return;
	gl.GetString = mesa("glGetString");
	gl.GetStringi = mesa("glGetStringi");
	gl.GetIntegerv = mesa("glGetIntegerv");
	gl.BindBuffer = mesa("glBindBuffer");
	gl.MapBufferRange = mesa("glMapBufferRange");
	gl.UnmapBuffer = mesa("glUnmapBuffer");
	gl.BufferSubData = mesa("glBufferSubData");
	gl.FenceSync = mesa("glFenceSync");
	gl.DeleteSync = mesa("glDeleteSync");
	gl.ClientWaitSync = mesa("glClientWaitSync");
	gl.GetBooleanv = mesa("glGetBooleanv");
	gl.GetFloatv = mesa("glGetFloatv");
	gl.IsEnabled = mesa("glIsEnabled");
	gl.Enable = mesa("glEnable");
	gl.Disable = mesa("glDisable");
	gl.BindFramebuffer = mesa("glBindFramebuffer");
	gl.ColorMask = mesa("glColorMask");
	gl.ClearColor = mesa("glClearColor");
	gl.Clear = mesa("glClear");
	if (!gl.GetString || !gl.GetStringi || !gl.GetIntegerv || !gl.BindBuffer || !gl.MapBufferRange || !gl.UnmapBuffer ||
		!gl.BufferSubData || !gl.FenceSync || !gl.DeleteSync || !gl.ClientWaitSync || !gl.GetBooleanv ||
		!gl.GetFloatv || !gl.IsEnabled || !gl.Enable || !gl.Disable || !gl.BindFramebuffer || !gl.ColorMask ||
		!gl.ClearColor || !gl.Clear)
	{
		host_fatal("Mesa is missing OpenGL ES 3 functions");
	}
	gl.loaded = 1;
}

struct string_call
{
	uint32_t name;
	int index;
	char *buffer;
	uint32_t size;
};

static void get_string(void *context)
{
	const struct string_call *call = context;
	const GLubyte *text;

	load();
	text = call->index >= 0 ? gl.GetStringi(call->name, (GLuint)call->index) : gl.GetString(call->name);
	if (!call->size)
		return;
	call->buffer[0] = 0;
	if (text)
	{
		strncpy(call->buffer, (const char *)text, call->size - 1);
		call->buffer[call->size - 1] = 0;
	}
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	struct string_call call = { name, index, buffer, size };

	host_render_call(get_string, &call);
}

struct extension_call
{
	const char *name;
	int found;
};

static void has_extension(void *context)
{
	struct extension_call *call = context;
	GLint count = 0, index;

	load();
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)gl.GetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, call->name))
		{
			call->found = 1;
			return;
		}
	}
}

int host_gl_has_extension(const char *name)
{
	struct extension_call call = { name, 0 };

	host_render_call(has_extension, &call);
	return call.found;
}

/* copies size bytes of a buffer object (the snapshots of the visibility
tests' counters, d3d8_gl.c); ES has no glGetBufferSubData, and the mapping
it offers instead is a host pointer. Binds GL_COPY_READ_BUFFER, which the
renderer uses only for its copies, and leaves it unbound. */
struct read_call
{
	uint32_t buffer, offset, size;
	void *data;
};

static void read_buffer(void *context)
{
	const struct read_call *call = context;
	const void *mapping;
	u64 start;

	load();
	gl.BindBuffer(GL_COPY_READ_BUFFER, call->buffer);
	/* (the map waits for what the GPU has yet to write to the buffer: how
	long is counted) */
	start = armGetSystemTick();
	mapping = gl.MapBufferRange(GL_COPY_READ_BUFFER, call->offset, call->size, GL_MAP_READ_BIT);
	host_profile_add(_host_profile_gpu_map, armGetSystemTick() - start, 0);
	if (mapping)
	{
		memcpy(call->data, mapping, call->size);
		gl.UnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(call->data, 0, call->size);
	}
	gl.BindBuffer(GL_COPY_READ_BUFFER, 0);
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it (port/android/host/host_gl.c). */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

/* Mesa's driver here waits for a fence in a loop that does not sleep: a
wait with a time would keep the thread's core busy for as long as the GPU
is behind. A wait of no time only asks; the thread sleeps between its
questions. */
#define FENCE_POLL_NANOSECONDS 250000ll

/* until the GPU has passed a slot's fence */
static void frame_passed(uint32_t slot)
{
	u64 start, limit;

	load();
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	start = armGetSystemTick();
	/* at most a second: a lost context must not hang the game */
	limit = start + armGetSystemTickFreq();
	for (;;)
	{
		GLenum state = gl.ClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 0);

		if (state != GL_TIMEOUT_EXPIRED || armGetSystemTick() >= limit)
			break;
		svcSleepThread(FENCE_POLL_NANOSECONDS);
	}
	host_profile_add(_host_profile_gpu_wait, armGetSystemTick() - start, 0);
}

/* Reading ahead, with the render thread. A read has the game's thread wait
for the render thread, which at the end of a frame has most of the frame's
calls still to make and the wait for the display after them: with a read in
every frame (a scene with lens flares has one) the game's thread would run
no further ahead than it does without the render thread.

The renderer reads one snapshot of the tests' counters a frame, straight
after host_gl_wait_frame for the ring slot the snapshot is of; a slot's
snapshot is written at the end of the slot's frame, and not again until the
slot comes round (d3d8_gl.c, D3DDevice_Present). So what it reads after one
frame's host_gl_wait_frame was there to be read after the frame's before:
each host_gl_wait_frame also queues the read expected after the next, into
memory here, and when that read comes it waits only for a call queued a
frame ago.

That read is of what the GPU wrote a frame ago, where the renderer's own is
of what it wrote two frames ago, and the GPU may not have got that far: it
is a frame or more behind when it is what the frame rate waits for (the
dock's 1920x1080, say). The map of a buffer the GPU has yet to write waits
for it inside the driver, in the loop that does not sleep: 5% of the render
thread's time on a console, with nothing gained by it. The snapshot is the
last the GPU does before the slot's fence, so the read waits for that fence
first (frame_passed), asleep.

What is expected is what was seen: the slot waited for after this one the
last time, and what was read after the wait for that slot the last time.
A read ahead is for the read after the next wait, if that wait is for its
slot, and for no other; any other read waits, as before. */
#define AHEAD_BYTES (64 * 1024)

struct read_ahead
{
	/* what was last read after the wait for this slot, and the slot waited
	for after this one */
	uint32_t buffer, offset, size;
	int known;
	uint32_t next;
	int next_known;
	/* a read of it queued ahead that is still good, where in the queue
	that ends, and where its bytes go */
	int ready;
	uint64_t mark;
	uint8_t *data;
};

struct ahead_call
{
	uint32_t slot, buffer, offset, size;
	uint8_t *data;
};

static struct read_ahead ahead[FRAME_FENCE_SLOTS];
/* the slot last waited for, and the one a read is queued ahead for */
static int waited_slot = -1, ahead_slot = -1;

static void read_ahead(const void *data)
{
	const struct ahead_call *call = data;
	struct read_call read = { call->buffer, call->offset, call->size, call->data };

	frame_passed(call->slot);
	read_buffer(&read);
}

/* host_gl_wait_frame for a slot has been queued (the game's thread) */
static void read_ahead_waited(uint32_t slot)
{
	if (waited_slot >= 0)
	{
		ahead[waited_slot].ready = 0;
		ahead[waited_slot].next = slot;
		ahead[waited_slot].next_known = 1;
	}
	if (ahead_slot >= 0 && ahead_slot != (int)slot)
		ahead[ahead_slot].ready = 0;
	ahead_slot = -1;
	waited_slot = (int)slot;
	if (ahead[slot].next_known && ahead[slot].next != slot)
	{
		struct read_ahead *next = &ahead[ahead[slot].next];

		if (next->known && !next->data)
			next->data = malloc(AHEAD_BYTES);
		if (next->known && next->data)
		{
			struct ahead_call *call = host_render_begin(read_ahead, sizeof(*call));

			call->slot = ahead[slot].next;
			call->buffer = next->buffer;
			call->offset = next->offset;
			call->size = next->size;
			call->data = next->data;
			host_render_send();
			next->mark = host_render_mark();
			next->ready = 1;
			ahead_slot = (int)ahead[slot].next;
		}
	}
}

void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	struct read_call call = { buffer, offset, size, data };

	if (host_render_remote() && waited_slot >= 0)
	{
		struct read_ahead *slot = &ahead[waited_slot];

		if (slot->ready && slot->buffer == buffer && slot->offset == offset && slot->size == size)
		{
			slot->ready = 0;
			host_render_wait_mark(slot->mark);
			memcpy(data, slot->data, size);
			return;
		}
		slot->ready = 0;
		slot->buffer = buffer;
		slot->offset = offset;
		slot->size = size;
		slot->known = size <= AHEAD_BYTES;
	}
	host_render_call(read_buffer, &call);
}

static void fence_frame(const void *data)
{
	uint32_t slot = *(const uint32_t *)data;

	load();
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		gl.DeleteSync(frame_fences[slot]);
	frame_fences[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/* runs function(slot) where the context is, in its turn */
static void frame_call(void (*function)(const void *data), uint32_t slot)
{
	if (host_render_remote())
	{
		*(uint32_t *)host_render_begin(function, sizeof(slot)) = slot;
		host_render_send();
	}
	else
	{
		function(&slot);
	}
}

void host_gl_fence_frame(uint32_t slot)
{
	frame_call(fence_frame, slot);
}

static void wait_frame(const void *data)
{
	uint32_t slot = *(const uint32_t *)data;

	frame_passed(slot);
	if (slot < FRAME_FENCE_SLOTS && frame_fences[slot])
	{
		gl.DeleteSync(frame_fences[slot]);
		frame_fences[slot] = NULL;
	}
}

/* (with the render thread it is that thread which waits, before the calls
that write the buffers: the game's thread goes on) */
void host_gl_wait_frame(uint32_t slot)
{
	frame_call(wait_frame, slot);
	if (host_render_remote() && slot < FRAME_FENCE_SLOTS)
		read_ahead_waited(slot);
}

/* writes data into the buffer bound to target: an unsynchronized mapping,
which promises the driver that no queued draw reads the range (the renderer
waits for the frame that last used it, host_gl_wait_frame), and no
invalidation, which would have it discard the range first
(port/android/host/host_gl.c has what that cost) */
struct write_call
{
	uint32_t target, offset, size;
	/* (NULL: the bytes follow the call, in the queue) */
	const void *data;
};

static void buffer_write(const struct write_call *call, const void *data)
{
	void *mapping;

	load();
	mapping = gl.MapBufferRange(call->target, call->offset, call->size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if (!mapping)
	{
		gl.BufferSubData(call->target, call->offset, call->size, data);
		return;
	}
	memcpy(mapping, data, call->size);
	gl.UnmapBuffer(call->target);
}

static void buffer_write_waited(void *context)
{
	const struct write_call *call = context;

	buffer_write(call, call->data);
}

static void buffer_write_queued(const void *data)
{
	const struct write_call *call = data;

	buffer_write(call, call + 1);
}

/* (with the render thread the bytes go into the queue with the call, and
from there into the buffer: the game's thread still copies them once, and
does not wait) */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	struct write_call call = { target, offset, size, data };

	if (host_render_remote() && size <= HOST_RENDER_COPY_MAXIMUM)
	{
		struct write_call *queued = host_render_begin(buffer_write_queued, sizeof(call) + size);

		*queued = call;
		queued->data = NULL;
		memcpy(queued + 1, data, size);
		host_render_send();
		return;
	}
	host_render_call(buffer_write_waited, &call);
}

/* The game leaves what it likes in the alpha of its picture, as on the Xbox,
where nothing read it. A window whose framebuffer has alpha may be shown
through it: before such a framebuffer is shown, its alpha is set to one
(host_sdl.c). The renderer keeps track of the context's state itself, so
each thing changed here is put back. */
void host_gl_make_opaque(void)
{
	GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	GLfloat color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	GLint framebuffer = 0;
	GLboolean scissor, discard;

	load();
	gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
	gl.GetFloatv(GL_COLOR_CLEAR_VALUE, color);
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
	scissor = gl.IsEnabled(GL_SCISSOR_TEST);
	discard = gl.IsEnabled(GL_RASTERIZER_DISCARD);
	if (framebuffer)
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	if (scissor)
		gl.Disable(GL_SCISSOR_TEST);
	if (discard)
		gl.Disable(GL_RASTERIZER_DISCARD);
	gl.ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.ClearColor(color[0], color[1], color[2], color[3]);
	gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
	if (discard)
		gl.Enable(GL_RASTERIZER_DISCARD);
	if (scissor)
		gl.Enable(GL_SCISSOR_TEST);
	if (framebuffer)
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)framebuffer);
}
