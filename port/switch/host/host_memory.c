/*
HOST_MEMORY.C

Guest address space for the Switch port.

Everything the guest touches must lie below 4 GB, and two of its ranges at
the addresses it was built for: the Xbox contiguous window at 0x80000000 and
the image's own. A Horizon process cannot ask for memory at an address, but
it can map pages of its heap a second time at one, as a program loader maps
the modules it loads (svcMapProcessCodeMemory, which homebrew may use on
itself: hbloader gives each program its process handle). Each range the
guest has is made that way, from heap memory the host allocates:

- the window and the image, at start-up;
- pools for the guest's other mappings (malloc arenas, thread stacks), and a
  range of its own for each large one, wherever there is room below 4 GB.

Such memory is in one of two states: code, as mapped, which can be made
read-only and executable but never written, or data, once it has been made
writable, whose protection svcSetMemoryPermission then changes at will (but
only over pages that have the same one, hence set_permission below). The
image's code is therefore copied into its heap pages before they are mapped.

The kernel places the heap's and its other regions at random in a 512 GB
address space. Now and then one of them covers the fixed ranges, which
cannot then be mapped: the game says so, and starts the next time.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches, and the exception handler (host_exception.c) records
the first write to each here. With debug.memory_watch = false no page is
protected, and the tracking compares page contents instead, with the
Android app's code for it (port/android/host/host_watch_hash.c).
*/

#include "host.h"
/* (the Android app's, which the two hosts share: tools/switch_build.py
compiles its source with this program) */
#include "../../android/host/host_watch_hash.h"

#include <switch.h>

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000ULL
#define LOW_LIMIT 0x100000000ULL
/* nothing of the guest's below this */
#define LOW_START 0x10000000ULL
/* ranges are placed on these boundaries, in the heap and below 4 GB, so the
kernel can map them with its larger blocks */
#define RANGE_ALIGNMENT 0x200000ULL
#define POOL_SIZE (32ULL * 1024 * 1024)
/* mappings above this get a range of their own, given back when unmapped */
#define POOL_LARGEST (POOL_SIZE / 4)
#define MAXIMUM_RANGES 192

/* the guest's mmap flags (musl's sys/mman.h) */
#define GUEST_MAP_FIXED 0x10
#define GUEST_MAP_ANONYMOUS 0x20
#define GUEST_MAP_FIXED_NOREPLACE 0x100000

enum
{
	_range_unused,
	_range_window,
	_range_image,
	_range_pool,
	/* one mapping of the guest's: unmapped when its last page is */
	_range_single,
};

struct range
{
	int type;
	uint64_t base, size;
	/* the heap pages mapped at base */
	void *source;
	/* 1 for each page handed out (pools and single mappings) */
	uint8_t *used;
	uint64_t free_pages;
};

static struct range ranges[MAXIMUM_RANGES];
static Mutex memory_lock;
static Handle process_handle = INVALID_HANDLE;

/* the regions the kernel maps nothing of this kind in */
static uint64_t excluded[3][2];

static uint64_t round_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

/* ---------- placing heap pages below 4 GB */

