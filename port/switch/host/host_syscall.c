/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here).

The guest's C library is written for Linux, which Horizon is not: each call
the guest makes is done here with libnx and its C library, and answered as
Linux would. That means

- Linux's error numbers, which are not this C library's above ERANGE;
- Linux's structures, in the guest's ILP32 layout: timespec (the guest's
  time_t is 32-bit, as in the MSVC runtime of the game), iovec, the kernel's
  stat and directory entries;
- futexes, which musl's locks, condition variables and semaphores are made
  of, with Horizon's address arbiter (the same idea: sleep while a word in
  memory has a value);
- memory mappings, which must stay below 4 GB (host_memory.c);
- the standard output and error streams, which go to the log;
- process exit, and calls that have no meaning for the guest (signals).
*/

#include "host.h"
#include "guest_syscall_numbers.h"

#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));

/* ---------- Linux's error numbers */

#define GUEST_ENAMETOOLONG 36
#define GUEST_ENOSYS 38
#define GUEST_ENOTEMPTY 39
#define GUEST_ELOOP 40
#define GUEST_EOVERFLOW 75
#define GUEST_EILSEQ 84
#define GUEST_ENOTSOCK 88
#define GUEST_EDESTADDRREQ 89
#define GUEST_EMSGSIZE 90
#define GUEST_EPROTOTYPE 91
#define GUEST_ENOPROTOOPT 92
#define GUEST_EPROTONOSUPPORT 93
#define GUEST_EOPNOTSUPP 95
#define GUEST_EAFNOSUPPORT 97
#define GUEST_EADDRINUSE 98
#define GUEST_EADDRNOTAVAIL 99
#define GUEST_ENETDOWN 100
#define GUEST_ENETUNREACH 101
#define GUEST_ENETRESET 102
#define GUEST_ECONNABORTED 103
#define GUEST_ECONNRESET 104
#define GUEST_ENOBUFS 105
#define GUEST_EISCONN 106
#define GUEST_ENOTCONN 107
#define GUEST_ETIMEDOUT 110
#define GUEST_ECONNREFUSED 111
#define GUEST_EHOSTUNREACH 113
#define GUEST_EALREADY 114
#define GUEST_EINPROGRESS 115

/* the error number Linux has for one of this C library's */
static int guest_errno(int error)
{
	/* (the first 34, to ERANGE, are everyone's) */
	if (error >= 0 && error <= ERANGE)
		return error;
	switch (error)
	{
	case ENAMETOOLONG: return GUEST_ENAMETOOLONG;
	case ENOSYS: return GUEST_ENOSYS;
	case ENOTEMPTY: return GUEST_ENOTEMPTY;
	case ELOOP: return GUEST_ELOOP;
	case EOVERFLOW: return GUEST_EOVERFLOW;
	case EILSEQ: return GUEST_EILSEQ;
	case ENOTSOCK: return GUEST_ENOTSOCK;
	case EDESTADDRREQ: return GUEST_EDESTADDRREQ;
	case EMSGSIZE: return GUEST_EMSGSIZE;
	case EPROTOTYPE: return GUEST_EPROTOTYPE;
	case ENOPROTOOPT: return GUEST_ENOPROTOOPT;
	case EPROTONOSUPPORT: return GUEST_EPROTONOSUPPORT;
	case EOPNOTSUPP: return GUEST_EOPNOTSUPP;
	case EAFNOSUPPORT: return GUEST_EAFNOSUPPORT;
	case EADDRINUSE: return GUEST_EADDRINUSE;
	case EADDRNOTAVAIL: return GUEST_EADDRNOTAVAIL;
	case ENETDOWN: return GUEST_ENETDOWN;
	case ENETUNREACH: return GUEST_ENETUNREACH;
	case ENETRESET: return GUEST_ENETRESET;
	case ECONNABORTED: return GUEST_ECONNABORTED;
	case ECONNRESET: return GUEST_ECONNRESET;
	case ENOBUFS: return GUEST_ENOBUFS;
	case EISCONN: return GUEST_EISCONN;
	case ENOTCONN: return GUEST_ENOTCONN;
	case ETIMEDOUT: return GUEST_ETIMEDOUT;
	case ECONNREFUSED: return GUEST_ECONNREFUSED;
	case EHOSTUNREACH: return GUEST_EHOSTUNREACH;
	case EALREADY: return GUEST_EALREADY;
	case EINPROGRESS: return GUEST_EINPROGRESS;
	default: return EIO;
	}
}

