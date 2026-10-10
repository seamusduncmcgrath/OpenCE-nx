/*
HOST_DEBUG.C

Where the program spends its time, for a console, where no debugger and no
profiler attaches. Two tools, both set in config.toml's [debug]:

sample_seconds: every thread that runs guest code is paused that often and
its program counter, link register and frame chain are written to the log.
This finds a hang.

profile: every thread is paused each few milliseconds and its program
counter counted. Every two seconds a line of the log says how the time went:
the frames and the longest of them, the share of a core each thread used,
what the main thread's samples were in (the game's code, the program's,
which is Mesa's mostly, calls to the system's services, waiting), and the
counts the rest of the host keeps (host_profile_add: waits for the GPU,
write faults, reads of files, the mixer, the render thread's queue). The
samples of the seconds in
which the main thread was busy, or a frame was late, are added up, and
written to profile.txt in the data folder each twenty such seconds, most
frequent first:

	<thread> <where> <called by> <the game's caller> <count> <callers...> / <outermost...>

An address is g:<address> in the game's image (build/switch/halo_guest.elf)
or h+<offset> in the program (build/switch/OpenCE.elf); svc<number> is a
thread in that system call, and "called by" then the address that made it.
The game's caller is the nearest of the game's functions on the stack, for
a sample that is not in the game; a thread in a system call has the game's
callers above that one too, to tell what a wait is for. The main thread's
samples also have the game's outermost callers, the first of them first, to
tell which part of a frame the time belongs to (its simulation, its
drawing); a thread that runs none of the game's code (the render thread)
has the program's own outermost callers there, which tell the OpenGL call
its time was in. tools/switch_profile.py names them all.

A thread waiting in a system call shows where it called from: the kernel
gives its frame pointer and return address then, and no other registers.
*/

#include "host.h"

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_THREADS 64

#define THREAD_NAME_SIZE 20

struct tracked_thread
{
	Handle handle;
	char name[THREAD_NAME_SIZE];
	/* the thread's time on the cores at the last report, in ticks */
	u64 ticks;
};

static struct tracked_thread threads[MAXIMUM_THREADS];
static Mutex threads_lock;
static Thread sampler_thread;
static unsigned int thread_serial;

static struct tracked_thread *thread_find(Handle handle, int add)
{
	struct tracked_thread *free_slot = NULL;
	int index;

	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (threads[index].handle == handle)
			return &threads[index];
		if (!threads[index].handle && !free_slot)
			free_slot = &threads[index];
	}
	if (!add || !free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->handle = handle;
	/* (the first is the one the game starts on) */
	if (!thread_serial++)
		snprintf(free_slot->name, sizeof(free_slot->name), "main");
	else
		snprintf(free_slot->name, sizeof(free_slot->name), "thread%u", thread_serial - 1);
	return free_slot;
}

void host_debug_thread_started(void)
{
	mutexLock(&threads_lock);
	thread_find(threadGetCurHandle(), 1);
	mutexUnlock(&threads_lock);
}

void host_debug_thread_exited(void)
{
	struct tracked_thread *thread;

	mutexLock(&threads_lock);
	thread = thread_find(threadGetCurHandle(), 0);
	if (thread)
		thread->handle = 0;
	mutexUnlock(&threads_lock);
}

void host_debug_thread_name(const char *name)
{
	struct tracked_thread *thread;

	mutexLock(&threads_lock);
	thread = thread_find(threadGetCurHandle(), 1);
	if (thread)
		snprintf(thread->name, sizeof(thread->name), "%s", name);
	mutexUnlock(&threads_lock);
}

/* ---------- frame chains */

/* the memory around a frame record that is known to be readable */
struct readable_region
{
	u64 start, end;
};