static void address_space_query(void)
{
	static const InfoType types[3][2] =
	{
		{ InfoType_HeapRegionAddress, InfoType_HeapRegionSize },
		{ InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
		{ InfoType_StackRegionAddress, InfoType_StackRegionSize },
	};
	int index;

	/* (the kernel wants a real handle for these mappings, which the
	homebrew loader passes; the "current process" one does not do) */
	process_handle = envGetOwnProcessHandle();
	for (index = 0; index < 3; index++)
	{
		u64 address = 0, size = 0;

		svcGetInfo(&address, types[index][0], CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&size, types[index][1], CUR_PROCESS_HANDLE, 0);
		excluded[index][0] = address;
		excluded[index][1] = address + size;
		host_logf(HOST_LOG_INFO, "address space: %s region %010llx-%010llx",
			index == 0 ? "heap" : index == 1 ? "alias" : "stack",
			(unsigned long long)address, (unsigned long long)(address + size));
	}
}

/* whether nothing is mapped in the range, and code may be */
static int address_range_free(uint64_t base, uint64_t size)
{
	MemoryInfo information;
	u32 page_information;
	int index;

	if (base < LOW_START || base + size > LOW_LIMIT)
		return 0;
	for (index = 0; index < 3; index++)
	{
		if (base < excluded[index][1] && excluded[index][0] < base + size)
			return 0;
	}
	if (R_FAILED(svcQueryMemory(&information, &page_information, base)))
		return 0;
	return (information.type & 0xff) == MemType_Unmapped && information.addr + information.size >= base + size;
}

/* logs what the address space holds in a range that could not be mapped */
static void address_range_describe(uint64_t base, uint64_t size)
{
	uint64_t address = base;
	int count;

	for (count = 0; address < base + size && count < 16; count++)
	{
		MemoryInfo information;
		u32 page_information;

		if (R_FAILED(svcQueryMemory(&information, &page_information, address)) || !information.size)
			break;
		host_logf(HOST_LOG_ERROR, "  %010llx-%010llx: memory type %x, permission %x",
			(unsigned long long)information.addr, (unsigned long long)(information.addr + information.size),
			(unsigned)(information.type & 0xff), (unsigned)information.perm);
		address = information.addr + information.size;
	}
}

static struct range *range_slot(void)
{
	int index;

	for (index = 0; index < MAXIMUM_RANGES; index++)
	{
		if (ranges[index].type == _range_unused)
			return &ranges[index];
	}
	return NULL;
}

/* heap pages for a range of size bytes */
static void *range_source(uint64_t size)
{
	void *source = memalign(size >= RANGE_ALIGNMENT ? RANGE_ALIGNMENT : PAGE, size);

	if (!source)
		host_logf(HOST_LOG_ERROR, "out of memory for %llu MB of guest memory", (unsigned long long)(size >> 20));
	return source;
}

/* maps the size bytes of heap pages at source at base, as data; code_size
bytes at its start stay code (read-only, executable). The range frees source
when it is unmapped; on failure it is the caller's still */
static struct range *range_map(int type, uint64_t base, uint64_t size, void *source, uint64_t code_size)
{
	struct range *range = range_slot();
	Result result;

	if (!range || !source)
		return NULL;
	/* (libnx finds room for its own mappings under this lock) */
	virtmemLock();
	result = address_range_free(base, size) ? svcMapProcessCodeMemory(process_handle, base, (u64)(uintptr_t)source, size) :
		MAKERESULT(Module_Libnx, LibnxError_BadInput);
	virtmemUnlock();
	if (R_SUCCEEDED(result))
	{
		if (code_size)
			result = svcSetProcessMemoryPermission(process_handle, base, code_size, Perm_Rx);
		if (R_SUCCEEDED(result) && size > code_size)
			result = svcSetProcessMemoryPermission(process_handle, base + code_size, size - code_size, Perm_Rw);
		if (R_FAILED(result))
			svcUnmapProcessCodeMemory(process_handle, base, (u64)(uintptr_t)source, size);
	}
	if (R_FAILED(result))
		return NULL;
	range->type = type;
	range->base = base;
	range->size = size;
	range->source = source;
	range->used = NULL;
	range->free_pages = 0;
	return range;
}

static void range_unmap(struct range *range)
{
	svcUnmapProcessCodeMemory(process_handle, range->base, (u64)(uintptr_t)range->source, range->size);
	free(range->source);
	free(range->used);
	memset(range, 0, sizeof(*range));
}

/* a range of size bytes wherever there is room, above the image first */
static struct range *range_map_anywhere(int type, uint64_t size)
{
	uint64_t above = round_up(host_image.end ? host_image.end : HALO_GUEST_IMAGE_BASE, RANGE_ALIGNMENT);
	uint64_t starts[2] = { above, LOW_START }, ends[2] = { LOW_LIMIT, HALO_GUEST_WINDOW_BASE };
	void *source;
	int pass;

	if (!range_slot())
	{
		host_logf(HOST_LOG_ERROR, "too many ranges of guest memory");
		return NULL;
	}
	source = range_source(size);
	if (!source)
		return NULL;
	for (pass = 0; pass < 2; pass++)
	{
		uint64_t base;

		for (base = starts[pass]; base + size <= ends[pass]; base += RANGE_ALIGNMENT)
		{
			struct range *range = address_range_free(base, size) ? range_map(type, base, size, source, 0) : NULL;

			if (range)
				return range;
		}
	}
	free(source);
	host_logf(HOST_LOG_ERROR, "no room below 4 GB for %llu MB of guest memory", (unsigned long long)(size >> 20));
	return NULL;
}

static struct range *range_of(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < MAXIMUM_RANGES; index++)
	{
		struct range *range = &ranges[index];

		if (range->type != _range_unused && in_range(address, size, range->base, range->base + range->size))
			return range;
	}
	return NULL;
}