/* the host's errno on this thread, after a call to a host function, as the
guest's C library numbers it (the generated posix_* wrappers copy it) */
int host_errno(void)
{
	return guest_errno(errno);
}

static long failure(void)
{
	return -(long)guest_errno(errno);
}

static long result_of(long value)
{
	return value == -1 ? failure() : value;
}

/* ---------- the guest's structures */

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* the AArch64 kernel's struct stat (the guest's arch/arm64_32/kstat.h) */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t padding;
	int64_t st_size;
	int32_t st_blksize;
	int32_t padding2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

/* the kernel's directory entry (getdents64) */
struct guest_dirent
{
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

/* ---------- time */

#define GUEST_CLOCK_REALTIME 0
#define GUEST_CLOCK_REALTIME_COARSE 5
#define GUEST_TIMER_ABSTIME 1

/* the wall clock when the program started, and the system's tick then: the
time of day without a call to the system each time */
static uint64_t real_time_base, real_time_tick;

static int64_t monotonic_nanoseconds(void)
{
	return (int64_t)armTicksToNs(armGetSystemTick());
}

static int64_t clock_nanoseconds(int clock)
{
	if (clock == GUEST_CLOCK_REALTIME || clock == GUEST_CLOCK_REALTIME_COARSE)
	{
		if (!real_time_base)
		{
			u64 now = 0;

			if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &now)) || !now)
				now = (u64)time(NULL);
			real_time_tick = armGetSystemTick();
			real_time_base = now;
		}
		return (int64_t)real_time_base * 1000000000LL + (int64_t)armTicksToNs(armGetSystemTick() - real_time_tick);
	}
	return monotonic_nanoseconds();
}

/* nanoseconds, or -1 where there is no timespec */
static int64_t timespec_in(uint64_t address)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return -1;
	if (value->seconds < 0)
		return 0;
	return (int64_t)value->seconds * 1000000000LL + value->nanoseconds;
}

static void timespec_out(uint64_t address, int64_t nanoseconds)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)(nanoseconds / 1000000000LL);
	result->nanoseconds = (int32_t)(nanoseconds % 1000000000LL);
}

static void sleep_nanoseconds(int64_t nanoseconds)
{
	/* (zero and less mean kinds of yield to the kernel) */
	if (nanoseconds > 0)
		svcSleepThread(nanoseconds);
	else
		svcSleepThread(YieldType_WithCoreMigration);
}

/* ---------- futexes */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_COMMAND_MASK 127

static long futex_wait(uint64_t address, uint32_t value, int64_t timeout)
{
	u64 start = armGetSystemTick();
	Result result = svcWaitForAddress(GUEST(void *, address), ArbitrationType_WaitIfEqual, (s32)value, timeout);

	host_profile_add(_host_profile_futex, armGetSystemTick() - start, 0);
	if (R_SUCCEEDED(result))
		return 0;
	if (result == KERNELRESULT(TimedOut))
		return -GUEST_ETIMEDOUT;
	/* the word no longer has the value */
	if (result == KERNELRESULT(InvalidState))
		return -EAGAIN;
	if (result == KERNELRESULT(Cancelled))
		return -EINTR;
	return -EFAULT;
}