/* whether the two words of a frame record can be read: a stopped thread's
frame pointer is whatever its code left in the register, and what is at an
address is asked of the kernel (once for each region, a stack as a rule)
rather than found out */
static int frame_readable(struct readable_region *region, u64 address)
{
	MemoryInfo information;
	u32 page_information;

	if (!address || (address & 7))
		return 0;
	if (address >= region->start && address + 16 <= region->end)
		return 1;
	if (R_FAILED(svcQueryMemory(&information, &page_information, address)) || !(information.perm & Perm_R))
		return 0;
	region->start = information.addr;
	region->end = information.addr + information.size;
	return address + 16 <= region->end;
}

/* ---------- the sampler of hangs */

static void sample(Handle thread)
{
	struct readable_region region = { 0, 0 };
	ThreadContext context;
	char line[512];
	int length, depth;
	uint64_t fp;

	if (R_FAILED(svcSetThreadActivity(thread, ThreadActivity_Paused)))
		return;
	memset(&context, 0, sizeof(context));
	if (R_SUCCEEDED(svcGetThreadContext3(&context, thread)))
	{
		fp = context.fp;
		length = snprintf(line, sizeof(line), "sample thread %x: pc %llx lr %llx", (unsigned)thread,
			(unsigned long long)context.pc.x, (unsigned long long)context.lr);
		for (depth = 0; depth < 12 && frame_readable(&region, fp) && length < (int)sizeof(line) - 20; depth++)
		{
			const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

			length += snprintf(line + length, sizeof(line) - (size_t)length, " %llx", (unsigned long long)frame[1]);
			if (frame[0] <= fp)
				break;
			fp = frame[0];
		}
	}
	else
	{
		snprintf(line, sizeof(line), "sample thread %x: no context", (unsigned)thread);
	}
	svcSetThreadActivity(thread, ThreadActivity_Runnable);
	/* (after the thread runs again: it may hold the log's lock) */
	host_logf(HOST_LOG_INFO, "%s", line);
}

static void sampler(void *context)
{
	u64 nanoseconds = (u64)(uintptr_t)context;

	for (;;)
	{
		Handle handles[MAXIMUM_THREADS];
		int index;

		svcSleepThread((s64)nanoseconds);
		mutexLock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
			handles[index] = threads[index].handle;
		mutexUnlock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (handles[index])
				sample(handles[index]);
		}
	}
}

/* a thread of the host's own, ahead of the threads it samples (so that it
runs when they do not stop) and off the main thread's core */
static int debug_thread_start(Thread *thread, ThreadFunc function, void *argument)
{
	s32 priority = 0x2c;

	if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) && priority > 30)
		priority -= 3;
	if (R_FAILED(threadCreate(thread, function, argument, NULL, 0x10000, priority, host_thread_other_core())))
		return 0;
	if (R_FAILED(threadStart(thread)))
	{
		threadClose(thread);
		return 0;
	}
	return 1;
}

void host_debug_start_sampler(double seconds)
{
	if (seconds < 0.05)
		return;
	if (!debug_thread_start(&sampler_thread, sampler, (void *)(uintptr_t)(u64)(seconds * 1000000000.0)))
	{
		host_logf(HOST_LOG_WARN, "the sampler's thread cannot start");
		return;
	}
	host_logf(HOST_LOG_INFO, "sampling guest threads every %g s", seconds);
}

/* ---------- the profile */

/* nanoseconds between samples, and samples in a line of the log */
#define PROFILE_INTERVAL 4000000ll
#define PROFILE_WINDOW 500
/* the share of a core the main thread uses in a busy window, in percent */
#define PROFILE_BUSY 70
/* busy windows between writes of the file, and the lines of it */
#define PROFILE_WRITE_WINDOWS 10
#define PROFILE_LINES 60000
/* the file is megabytes, and each write of it to the card takes
milliseconds: this much at a time */
#define PROFILE_FILE_BUFFER (512 * 1024)
/* the game's outermost callers kept, of the main thread, and the program's
own, of a thread that has none of the game's */
#define PROFILE_OUTER 8
#define PROFILE_HOST_OUTER 4
/* the game's callers kept above the nearest, of a thread in a system call */
#define PROFILE_CHAIN 7
/* frames this long are slow ones, in milliseconds */
#define PROFILE_SLOW_FRAME 25

