/*
HOST_EXCEPTION.C

CPU exceptions (host_exception_asm.S has the entry and how Horizon delivers
them).

A write to a page the renderer watches is the one exception the game goes
on from: the page becomes writable and the write runs again
(host_memory.c). Every other is a crash. The handler does not report it
there, where the thread may hold any lock: it points the kernel's record at
host_exception_report on a stack of its own, and the thread resumes there
as ordinary code, which writes crash.txt beside the game data, tells the
player and ends the program.
*/

#include "host.h"

#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXCEPTION_STACK_SIZE 0x8000
#define REPORT_STACK_SIZE 0x10000

/* ams::svc::ExceptionType */
#define EXCEPTION_DATA_ABORT 0x101

/* what host_exception_asm.S saves, in its order */
struct host_exception_registers
{
	uint64_t x9_to_x29[21];
	uint64_t fpcr, fpsr;
	uint64_t padding;
	__uint128_t vectors[32];
};

struct crash
{
	uint32_t type;
	int thread;
	ThreadExceptionFrameA64 record;
	uint64_t x9_to_x29[21];
};

static uint8_t exception_stack[EXCEPTION_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t report_stack[REPORT_STACK_SIZE] __attribute__((aligned(16)));
static struct crash crash;
static volatile int crashed;

/* (host_exception_asm.S reads it; zero until the handler may run) */
uint64_t host_exception_stack_top;

/* host_main.c: the data folder, on the SD card's file system */
const char *host_data_folder(void);

/* the program's first byte (libnx's linker script) */
extern const char __start__[];

static void host_exception_report(void) __attribute__((noreturn));

int host_exception_handler(uint32_t type, ThreadExceptionFrameA64 *record,
	struct host_exception_registers *registers)
{
	if (type == EXCEPTION_DATA_ABORT && host_memory_watch_fault(record->far))
		return 1;
	/* a second crash (another thread's, or the report's own): the kernel's */
	if (__sync_lock_test_and_set(&crashed, 1))
		return 0;
	crash.type = type;
	crash.thread = host_thread_id();
	crash.record = *record;
	memcpy(crash.x9_to_x29, registers->x9_to_x29, sizeof(crash.x9_to_x29));
	record->elr_el1 = (uint64_t)(uintptr_t)host_exception_report;
	record->sp = (uint64_t)(uintptr_t)(report_stack + sizeof(report_stack));
	record->lr = 0;
	return 1;
}

void host_exception_initialize(void)
{
	host_exception_stack_top = (uint64_t)(uintptr_t)(exception_stack + sizeof(exception_stack));
}

/* ---------- the report */

static char report[8192];
static size_t report_length;

static void line(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void line(const char *format, ...)
{
	va_list arguments;
	size_t room;
	int length;

	/* (room for the text, then its newline and the terminator) */
	if (report_length + 3 > sizeof(report))
		return;
	room = sizeof(report) - report_length - 2;
	va_start(arguments, format);
	length = vsnprintf(report + report_length, room, format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	report_length += (size_t)length < room ? (size_t)length : room - 1;
	report[report_length++] = '\n';
	report[report_length] = 0;
}

/* where an address is: the guest image (for llvm-symbolizer
--obj=build/switch/halo_guest.elf), or the program (--obj=build/switch/OpenCE.elf
with the offset) */
static void describe(const char *name, uint64_t address)
{
	uint64_t program = (uint64_t)(uintptr_t)__start__;

	if (host_image.base && address >= host_image.base && address < host_image.end)
		line("%s %016llx  halo_guest.elf 0x%llx", name, (unsigned long long)address, (unsigned long long)address);
	else if (address >= program && address - program < 0x4000000ULL)
		line("%s %016llx  OpenCE.elf 0x%llx", name, (unsigned long long)address, (unsigned long long)(address - program));
	else
		line("%s %016llx", name, (unsigned long long)address);
}

static void write_crash_file(void)
{
	FsFileSystem *file_system = fsdevGetDeviceFileSystem("sdmc");
	char path[FS_MAX_PATH];
	FsFile file;

	/* (the file system's own calls: the C library's may be what crashed,
	or locked) */
	if (!file_system)
		return;
	snprintf(path, sizeof(path), "%s/crash.txt", host_data_folder());
	fsFsDeleteFile(file_system, path);
	if (R_FAILED(fsFsCreateFile(file_system, path, (s64)report_length, 0)))
		return;
	if (R_SUCCEEDED(fsFsOpenFile(file_system, path, FsOpenMode_Write, &file)))
	{
		fsFileWrite(&file, 0, report, report_length, FsWriteOption_Flush);
		fsFileClose(&file);
	}
}

static void host_exception_report(void)
{
	const ThreadExceptionFrameA64 *record = &crash.record;
	uint64_t fp = crash.x9_to_x29[20];
	int index;

	line("OpenCE %s stopped: exception %x on thread %d", HALO_SWITCH_VERSION, (unsigned)crash.type, crash.thread);
	line("address %016llx  esr %08x  pstate %08x", (unsigned long long)record->far, (unsigned)record->esr,
		(unsigned)record->pstate);
	describe("pc ", record->elr_el1);
	describe("lr ", record->lr);
	line("sp  %016llx", (unsigned long long)record->sp);
	for (index = 0; index < 9; index += 3)
	{
		line("x%-2d %016llx %016llx %016llx", index, (unsigned long long)record->cpu_gprs[index],
			(unsigned long long)record->cpu_gprs[index + 1], (unsigned long long)record->cpu_gprs[index + 2]);
	}
	for (index = 0; index < 21; index += 3)
	{
		line("x%-2d %016llx %016llx %016llx", index + 9, (unsigned long long)crash.x9_to_x29[index],
			(unsigned long long)crash.x9_to_x29[index + 1], (unsigned long long)crash.x9_to_x29[index + 2]);
	}
	/* the frame records, while they are in guest memory: fp and lr, 8
	bytes each */
	for (index = 0; index < 32 && fp && !(fp & 7) && host_low_owns(fp, 16); index++)
	{
		const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

		describe("  from", frame[1]);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}

	svcOutputDebugString(report, report_length);
	write(STDOUT_FILENO, report, report_length);
	write_crash_file();
	host_show_message("OpenCE stopped",
		"The game stopped because of an error.\n\nThe details are in crash.txt, in the folder of the game data.");
	svcExitProcess();
	for (;;)
		;
}