static long futex_wake(uint64_t address, int64_t count)
{
	/* (none or less is everyone, to the kernel) */
	if (count <= 0)
		return 0;
	svcSignalToAddress(GUEST(void *, address), SignalType_Signal, 0, count > 0x7fffffff ? 0x7fffffff : (s32)count);
	return 0;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t fourth, uint64_t address2)
{
	(void)address2;
	switch (operation & FUTEX_COMMAND_MASK)
	{
	case FUTEX_WAIT:
		/* for as long as the timespec says */
		return futex_wait(address, value, timespec_in(fourth));
	case FUTEX_WAIT_BITSET:
	{
		/* until the time the timespec says */
		int64_t until = timespec_in(fourth);

		if (until >= 0)
		{
			until -= clock_nanoseconds(operation & FUTEX_CLOCK_REALTIME ? GUEST_CLOCK_REALTIME : 1);
			if (until < 0)
				until = 0;
		}
		return futex_wait(address, value, until);
	}
	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		return futex_wake(address, (int32_t)value);
	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE:
		/* wakes some waiters and has the rest wait on another word, to be
		woken through it later. The arbiter cannot move a waiter, so those
		are woken now instead: early, which every futex wait allows for
		(musl's condition variables requeue one waiter to its mutex, which
		it then waits for as any thread does) */
		return futex_wake(address, (int64_t)(int32_t)value + (int64_t)(int32_t)(uint32_t)fourth);
	default:
		return -GUEST_ENOSYS;
	}
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static Mutex log_lock;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	mutexLock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_log(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		if (c != '\r')
			stream->line[stream->length++] = c;
	}
	mutexUnlock(&log_lock);
}

/* ---------- files */

#define FILE_LOCK_COUNT 64

static Mutex file_locks[FILE_LOCK_COUNT];

void host_file_lock(int descriptor)
{
	mutexLock(&file_locks[(unsigned int)descriptor % FILE_LOCK_COUNT]);
}

void host_file_unlock(int descriptor)
{
	mutexUnlock(&file_locks[(unsigned int)descriptor % FILE_LOCK_COUNT]);
}

static long guest_read(int fd, void *buffer, size_t size)
{
	long result;

	host_file_lock(fd);
	result = result_of(read(fd, buffer, size));
	host_file_unlock(fd);
	return result;
}

static long guest_write(int fd, const void *buffer, size_t size)
{
	long result;

	host_file_lock(fd);
	result = result_of(write(fd, buffer, size));
	host_file_unlock(fd);
	return result;
}

static long guest_seek(int fd, off_t offset, int whence)
{
	long result;

	host_file_lock(fd);
	result = result_of((long)lseek(fd, offset, whence));
	host_file_unlock(fd);
	return result;
}

/* the guest's open flags (its bits/fcntl.h) */
#define GUEST_O_ACCMODE 03
#define GUEST_O_CREAT 0100
#define GUEST_O_EXCL 0200
#define GUEST_O_TRUNC 01000
#define GUEST_O_APPEND 02000
#define GUEST_O_DIRECTORY 040000

#define GUEST_AT_FDCWD (-100)
#define GUEST_AT_REMOVEDIR 0x200

/* directories the guest has open: its descriptors for them, from here up */
#define DIRECTORY_DESCRIPTOR 0x4000
#define DIRECTORY_COUNT 32

struct guest_directory
{
	DIR *stream;
	long position;
	/* an entry read that the guest's buffer had no room for */
	int pending;
	unsigned char pending_type;
	char pending_name[NAME_MAX + 1];
};

static struct guest_directory directories[DIRECTORY_COUNT];
static Mutex directory_lock;

static struct guest_directory *directory_of(int fd)
{
	if (fd < DIRECTORY_DESCRIPTOR || fd >= DIRECTORY_DESCRIPTOR + DIRECTORY_COUNT)
		return NULL;
	return directories[fd - DIRECTORY_DESCRIPTOR].stream ? &directories[fd - DIRECTORY_DESCRIPTOR] : NULL;
}