#define WINDOW_ENTRIES 8192
#define TOTAL_ENTRIES 65536

int host_profile_enabled;

static struct
{
	volatile u64 count, ticks, amount;
} counts[NUMBER_OF_HOST_PROFILE_COUNTS];

/* the frames (the thread that draws writes these; the profile's reads them) */
static volatile u64 frame_last, frame_longest;
static volatile u32 frame_count, frame_slow;

void host_profile_add(int which, uint64_t ticks, uint64_t amount)
{
	if (!host_profile_enabled)
		return;
	__sync_fetch_and_add(&counts[which].count, 1);
	__sync_fetch_and_add(&counts[which].ticks, ticks);
	__sync_fetch_and_add(&counts[which].amount, amount);
}

void host_profile_frame(uint64_t start, uint64_t end)
{
	if (!host_profile_enabled)
		return;
	if (frame_last)
	{
		u64 interval = start - frame_last;

		if (interval > frame_longest)
			frame_longest = interval;
		if (armTicksToNs(interval) > PROFILE_SLOW_FRAME * 1000000ull)
			frame_slow++;
	}
	frame_last = start;
	frame_count++;
	host_profile_add(_host_profile_swap, end - start, 0);
}

/* what a sample was in */
enum
{
	_sample_game,
	_sample_program,
	_sample_service,
	_sample_waiting,
	_sample_kernel,
	_sample_other,
	NUMBER_OF_SAMPLE_KINDS,
};

struct profile_entry
{
	/* the program counter, or the number of the system call it was in with
	SAMPLE_CALL; the address that made the call; the game's caller */
	u64 where, from, caller;
	/* (in a system call) the game's callers above that one, nearest first */
	u64 chain[PROFILE_CHAIN];
	/* (the main thread) the game's outermost callers, the first first; (a
	thread with none) the program's */
	u64 outer[PROFILE_OUTER];
	u32 count;
	u16 thread;
	u16 used;
};

#define SAMPLE_CALL 0x8000000000000000ull

static struct profile_entry window_entries[WINDOW_ENTRIES];
static struct profile_entry total_entries[TOTAL_ENTRIES];
static struct profile_entry *sorted_entries[TOTAL_ENTRIES];
static u32 window_kinds[MAXIMUM_THREADS][NUMBER_OF_SAMPLE_KINDS];
static u32 total_samples, total_dropped, windows_seen, windows_busy;
static char profile_path[640];
static Thread profile_thread;

/* the program's own image: its first instruction, which is where it was
loaded, and its end (libnx's linker script) */
extern void _start(void);
extern const char __end__[];

static u64 program_base(void)
{
	return (u64)(uintptr_t)_start;
}

static int in_game(u64 address)
{
	return address >= host_image.base && address < host_image.end;
}

static int in_program(u64 address)
{
	return address >= program_base() && address < (u64)(uintptr_t)__end__;
}

static void entry_add(struct profile_entry *table, u32 size, const struct profile_entry *sample)
{
	u64 hash = (sample->where * 0x9e3779b97f4a7c15ull) ^ (sample->from * 0xc2b2ae3d27d4eb4full) ^
		(sample->caller * 0x165667b19e3779f9ull) ^ sample->thread;
	u32 slot, probe;
	int link;

	for (link = 0; link < PROFILE_CHAIN; link++)
		hash = (hash ^ sample->chain[link]) * 0x100000001b3ull;
	for (link = 0; link < PROFILE_OUTER; link++)
		hash = (hash ^ sample->outer[link]) * 0x100000001b3ull;
	slot = (u32)(hash >> 40) % size;

	for (probe = 0; probe < 64; probe++, slot = (slot + 1) % size)
	{
		struct profile_entry *entry = &table[slot];

		if (!entry->used)
		{
			*entry = *sample;
			entry->used = 1;
			return;
		}
		if (entry->where == sample->where && entry->from == sample->from && entry->caller == sample->caller &&
			entry->thread == sample->thread && !memcmp(entry->chain, sample->chain, sizeof(entry->chain)) &&
			!memcmp(entry->outer, sample->outer, sizeof(entry->outer)))
		{
			entry->count += sample->count;
			return;
		}
	}
	total_dropped += sample->count;
}

