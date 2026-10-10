/*
HOST_FILES.C

port/linux/src/posix.h's file system helpers for the Switch: what
port/linux/src/posix_files.c is to the other ports, with the SD card's file
system as libnx gives it, which keeps a file's size and times and nothing
else (no permissions, no times to set).

The guest calls these as it does on Android: directory streams are small
handles to it, since it cannot hold a 64-bit pointer.
*/

#include "posix.h"
#include "host.h"

#include <switch.h>

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

static void split64(unsigned long long value, posix_ulong *low, posix_ulong *high)
{
	*low = (posix_ulong)(value & 0xffffffffULL);
	*high = (posix_ulong)(value >> 32);
}

static void fill_information(const struct stat *st, struct posix_file_information *information)
{
	memset(information, 0, sizeof(*information));
	if (S_ISDIR(st->st_mode))
		information->flags |= _posix_file_is_directory;
	split64((unsigned long long)st->st_size, &information->size_low, &information->size_high);
	information->modification_seconds = (posix_ulong)st->st_mtime;
	information->access_seconds = (posix_ulong)st->st_atime;
	information->creation_seconds = (posix_ulong)st->st_ctime;
}

int posix_stat(const char *path, struct posix_file_information *information)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_fstat(int descriptor, struct posix_file_information *information)
{
	struct stat st;

	if (fstat(descriptor, &st) != 0)
		return -1;
	fill_information(&st, information);
	return 0;
}

int posix_set_file_times(const char *path,
	posix_ulong access_seconds, posix_ulong access_nanoseconds,
	posix_ulong modification_seconds, posix_ulong modification_nanoseconds)
{
	(void)path;
	(void)access_seconds;
	(void)access_nanoseconds;
	(void)modification_seconds;
	(void)modification_nanoseconds;
	return 0;
}

int posix_seek(int descriptor, posix_long offset_low, posix_long offset_high, int whence,
	posix_ulong *position_low, posix_ulong *position_high)
{
	off_t offset = (off_t)(((unsigned long long)(posix_ulong)offset_high << 32) | (posix_ulong)offset_low);
	off_t result;

	host_file_lock(descriptor);
	result = lseek(descriptor, offset, whence);
	host_file_unlock(descriptor);
	if (result == (off_t)-1)
		return -1;
	split64((unsigned long long)result, position_low, position_high);
	return 0;
}

int posix_truncate(int descriptor, posix_ulong size_low, posix_ulong size_high)
{
	int result;

	host_file_lock(descriptor);
	result = ftruncate(descriptor, (off_t)(((unsigned long long)size_high << 32) | size_low));
	host_file_unlock(descriptor);
	return result;
}

int posix_disk_space(const char *path,
	posix_ulong *free_low, posix_ulong *free_high,
	posix_ulong *total_low, posix_ulong *total_high)
{
	struct statvfs st;

	if (statvfs(path, &st) != 0)
		return -1;
	split64((unsigned long long)st.f_bavail * st.f_frsize, free_low, free_high);
	split64((unsigned long long)st.f_blocks * st.f_frsize, total_low, total_high);
	return 0;
}

int posix_set_read_only(const char *path, int read_only)
{
	struct stat st;

	(void)read_only;
	return stat(path, &st);
}

int posix_make_directory(const char *path)
{
	return mkdir(path, 0777);
}

#define DIRECTORY_HANDLE_COUNT 64

static DIR *directory_handles[DIRECTORY_HANDLE_COUNT];
static Mutex directory_handle_lock;

static void *directory_handle_new(DIR *directory)
{
	unsigned long index;

	if (!directory)
		return NULL;
	mutexLock(&directory_handle_lock);
	for (index = 0; index < DIRECTORY_HANDLE_COUNT; index++)
	{
		if (!directory_handles[index])
		{
			directory_handles[index] = directory;
			mutexUnlock(&directory_handle_lock);
			return (void *)(index + 1);
		}
	}
	mutexUnlock(&directory_handle_lock);
	closedir(directory);
	return NULL;
}

static DIR *directory_from_handle(void *handle, int release)
{
	unsigned long index = (unsigned long)handle - 1;
	DIR *directory = NULL;

	if (index >= DIRECTORY_HANDLE_COUNT)
		return NULL;
	mutexLock(&directory_handle_lock);
	directory = directory_handles[index];
	if (release)
		directory_handles[index] = NULL;
	mutexUnlock(&directory_handle_lock);
	return directory;
}

void *posix_directory_open(const char *path)
{
	return directory_handle_new(opendir(path));
}

int posix_directory_next(void *directory, char *name, posix_ulong name_size)
{
	DIR *stream = directory_from_handle(directory, 0);
	struct dirent *entry;

	if (!stream)
		return 0;
	while ((entry = readdir(stream)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (strlen(entry->d_name) + 1 > name_size)
			continue;
		strcpy(name, entry->d_name);
		return 1;
	}
	return 0;
}

void posix_directory_close(void *directory)
{
	DIR *stream = directory_from_handle(directory, 1);

	if (stream)
		closedir(stream);
}

int posix_find_entry_case_insensitive(const char *directory, const char *name,
	char *result, posix_ulong result_size)
{
	DIR *handle = opendir(*directory ? directory : ".");
	struct dirent *entry;
	int found = 0;

	if (!handle)
		return 0;
	while ((entry = readdir(handle)) != NULL)
	{
		if (!strcasecmp(entry->d_name, name) && strlen(entry->d_name) + 1 <= result_size)
		{
			strcpy(result, entry->d_name);
			found = 1;
			break;
		}
	}
	closedir(handle);
	return found;
}