/* a path relative to a directory descriptor: only the current directory's
are (the guest's C library has no other use of them) */
static const char *path_at(int directory, uint64_t path)
{
	const char *text = GUEST(const char *, path);

	if (!text || (directory != GUEST_AT_FDCWD && text[0] != '/'))
		return NULL;
	return text;
}

static long guest_open(int directory, uint64_t path, int flags, int mode)
{
	const char *text = path_at(directory, path);
	int host_flags = flags & GUEST_O_ACCMODE;

	if (!text)
		return -EBADF;
	if (flags & GUEST_O_DIRECTORY)
	{
		DIR *stream = opendir(text);
		int index;

		if (!stream)
			return failure();
		mutexLock(&directory_lock);
		for (index = 0; index < DIRECTORY_COUNT; index++)
		{
			if (!directories[index].stream)
			{
				memset(&directories[index], 0, sizeof(directories[index]));
				directories[index].stream = stream;
				break;
			}
		}
		mutexUnlock(&directory_lock);
		if (index == DIRECTORY_COUNT)
		{
			closedir(stream);
			return -EMFILE;
		}
		return DIRECTORY_DESCRIPTOR + index;
	}
	if (flags & GUEST_O_CREAT)
		host_flags |= O_CREAT;
	if (flags & GUEST_O_EXCL)
		host_flags |= O_EXCL;
	if (flags & GUEST_O_TRUNC)
		host_flags |= O_TRUNC;
	if (flags & GUEST_O_APPEND)
		host_flags |= O_APPEND;
	return result_of(open(text, host_flags, mode));
}

static long guest_close(int fd)
{
	struct guest_directory *directory = directory_of(fd);

	if (directory)
	{
		DIR *stream = directory->stream;

		mutexLock(&directory_lock);
		directory->stream = NULL;
		mutexUnlock(&directory_lock);
		return result_of(closedir(stream));
	}
	/* (standard input, output and error stay the host's) */
	if (fd >= 0 && fd <= 2)
		return 0;
	return result_of(close(fd));
}

static void stat_out(const struct stat *host, struct guest_kstat *guest)
{
	memset(guest, 0, sizeof(*guest));
	guest->st_dev = (uint64_t)host->st_dev;
	guest->st_ino = (uint64_t)host->st_ino;
	guest->st_mode = (uint32_t)host->st_mode;
	guest->st_nlink = host->st_nlink ? (uint32_t)host->st_nlink : 1;
	guest->st_size = (int64_t)host->st_size;
	guest->st_blksize = 0x1000;
	guest->st_blocks = ((int64_t)host->st_size + 511) / 512;
	guest->st_atime_sec = (int64_t)host->st_atime;
	guest->st_mtime_sec = (int64_t)host->st_mtime;
	guest->st_ctime_sec = (int64_t)host->st_ctime;
}

static long guest_stat(int directory, uint64_t path, uint64_t result)
{
	const char *text = path_at(directory, path);
	struct stat information;

	if (!text)
		return -EBADF;
	if (stat(text, &information) != 0)
		return failure();
	stat_out(&information, GUEST(struct guest_kstat *, result));
	return 0;
}

static long guest_fstat(int fd, uint64_t result)
{
	struct guest_kstat *guest = GUEST(struct guest_kstat *, result);
	struct stat information;

	if (directory_of(fd))
	{
		memset(guest, 0, sizeof(*guest));
		guest->st_mode = S_IFDIR | 0777;
		guest->st_nlink = 1;
		return 0;
	}
	if (fd >= 0 && fd <= 2)
	{
		memset(guest, 0, sizeof(*guest));
		guest->st_mode = S_IFCHR | 0666;
		guest->st_nlink = 1;
		return 0;
	}
	if (fstat(fd, &information) != 0)
		return failure();
	stat_out(&information, guest);
	return 0;
}