/* the kind of a thread stopped in a system call */
static int call_kind(u32 number)
{
	switch (number)
	{
	case 0x0b: /* SleepThread */
	case 0x18: /* WaitSynchronization */
	case 0x1a: /* ArbitrateLock */
	case 0x1c: /* WaitProcessWideKeyAtomic */
	case 0x34: /* WaitForAddress */
		return _sample_waiting;
	case 0x21: /* SendSyncRequest */
	case 0x22: /* SendSyncRequestWithUserBuffer */
		return _sample_service;
	default:
		return _sample_kernel;
	}
}

static void profile_sample(int index, Handle handle)
{
	struct readable_region region = { 0, 0 };
	ThreadContext context;
	struct profile_entry entry;
	u64 pc, fp;
	/* (the last of the game's callers found, for the outermost of them, and
	of the program's own) */
	u64 last[PROFILE_OUTER], host_last[PROFILE_HOST_OUTER];
	int kind, depth, in_call = 0, links = 0, found = 0, host_found = 0, whole = index == 0;

	if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused)))
		return;
	memset(&context, 0, sizeof(context));
	memset(&entry, 0, sizeof(entry));
	if (R_FAILED(svcGetThreadContext3(&context, handle)))
	{
		svcSetThreadActivity(handle, ThreadActivity_Runnable);
		return;
	}
	pc = context.pc.x;
	entry.thread = (u16)index;
	entry.count = 1;
	entry.where = pc;
	kind = in_game(pc) ? _sample_game : in_program(pc) ? _sample_program : _sample_other;
	/* stopped in a system call: the kernel gives the address of the call
	itself while the call has yet to return, and the one after it once it
	has */
	if (kind != _sample_other && !(pc & 3) && pc >= 4)
	{
		u32 instruction = *(const u32 *)(uintptr_t)pc;

		if ((instruction & 0xffe0001f) != 0xd4000001)
			instruction = *(const u32 *)(uintptr_t)(pc - 4);
		if ((instruction & 0xffe0001f) == 0xd4000001)
		{
			u32 number = (instruction >> 5) & 0xffff;

			kind = call_kind(number);
			entry.where = SAMPLE_CALL | number;
			entry.from = context.lr;
			in_call = 1;
		}
	}
	/* the game's function that this is for: the nearest return address in
	the game's image, in the frame chain (the thread's stack is the guest's)
	or else the link register; the main thread's chain is followed to its
	end, for the outermost */
	if (!in_game(pc) || whole)
	{
		fp = context.fp;
		for (depth = 0; depth < 160 && frame_readable(&region, fp); depth++)
		{
			const u64 *frame = (const u64 *)(uintptr_t)fp;

			if (in_program(frame[1]))
				host_last[host_found++ % PROFILE_HOST_OUTER] = frame[1];
			if (in_game(frame[1]))
			{
				last[found++ % PROFILE_OUTER] = frame[1];
				if (!entry.caller && !in_game(pc))
					entry.caller = frame[1];
				else if (links < PROFILE_CHAIN && (in_call || in_game(pc)))
					entry.chain[links++] = frame[1];
				if (!whole && (!in_call || links == PROFILE_CHAIN))
					break;
			}
			if (frame[0] <= fp)
				break;
			fp = frame[0];
		}
		if (!entry.caller && !in_game(pc) && in_game(context.lr))
			entry.caller = context.lr;
		if (whole)
		{
			int kept = found < PROFILE_OUTER ? found : PROFILE_OUTER, link;

			/* (the last found is the outermost) */
			for (link = 0; link < kept; link++)
				entry.outer[link] = last[(found - 1 - link) % PROFILE_OUTER];
		}
		else if (!found && !in_game(context.lr))
		{
			/* none of the game's code on this thread's stack */
			int kept = host_found < PROFILE_HOST_OUTER ? host_found : PROFILE_HOST_OUTER, link;

			for (link = 0; link < kept; link++)
				entry.outer[link] = host_last[(host_found - 1 - link) % PROFILE_HOST_OUTER];
		}
	}
	svcSetThreadActivity(handle, ThreadActivity_Runnable);
	window_kinds[index][kind]++;
	entry_add(window_entries, WINDOW_ENTRIES, &entry);
}