/* svcSetMemoryPermission needs the pages of a range to have one protection
now; where they do not, each half is set by itself */
static Result set_permission(uint64_t address, uint64_t size, uint32_t permission)
{
	Result result = svcSetMemoryPermission((void *)(uintptr_t)address, size, permission);
	uint64_t half;

	if (R_SUCCEEDED(result) || size <= PAGE)
		return result;
	half = round_up(size / 2, PAGE);
	result = set_permission(address, half, permission);
	if (R_SUCCEEDED(result))
		result = set_permission(address + half, size - half, permission);
	return result;
}

static uint32_t permission_of(int protection)
{
	if (protection & HOST_PROT_WRITE)
		return Perm_Rw;
	/* (data cannot be executed; nothing of the game's is) */
	return protection & (HOST_PROT_READ | HOST_PROT_EXEC) ? Perm_R : Perm_None;
}

int host_memory_initialize(uint32_t base, void *image_memory, uint32_t size, uint32_t data_offset)
{
	void *window;

	mutexInit(&memory_lock);
	address_space_query();
	if (process_handle == INVALID_HANDLE)
	{
		host_logf(HOST_LOG_ERROR, "the homebrew loader gave no process handle: memory cannot be mapped below 4 GB");
		return -1;
	}
	window = range_source(HALO_GUEST_WINDOW_SIZE);
	if (!window)
		return -1;
	memset(window, 0, HALO_GUEST_WINDOW_SIZE);
	if (!range_map(_range_window, HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE, window, 0))
	{
		host_logf(HOST_LOG_ERROR, "cannot map the Xbox memory window at %08x", (unsigned)HALO_GUEST_WINDOW_BASE);
		address_range_describe(HALO_GUEST_WINDOW_BASE, HALO_GUEST_WINDOW_SIZE);
		free(window);
		return -1;
	}
	if (!range_map(_range_image, base, size, image_memory, data_offset))
	{
		host_logf(HOST_LOG_ERROR, "cannot map the guest image at %08x", (unsigned)base);
		address_range_describe(base, size);
		return -1;
	}
	return 0;
}

/* ---------- page pools */