static long guest_getdents(int fd, uint64_t buffer, uint32_t size)
{
	struct guest_directory *directory = directory_of(fd);
	char *out = GUEST(char *, buffer);
	uint32_t used = 0;

	if (!directory)
		return -EBADF;
	for (;;)
	{
		struct guest_dirent *guest = (struct guest_dirent *)(out + used);
		size_t length, record;

		if (!directory->pending)
		{
			struct dirent *entry = readdir(directory->stream);

			if (!entry)
				break;
			snprintf(directory->pending_name, sizeof(directory->pending_name), "%s", entry->d_name);
			directory->pending_type = entry->d_type;
			directory->pending = 1;
		}
		length = strlen(directory->pending_name);
		record = (offsetof(struct guest_dirent, d_name) + length + 1 + 7) & ~(size_t)7;
		/* (an entry that does not fit is the next call's) */
		if (used + record > size)
			return used ? (long)used : -EINVAL;
		memset(guest, 0, record);
		guest->d_ino = 1;
		guest->d_off = (int64_t)++directory->position;
		guest->d_reclen = (uint16_t)record;
		/* (Linux's DT_DIR and DT_REG) */
		guest->d_type = directory->pending_type == DT_DIR ? 4 : 8;
		memcpy(guest->d_name, directory->pending_name, length);
		directory->pending = 0;
		used += (uint32_t)record;
	}
	return used;
}

/* (output streams are the log's; the others' vectors one after another, as
far as each is written or read in full) */
static long guest_vector(int fd, uint64_t vector, int count, int writing)
{
	const struct guest_iovec *vectors = GUEST(const struct guest_iovec *, vector);
	int logged = writing && (fd == 1 || fd == 2);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -EINVAL;
	if (!logged)
		host_file_lock(fd);
	for (index = 0; index < count; index++)
	{
		void *base = GUEST(void *, vectors[index].base);
		size_t length = vectors[index].length;
		ssize_t done;

		if (!length)
			continue;
		if (logged)
		{
			log_bytes(fd, base, length);
			done = (ssize_t)length;
		}
		else
		{
			done = writing ? write(fd, base, length) : read(fd, base, length);
		}
		if (done < 0)
		{
			total = total ? total : failure();
			break;
		}
		total += done;
		if ((size_t)done < length)
			break;
	}
	if (!logged)
		host_file_unlock(fd);
	return total;
}

/* (this C library has no read or write at an offset: the position is moved
there and back, with the file's lock held) */
static long guest_positioned(int fd, uint64_t buffer, uint32_t size, int64_t offset, int writing)
{
	off_t previous;
	long result;

	host_file_lock(fd);
	previous = lseek(fd, 0, SEEK_CUR);
	if (previous == (off_t)-1 || lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1)
	{
		result = failure();
	}
	else
	{
		ssize_t done = writing ? write(fd, GUEST(const void *, buffer), size) :
			read(fd, GUEST(void *, buffer), size);

		result = result_of(done);
		lseek(fd, previous, SEEK_SET);
	}
	host_file_unlock(fd);
	return result;
}

static long guest_rename(int old_directory, uint64_t old_path, int new_directory, uint64_t new_path)
{
	const char *from = path_at(old_directory, old_path), *to = path_at(new_directory, new_path);

	if (!from || !to)
		return -EBADF;
	if (rename(from, to) == 0)
		return 0;
	/* (the SD card's file system does not rename onto a file there is) */
	if (errno == EEXIST && unlink(to) == 0 && rename(from, to) == 0)
		return 0;
	return failure();
}

static long guest_getcwd(uint64_t buffer, uint32_t size)
{
	char path[FS_MAX_PATH];
	const char *start;

	if (!getcwd(path, sizeof(path)))
		return failure();
	/* (without the device, "sdmc:", which the guest's paths do not have:
	the SD card is where a path without one leads) */
	start = strchr(path, ':');
	start = start ? start + 1 : path;
	if (strlen(start) + 1 > size)
		return -ERANGE;
	strcpy(GUEST(char *, buffer), start);
	return (long)strlen(start) + 1;
}