static u64 thread_ticks(Handle handle)
{
	u64 ticks = 0;

	if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
		R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total)))
	{
		return 0;
	}
	return ticks;
}

static int compare_entries(const void *a, const void *b)
{
	const struct profile_entry *first = *(const struct profile_entry *const *)a;
	const struct profile_entry *second = *(const struct profile_entry *const *)b;

	return first->count < second->count ? 1 : first->count > second->count ? -1 : 0;
}

static void print_address(FILE *file, u64 address)
{
	if (address & SAMPLE_CALL)
		fprintf(file, " svc%02x", (unsigned)(address & 0xffff));
	else if (!address)
		fprintf(file, " -");
	else if (in_game(address))
		fprintf(file, " g:%llx", (unsigned long long)address);
	else if (in_program(address))
		fprintf(file, " h+%llx", (unsigned long long)(address - program_base()));
	else
		fprintf(file, " x:%llx", (unsigned long long)address);
}

static void profile_write(const char names[MAXIMUM_THREADS][THREAD_NAME_SIZE])
{
	FILE *file = fopen(profile_path, "w");
	u32 count = 0, index;
	int link;

	if (!file)
		return;
	setvbuf(file, NULL, _IOFBF, PROFILE_FILE_BUFFER);
	for (index = 0; index < TOTAL_ENTRIES; index++)
	{
		if (total_entries[index].used)
			sorted_entries[count++] = &total_entries[index];
	}
	qsort(sorted_entries, count, sizeof(*sorted_entries), compare_entries);
	fprintf(file, "OpenCE %s profile: %u samples of %u busy windows (of %u), a sample each %d ms; %u not kept\n",
		HALO_SWITCH_VERSION, (unsigned)total_samples, (unsigned)windows_busy, (unsigned)windows_seen,
		(int)(PROFILE_INTERVAL / 1000000), (unsigned)total_dropped);
	fprintf(file, "game %llx-%llx program %llx-%llx\n", (unsigned long long)host_image.base,
		(unsigned long long)host_image.end, (unsigned long long)program_base(),
		(unsigned long long)(uintptr_t)__end__);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (names[index][0])
			fprintf(file, "thread %u %s\n", (unsigned)index, names[index]);
	}
	for (index = 0; index < count && index < PROFILE_LINES; index++)
	{
		const struct profile_entry *entry = sorted_entries[index];

		fprintf(file, "%u", (unsigned)entry->thread);
		print_address(file, entry->where);
		print_address(file, entry->from);
		print_address(file, entry->caller);
		fprintf(file, " %u", (unsigned)entry->count);
		for (link = 0; link < PROFILE_CHAIN && entry->chain[link]; link++)
			print_address(file, entry->chain[link]);
		if (entry->outer[0])
			fprintf(file, " /");
		for (link = 0; link < PROFILE_OUTER && entry->outer[link]; link++)
			print_address(file, entry->outer[link]);
		fprintf(file, "\n");
	}
	fclose(file);
}

static double milliseconds(u64 ticks)
{
	return (double)armTicksToNs(ticks) / 1000000.0;
}

/* what the counts were at the last window's end */
static u64 last_counts[NUMBER_OF_HOST_PROFILE_COUNTS][3];

