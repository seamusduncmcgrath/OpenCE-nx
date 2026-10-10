/*
HOST_LOADER.C

Loads the guest image: a statically linked AArch64 ELF executable (built
from ILP32 code, see tools/switch_build.py) linked to run at an address
below 4 GB. The image starts with a struct halo_guest_header naming its
import table, which is filled with the host functions of the same names.

Its segments are laid out in heap pages first, and those pages are then
mapped at the image's address (host_memory.c): the pages of its code can
only be written before that. The import table, which is data, is filled
afterwards. The linker script (port/android/guest/guest.ld)
puts everything that is written, .data and after, on a boundary of its own
after everything that is not.
*/

#include "host.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* <elf.h>, of which the C library here has none */
struct elf_header
{
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf_segment
{
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

#define ELF_CLASS_64 2
#define ELF_TYPE_EXECUTABLE 2
#define ELF_MACHINE_AARCH64 183
#define ELF_SEGMENT_LOAD 1
#define ELF_SEGMENT_WRITE 2

#define PAGE_MASK 0xfffULL

struct host_guest_image host_image;

static void missing_import(void)
{
	host_fatal("the guest called a host function that is not available");
}

int host_load_image(const void *file, size_t size)
{
	const struct elf_header *elf = file;
	const struct elf_segment *segments;
	uint64_t low = ~0ULL, high = 0, data = ~0ULL;
	const struct halo_guest_header *header;
	char *memory;
	uint32_t index;

	if (size < sizeof(*elf) || memcmp(elf->e_ident, "\177ELF", 4) || elf->e_ident[4] != ELF_CLASS_64 ||
		elf->e_machine != ELF_MACHINE_AARCH64 || elf->e_type != ELF_TYPE_EXECUTABLE ||
		elf->e_phentsize != sizeof(*segments) || elf->e_phoff + (uint64_t)elf->e_phnum * sizeof(*segments) > size)
	{
		host_logf(HOST_LOG_ERROR, "the guest image is not an AArch64 executable");
		return -1;
	}
	segments = (const struct elf_segment *)((const char *)file + elf->e_phoff);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const struct elf_segment *segment = &segments[index];

		if (segment->p_type != ELF_SEGMENT_LOAD)
			continue;
		if (segment->p_offset + segment->p_filesz > size || segment->p_filesz > segment->p_memsz)
			return -1;
		if (segment->p_vaddr < low)
			low = segment->p_vaddr;
		if (segment->p_vaddr + segment->p_memsz > high)
			high = segment->p_vaddr + segment->p_memsz;
		if ((segment->p_flags & ELF_SEGMENT_WRITE) && segment->p_vaddr < data)
			data = segment->p_vaddr;
	}
	low &= ~PAGE_MASK;
	high = (high + PAGE_MASK) & ~PAGE_MASK;
	if (low != HALO_GUEST_IMAGE_BASE || high > 0x100000000ULL || high <= low)
	{
		host_logf(HOST_LOG_ERROR, "the guest image spans %llx-%llx", (unsigned long long)low, (unsigned long long)high);
		return -1;
	}
	if (data == ~0ULL)
		data = high;
	/* (a page holds code or data, not both) */
	for (index = 0; index < elf->e_phnum; index++)
	{
		const struct elf_segment *segment = &segments[index];

		if (segment->p_type == ELF_SEGMENT_LOAD && !(segment->p_flags & ELF_SEGMENT_WRITE) &&
			segment->p_vaddr + segment->p_memsz > data)
		{
			data = low;
		}
	}
	if ((data & PAGE_MASK) || data <= low)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's data at %llx is not apart from its code", (unsigned long long)data);
		return -1;
	}

	memory = memalign(0x200000, high - low);
	if (!memory)
	{
		host_logf(HOST_LOG_ERROR, "out of memory for the guest image (%llu MB)", (unsigned long long)((high - low) >> 20));
		return -1;
	}
	memset(memory, 0, high - low);
	for (index = 0; index < elf->e_phnum; index++)
	{
		const struct elf_segment *segment = &segments[index];

		if (segment->p_type == ELF_SEGMENT_LOAD)
			memcpy(memory + (segment->p_vaddr - low), (const char *)file + segment->p_offset, segment->p_filesz);
	}

	header = (const struct halo_guest_header *)memory;
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "the guest image header does not match this host");
		free(memory);
		return -1;
	}
	/* (the import table is written once the image is in its place: it is
	data. host_link_image) */
	if (header->import_table < data || header->import_table >= high || header->import_names < low ||
		header->import_names >= high || header->import_count < low || header->import_count + 4 > high)
	{
		host_logf(HOST_LOG_ERROR, "the guest image's import table is out of place");
		free(memory);
		return -1;
	}

	if (host_memory_initialize((uint32_t)low, memory, (uint32_t)(high - low), (uint32_t)(data - low)) != 0)
	{
		free(memory);
		return -1;
	}
	host_image.header = (const struct halo_guest_header *)(uintptr_t)low;
	host_image.base = (uint32_t)low;
	host_image.end = (uint32_t)high;
	host_logf(HOST_LOG_INFO, "guest image %08llx-%08llx", (unsigned long long)low, (unsigned long long)high);
	return 0;
}

/* fills the image's import table with the host's functions (once EGL has
started: it names Mesa's, host_gl.c) */
void host_link_image(void)
{
	const struct halo_guest_header *header = host_image.header;
	uint64_t *table = (uint64_t *)(uintptr_t)header->import_table;
	const char *name = (const char *)(uintptr_t)header->import_names;
	uint32_t count = *(const uint32_t *)(uintptr_t)header->import_count;
	uint32_t index;
	int missing = 0;

	if ((uint64_t)header->import_table + (uint64_t)count * 8 > host_image.end)
		host_fatal("the guest image's import table is out of place");
	for (index = 0; index < count; index++)
	{
		void *function = host_resolve_import(name);

		if (!function && !strncmp(name, "hostgl_", 7))
			function = host_gl_resolve(name + 7);
		if (!function)
		{
			host_logf(HOST_LOG_WARN, "guest import %s is not available", name);
			function = (void *)missing_import;
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)function;
		name += strlen(name) + 1;
	}
	host_logf(HOST_LOG_INFO, "%u guest imports (%d unavailable)", count, missing);
}