/* ---------- dispatch */

/* a read's result, counted for the profile */
static long counted_read(u64 start, long result)
{
	host_profile_add(_host_profile_read, armGetSystemTick() - start, result > 0 ? (uint64_t)result : 0);
	return result;
}

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	/* (when the call began, for a read) */
	u64 start = armGetSystemTick();

	switch (number)
	{
	case GUEST_SYS_read:
		return counted_read(start, guest_read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case GUEST_SYS_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		return guest_write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c);
	case GUEST_SYS_readv:
		return counted_read(start, guest_vector((int)a, (uint64_t)b, (int)c, 0));
	case GUEST_SYS_writev:
		return guest_vector((int)a, (uint64_t)b, (int)c, 1);
	case GUEST_SYS_pread64:
		return counted_read(start, guest_positioned((int)a, (uint64_t)b, (uint32_t)c, d, 0));
	case GUEST_SYS_pwrite64:
		return guest_positioned((int)a, (uint64_t)b, (uint32_t)c, d, 1);
	case GUEST_SYS_openat:
		return guest_open((int)a, (uint64_t)b, (int)c, (int)d);
	case GUEST_SYS_close:
		return guest_close((int)a);
	case GUEST_SYS_lseek:
		return guest_seek((int)a, (off_t)b, (int)c);
	case GUEST_SYS_fstat:
		return guest_fstat((int)a, (uint64_t)b);
	case GUEST_SYS_newfstatat:
		return guest_stat((int)a, (uint64_t)b, (uint64_t)c);
	case GUEST_SYS_getdents64:
		return guest_getdents((int)a, (uint64_t)b, (uint32_t)c);
	case GUEST_SYS_faccessat:
	{
		const char *path = path_at((int)a, (uint64_t)b);
		struct stat information;

		if (!path)
			return -EBADF;
		return stat(path, &information) == 0 ? 0 : failure();
	}
	case GUEST_SYS_unlinkat:
	{
		const char *path = path_at((int)a, (uint64_t)b);

		if (!path)
			return -EBADF;
		return result_of((int)c & GUEST_AT_REMOVEDIR ? rmdir(path) : unlink(path));
	}
	case GUEST_SYS_mkdirat:
	{
		const char *path = path_at((int)a, (uint64_t)b);

		if (!path)
			return -EBADF;
		return result_of(mkdir(path, 0777));
	}
	case GUEST_SYS_renameat:
	case GUEST_SYS_renameat2:
		return guest_rename((int)a, (uint64_t)b, (int)c, (uint64_t)d);
	case GUEST_SYS_ftruncate:
	{
		long result;

		host_file_lock((int)a);
		result = result_of(ftruncate((int)a, (off_t)b));
		host_file_unlock((int)a);
		return result;
	}
	case GUEST_SYS_fsync:
	case GUEST_SYS_fdatasync:
		return a >= 0 && a <= 2 ? 0 : result_of(fsync((int)a));
	case GUEST_SYS_getcwd:
		return guest_getcwd((uint64_t)a, (uint32_t)b);
	case GUEST_SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case GUEST_SYS_readlinkat:
		/* (nothing is a link) */
		return -EINVAL;
	case GUEST_SYS_fcntl:
		/* (descriptor and status flags: none to tell, none that matter) */
		return 0;
	case GUEST_SYS_ioctl:
		/* the standard output is a terminal (its window's size is asked
		for, TIOCGWINSZ): musl then writes each line as it ends, and the
		log has the last ones of a game that stops */
		if ((a == 1 || a == 2) && (uint32_t)b == 0x5413)
		{
			memset(GUEST(void *, c), 0, 8);
			return 0;
		}
		return -ENOTTY;
	case GUEST_SYS_fchmod:
	case GUEST_SYS_fchmodat:
	case GUEST_SYS_utimensat:
	case GUEST_SYS_flock:
		/* (the SD card's file system keeps none of these) */
		return 0;
	case GUEST_SYS_umask:
		return 022;

	case GUEST_SYS_clock_gettime:
		timespec_out((uint64_t)b, clock_nanoseconds((int)a));
		return 0;
	case GUEST_SYS_clock_getres:
		timespec_out((uint64_t)b, 1);
		return 0;
	case GUEST_SYS_gettimeofday:
	{
		/* (seconds and microseconds) */
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);
		int64_t now = clock_nanoseconds(GUEST_CLOCK_REALTIME);

		if (result)
		{
			result->seconds = (int32_t)(now / 1000000000LL);
			result->nanoseconds = (int32_t)(now % 1000000000LL / 1000);
		}
		return 0;
	}
	case GUEST_SYS_nanosleep:
		if (!a)
			return -EFAULT;
		sleep_nanoseconds(timespec_in((uint64_t)a));
		return 0;
	case GUEST_SYS_clock_nanosleep:
	{
		int64_t duration = timespec_in((uint64_t)c);

		if (!c)
			return -EFAULT;
		if ((int)b & GUEST_TIMER_ABSTIME)
		{
			duration -= clock_nanoseconds((int)a);
			if (duration <= 0)
				return 0;
		}
		sleep_nanoseconds(duration);
		return 0;
	}
	case GUEST_SYS_sched_yield:
		svcSleepThread(YieldType_WithCoreMigration);
		return 0;
	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e);
	case GUEST_SYS_ppoll:
		/* (a wait with nothing to wait for: the sockets are polled by the
		host, port/switch/host/host_net.c) */
		if ((uint32_t)b == 0)
		{
			int64_t duration = timespec_in((uint64_t)c);

			sleep_nanoseconds(duration < 0 ? 1000000000LL : duration);
			return 0;
		}
		return -GUEST_ENOSYS;

	case GUEST_SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		return 0;
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		/* musl then falls back to mmap and copying */
		return -ENOMEM;

	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);

	case GUEST_SYS_set_tid_address:
	case GUEST_SYS_gettid:
		return host_thread_id();
	case GUEST_SYS_getpid:
		return 1;
	case GUEST_SYS_getppid:
	case GUEST_SYS_getuid:
	case GUEST_SYS_geteuid:
	case GUEST_SYS_getgid:
	case GUEST_SYS_getegid:
		return 0;
	case GUEST_SYS_getrandom:
		randomGet(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case GUEST_SYS_kill:
	case GUEST_SYS_tkill:
	case GUEST_SYS_tgkill:
		/* (the guest's abort() raises SIGABRT at itself) */
		host_abort("the game raised a signal");
	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_rt_sigprocmask:
	case GUEST_SYS_sigaltstack:
	case GUEST_SYS_membarrier:
		/* there are no signals */
		return 0;
	case GUEST_SYS_uname:
	{
		/* struct utsname: six strings of 65 characters */
		char *name = GUEST(char *, a);

		memset(name, 0, 6 * 65);
		strcpy(name, "Horizon");
		strcpy(name + 65, "switch");
		strcpy(name + 2 * 65, "0");
		strcpy(name + 3 * 65, "0");
		strcpy(name + 4 * 65, "aarch64");
		return 0;
	}
	case GUEST_SYS_prlimit64:
	case GUEST_SYS_getrlimit:
	case GUEST_SYS_statx:
	case GUEST_SYS_statfs:
	case GUEST_SYS_fstatfs:
	case GUEST_SYS_sysinfo:
	case GUEST_SYS_dup:
	case GUEST_SYS_dup3:
	case GUEST_SYS_pipe2:
	case GUEST_SYS_pselect6:
	case GUEST_SYS_sendmsg:
	case GUEST_SYS_recvmsg:
		return -GUEST_ENOSYS;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -GUEST_ENOSYS;
	}
}