/* Writing the file takes seconds, in which nothing is sampled: the window
after it starts from the counts as they are then, not as they were before
(a window that had those seconds' frames and time in it showed several
hundred frames a second and threads at several hundred percent). */
static void profile_start_again(void)
{
	int index, which;

	for (which = 0; which < NUMBER_OF_HOST_PROFILE_COUNTS; which++)
	{
		last_counts[which][0] = counts[which].count;
		last_counts[which][1] = counts[which].ticks;
		last_counts[which][2] = counts[which].amount;
	}
	__sync_lock_test_and_set(&frame_count, 0);
	__sync_lock_test_and_set(&frame_slow, 0);
	__sync_lock_test_and_set(&frame_longest, 0);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		Handle handle;
		u64 ticks;

		mutexLock(&threads_lock);
		handle = threads[index].handle;
		mutexUnlock(&threads_lock);
		if (!handle)
			continue;
		ticks = thread_ticks(handle);
		mutexLock(&threads_lock);
		if (threads[index].handle == handle)
			threads[index].ticks = ticks;
		mutexUnlock(&threads_lock);
	}
}

static void profile_window(u64 window_ticks)
{
	static char names[MAXIMUM_THREADS][THREAD_NAME_SIZE];
	static u32 unwritten;
	Handle handles[MAXIMUM_THREADS];
	double per_second = (double)armGetSystemTickFreq() / (double)(window_ticks ? window_ticks : 1);
	double delta[NUMBER_OF_HOST_PROFILE_COUNTS][3];
	char line[1000];
	u32 frames = __sync_lock_test_and_set(&frame_count, 0), slow = __sync_lock_test_and_set(&frame_slow, 0);
	u64 longest = __sync_lock_test_and_set(&frame_longest, 0);
	u32 main_samples = 0;
	int length, index, which, kind, main_share = 0, busy;

	for (which = 0; which < NUMBER_OF_HOST_PROFILE_COUNTS; which++)
	{
		u64 now[3] = { counts[which].count, counts[which].ticks, counts[which].amount };

		for (index = 0; index < 3; index++)
		{
			delta[which][index] = (double)(now[index] - last_counts[which][index]) * per_second;
			last_counts[which][index] = now[index];
		}
	}
	for (kind = 0; kind < NUMBER_OF_SAMPLE_KINDS; kind++)
		main_samples += window_kinds[0][kind];
	if (!main_samples)
		main_samples = 1;
	length = snprintf(line, sizeof(line), "profile: %.0f frames/s, longest %.0f ms, %u slow;",
		(double)frames * per_second, milliseconds(longest), (unsigned)slow);

	/* each thread's share of a core */
	mutexLock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		handles[index] = threads[index].handle;
		if (handles[index])
			memcpy(names[index], threads[index].name, sizeof(names[index]));
	}
	mutexUnlock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		u64 ticks;
		int share;

		if (!handles[index])
			continue;
		ticks = thread_ticks(handles[index]);
		mutexLock(&threads_lock);
		share = threads[index].handle == handles[index] && threads[index].ticks && window_ticks ?
			(int)((ticks - threads[index].ticks) * 100 / window_ticks) : 0;
		if (threads[index].handle == handles[index])
			threads[index].ticks = ticks;
		mutexUnlock(&threads_lock);
		if (!index)
			main_share = share;
		if ((share > 0 || !index) && length < (int)sizeof(line) - 40)
			length += snprintf(line + length, sizeof(line) - (size_t)length, " %s %d%%", names[index], share);
	}
	length += snprintf(line + length, sizeof(line) - (size_t)length,
		"; main in game %u%% program %u%% services %u%% kernel %u%% waiting %u%% other %u%%;"
		" swap %.0f ms/s, GPU wait %.0f ms/s and %.0f ms/s in maps; faults %.0f/s (%.1f ms/s),"
		" protects %.0f/s of %.0f pages (%.1f ms/s);"
		" reads %.0f/s of %.0f KB (%.0f ms/s); futex waits %.0f/s; mixer %.0f ms/s",
		(unsigned)(window_kinds[0][_sample_game] * 100 / main_samples),
		(unsigned)(window_kinds[0][_sample_program] * 100 / main_samples),
		(unsigned)(window_kinds[0][_sample_service] * 100 / main_samples),
		(unsigned)(window_kinds[0][_sample_kernel] * 100 / main_samples),
		(unsigned)(window_kinds[0][_sample_waiting] * 100 / main_samples),
		(unsigned)(window_kinds[0][_sample_other] * 100 / main_samples),
		milliseconds((u64)delta[_host_profile_swap][1]), milliseconds((u64)delta[_host_profile_gpu_wait][1]),
		milliseconds((u64)delta[_host_profile_gpu_map][1]),
		delta[_host_profile_fault][0], milliseconds((u64)delta[_host_profile_fault][1]),
		delta[_host_profile_protect][0], delta[_host_profile_protect][2], milliseconds((u64)delta[_host_profile_protect][1]),
		delta[_host_profile_read][0], delta[_host_profile_read][2] / 1024.0, milliseconds((u64)delta[_host_profile_read][1]),
		delta[_host_profile_futex][0], milliseconds((u64)delta[_host_profile_audio][1]));
	/* the render thread: what the game's thread queued for it, and what it
	waited for (calls it needs the result of, room, the frame before) */
	if (host_render_remote() && length < (int)sizeof(line) - 160)
	{
		length += snprintf(line + length, sizeof(line) - (size_t)length,
			"; render: %.0f calls/s of %.0f KB, waits for %.0f calls/s (%.1f ms/s), room %.1f ms/s, frames %.1f ms/s",
			delta[_host_profile_render_queued][1], delta[_host_profile_render_queued][2] / 1024.0,
			delta[_host_profile_render_wait][0], milliseconds((u64)delta[_host_profile_render_wait][1]),
			milliseconds((u64)delta[_host_profile_render_room][1]),
			milliseconds((u64)delta[_host_profile_render_frame][1]));
	}
	(void)length;
	host_logf(HOST_LOG_INFO, "%s", line);

	/* the samples of a busy window are kept: one in which the main thread
	had little time left, or a frame was late */
	windows_seen++;
	busy = main_share >= PROFILE_BUSY || slow > 0;
	for (index = 0; index < WINDOW_ENTRIES; index++)
	{
		if (window_entries[index].used && busy)
		{
			total_samples += window_entries[index].count;
			entry_add(total_entries, TOTAL_ENTRIES, &window_entries[index]);
		}
	}
	memset(window_entries, 0, sizeof(window_entries));
	memset(window_kinds, 0, sizeof(window_kinds));
	if (busy)
	{
		windows_busy++;
		if (++unwritten >= PROFILE_WRITE_WINDOWS)
		{
			unwritten = 0;
			profile_write(names);
			profile_start_again();
		}
	}
}