static void *range_take(struct range *range, uint64_t pages)
{
	uint64_t count = range->size / PAGE, run = 0, page;

	if (range->free_pages < pages)
		return NULL;
	for (page = 0; page < count; page++)
	{
		if (range->used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
		{
			uint64_t first = page + 1 - pages;

			memset(&range->used[first], 1, pages);
			range->free_pages -= pages;
			return (void *)(uintptr_t)(range->base + first * PAGE);
		}
	}
	return NULL;
}

static struct range *range_new(int type, uint64_t size)
{
	struct range *range = range_map_anywhere(type, size);

	if (!range)
		return NULL;
	range->used = calloc(1, size / PAGE);
	if (!range->used)
	{
		range_unmap(range);
		return NULL;
	}
	range->free_pages = size / PAGE;
	host_logf(HOST_LOG_INFO, "guest memory at %08llx, %llu MB", (unsigned long long)range->base,
		(unsigned long long)(size >> 20));
	return range;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t length = round_up(size, PAGE), pages = length / PAGE;
	void *address = NULL;
	int index;

	if (!pages || length >= LOW_LIMIT)
		return NULL;
	mutexLock(&memory_lock);
	if (length > POOL_LARGEST)
	{
		struct range *range = range_new(_range_single, round_up(length, RANGE_ALIGNMENT));

		if (range)
			address = range_take(range, pages);
	}
	else
	{
		for (index = 0; index < MAXIMUM_RANGES && !address; index++)
		{
			if (ranges[index].type == _range_pool)
				address = range_take(&ranges[index], pages);
		}
		if (!address)
		{
			struct range *range = range_new(_range_pool, POOL_SIZE);

			if (range)
				address = range_take(range, pages);
		}
	}
	mutexUnlock(&memory_lock);
	if (!address)
		return NULL;
	/* (free pages are writable: host_low_unmap) */
	memset(address, 0, length);
	if (permission_of(protection) != Perm_Rw)
		set_permission((uint64_t)(uintptr_t)address, length, permission_of(protection));
	return address;
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)(uintptr_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)(uintptr_t)address + size, PAGE) - start;
	struct range *range;

	mutexLock(&memory_lock);
	range = range_of(start, length);
	if (range && range->used)
	{
		uint64_t first = (start - range->base) / PAGE, count = length / PAGE, page;

		set_permission(start, length, Perm_Rw);
		for (page = first; page < first + count; page++)
		{
			if (range->used[page])
			{
				range->used[page] = 0;
				range->free_pages++;
			}
		}
		if (range->type == _range_single && range->free_pages == range->size / PAGE)
			range_unmap(range);
	}
	mutexUnlock(&memory_lock);
}

int host_low_protect(void *address, size_t size, int protection)
{
	uint64_t start = (uint64_t)(uintptr_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)(uintptr_t)address + size, PAGE) - start;

	return R_SUCCEEDED(set_permission(start, length, permission_of(protection))) ? 0 : -1;
}

/* (without the lock: the crash report asks, whatever its thread holds, and
ranges only come and go with the guest's large mappings) */
int host_low_owns(uintptr_t address, size_t size)
{
	return range_of(address, size) != NULL;
}

/* ---------- the guest's memory system calls */

static int owned_type(uint64_t address, uint64_t size)
{
	struct range *range;
	int type;

	mutexLock(&memory_lock);
	range = range_of(address, size);
	type = range ? range->type : _range_unused;
	mutexUnlock(&memory_lock);
	return type;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size, PAGE);
	void *result;

	(void)offset;
	if (!length)
		return -EINVAL;
	/* (nothing of the guest's maps a file) */
	if (!(flags & GUEST_MAP_ANONYMOUS) || fd != -1)
		return -ENODEV;
	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
	{
		int type;

		if ((address & (PAGE - 1)) || address + length > LOW_LIMIT)
			return -EINVAL;
		type = owned_type(address, length);
		if (type == _range_image)
			return -EINVAL;
		if (type == _range_unused)
		{
			/* a range of the guest's choosing (the Custom Edition tag
			cache, xbox_memory.c), if nothing is there */
			struct range *range;

			if (!(flags & GUEST_MAP_FIXED_NOREPLACE))
				return -EINVAL;
			void *source = range_source(length);
			uint8_t *used = malloc(length / PAGE);

			mutexLock(&memory_lock);
			range = source && used ? range_map(_range_single, address, length, source, 0) : NULL;
			if (range)
			{
				memset(used, 1, length / PAGE);
				range->used = used;
			}
			mutexUnlock(&memory_lock);
			if (!range)
			{
				free(source);
				free(used);
				return -EEXIST;
			}
		}
		else if (R_FAILED(set_permission(address, length, Perm_Rw)))
		{
			return -ENOMEM;
		}
		/* fresh pages, as a mapping over what was there gives */
		memset((void *)(uintptr_t)address, 0, length);
		if (permission_of(protection) != Perm_Rw)
			set_permission(address, length, permission_of(protection));
		return (long)address;
	}
	result = host_low_map(length, protection);
	return result ? (long)(uintptr_t)result : -ENOMEM;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size, PAGE);

	if ((address & (PAGE - 1)) || address + length > LOW_LIMIT)
		return -EINVAL;
	switch (owned_type(address, length))
	{
	case _range_window:
		/* the pages stay the window's, out of reach */
		set_permission(address, length, Perm_None);
		return 0;
	case _range_pool:
	case _range_single:
		host_low_unmap((void *)(uintptr_t)address, length);
		return 0;
	default:
		return -EINVAL;
	}
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t start = address & ~(PAGE - 1);
	uint64_t length = round_up(address + size, PAGE) - start;
	int type;

	if (address + size > LOW_LIMIT)
		return -EINVAL;
	type = owned_type(start, length);
	if (type == _range_unused || type == _range_image)
		return -ENOMEM;
	return R_SUCCEEDED(set_permission(start, length, permission_of(protection))) ? 0 : -EACCES;
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
/* debug.memory_watch = false: no page is protected, and a page that is
watched is hashed once a frame, as the Android app does where it cannot
catch its page faults (port/android/host/host_watch_hash.h) */
static int watch_hashing;
static uint8_t page_hash_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.base = (const uint8_t *)(uintptr_t)HALO_GUEST_WINDOW_BASE,
	.page_count = WATCH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_generation,
	.current_generation = &current_generation,
	.frame = 1,
};