static void profiler(void *unused)
{
	u64 window_start = armGetSystemTick();
	int samples = 0;

	(void)unused;
	for (;;)
	{
		Handle handles[MAXIMUM_THREADS];
		int index;

		svcSleepThread(PROFILE_INTERVAL);
		mutexLock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
			handles[index] = threads[index].handle;
		mutexUnlock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (handles[index])
				profile_sample(index, handles[index]);
		}
		if (++samples >= PROFILE_WINDOW)
		{
			u64 now = armGetSystemTick();

			samples = 0;
			profile_window(now - window_start);
			window_start = armGetSystemTick();
		}
	}
}

void host_profile_start(const char *directory)
{
	snprintf(profile_path, sizeof(profile_path), "%s/profile.txt", directory);
	host_profile_enabled = 1;
	if (!debug_thread_start(&profile_thread, profiler, NULL))
	{
		host_profile_enabled = 0;
		host_logf(HOST_LOG_WARN, "the profile's thread cannot start");
		return;
	}
	host_logf(HOST_LOG_INFO, "profile: a sample of each thread every %d ms, a line here every %d s, %s for the busy ones",
		(int)(PROFILE_INTERVAL / 1000000), (int)(PROFILE_INTERVAL * PROFILE_WINDOW / 1000000000), profile_path);
}