void host_memory_watch_use_hashes(void)
{
	watch_hashing = 1;
}

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / PAGE;
}

static uint64_t watch_address(uint64_t page)
{
	return HALO_GUEST_WINDOW_BASE + page * PAGE;
}

static Result mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	return svcSetMemoryPermission((void *)(uintptr_t)watch_address(page), PAGE, Perm_Rw);
}

/* (called by the exception handler, on whichever thread wrote: no locks) */
int host_memory_watch_fault(uint64_t address)
{
	uint64_t page;

	if (!watch_active || !in_window(address))
		return 0;
	page = watch_page(address);
	if (!page_protected[page])
		return 0;
	if (host_profile_enabled)
	{
		u64 start = armGetSystemTick();
		Result result = mark_written(page);

		host_profile_add(_host_profile_fault, armGetSystemTick() - start, 0);
		return R_SUCCEEDED(result);
	}
	return R_SUCCEEDED(mark_written(page));
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
	host_logf(HOST_LOG_INFO, "write tracking: %s", watch_hashing ? "page hashes (debug.memory_watch = false)" :
		"page protection");
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page, changed = 0;
	u64 start = host_profile_enabled ? armGetSystemTick() : 0;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_protect(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		uint64_t end = page;

		if (page_protected[page])
			continue;
		/* each run of pages not protected yet, with one call */
		while (end <= last && !page_protected[end])
			page_protected[end++] = 1;
		changed += end - page;
		if (R_FAILED(set_permission(watch_address(page), (end - page) * PAGE, Perm_R)))
		{
			/* (the kernel has no room to track more protections) pages
			nothing watches always look written */
			set_permission(watch_address(page), (end - page) * PAGE, Perm_Rw);
			for (; page < end; page++)
			{
				page_protected[page] = 0;
				page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
			}
		}
		page = end - 1;
	}
	if (changed)
		host_profile_add(_host_profile_protect, armGetSystemTick() - start, changed);
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	/* (which hashes the pages again, once a frame) */
	if (watch_hashing)
		return watch_hash_generation(&page_hashes, (uint32_t)first, (uint32_t)last);
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	/* (hashed, the write shows in the next hash, and no page is protected) */
	if (!watch_active || watch_hashing || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_forget(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

/* memory_watch_begin_frame: the pages may be hashed again; nothing with
page protection */
void host_memory_watch_begin_frame(void)
{
	if (watch_hashing)
		watch_hash_begin_frame(&page_hashes);
}
