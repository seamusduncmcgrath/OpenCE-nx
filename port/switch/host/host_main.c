/*
HOST_MAIN.C

Entry point of the Switch port (the homebrew program's main).

It loads the guest image (the game, built as ILP32 code) from the program's
RomFS, gives it an environment describing where the game data and saves
live, and runs its main() on a stack in guest memory (host_thread.c), on
which the game runs from then on. The image is placed first of all that goes
below 4 GB: it and the Xbox window have the only fixed addresses.

Storage (see port/switch/README.md): the game data (the folder holding
maps/) is the folder the program is in on the SD card, or /switch/OpenCE;
saves go to its save/ folder. The settings, config.toml, live there too
(port/linux/src/port_config.c, which the game reads); this file reads only
the few settings that are the host's own from it (config_read). The host's log
is opence.log there, and nxlink's console when the program was sent by
nxlink.

The program ends with the process (host_exit): the heap pages it mapped
below 4 GB stay locked while it lives, and the homebrew loader would hand
that heap to the next program.
*/

#include "host.h"
#include "tomlc17.h"

#include <switch.h>

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_FOLDER "/switch/OpenCE"
#define MAIN_STACK_SIZE (16 * 1024 * 1024)
/* what the game, Mesa and the system's services need between them */
#define MINIMUM_MEMORY (1024ULL * 1024 * 1024)

/* host_sdl.c */
int host_video_prepare(void);
int host_video_release(void);
void host_display_size(int *width, int *height);

/* ---------- logging and termination */

static Mutex log_lock;
static FILE *log_file;
static u64 log_synchronized;
/* nxlink's console, when the program was sent by nxlink */
static int nxlink_socket = -1;

void host_log_raw(const char *text)
{
	svcOutputDebugString(text, strlen(text));
}

void host_log(int priority, const char *text)
{
	char line[1200];
	int length = snprintf(line, sizeof(line), "%s%s\n",
		priority >= HOST_LOG_ERROR ? "error: " : priority == HOST_LOG_WARN ? "warning: " : "", text);

	if (length < 0)
		return;
	if ((size_t)length >= sizeof(line))
	{
		length = sizeof(line) - 1;
		line[length - 1] = '\n';
	}
	mutexLock(&log_lock);
	if (nxlink_socket >= 0)
		write(nxlink_socket, line, (size_t)length);
	if (log_file)
	{
		u64 now = armGetSystemTick();

		fwrite(line, 1, (size_t)length, log_file);
		fflush(log_file);
		/* onto the card now and then, and with each problem: the lines
		before the game stops are the ones that tell why */
		if (priority >= HOST_LOG_WARN || armTicksToNs(now - log_synchronized) > 2000000000ULL)
		{
			fsync(fileno(log_file));
			log_synchronized = now;
		}
	}
	mutexUnlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	host_log(priority, text);
}

static void log_close(void)
{
	mutexLock(&log_lock);
	if (log_file)
	{
		fflush(log_file);
		fsync(fileno(log_file));
		fclose(log_file);
		log_file = NULL;
	}
	mutexUnlock(&log_lock);
}

/* on the screen: libnx's text console (not the system's error dialog, which
files an error report with the system each time it is shown) */
static void console_message(const char *title, const char *message)
{
	PadState pad;

	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	printf("\n %s\n\n%s\n\n Press + to exit.\n", title, message);
	while (appletMainLoop())
	{
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
}

/* shows a message and waits for the player, if the screen can be had: the
console takes it from the game, which only the thread that draws the game
can give up (and the game does not go on afterwards) */
void host_show_message(const char *title, const char *message)
{
	if (host_video_release())
		console_message(title, message);
}

static void end_process(void) __attribute__((noreturn));

static void end_process(void)
{
	log_close();
	svcExitProcess();
	for (;;)
		;
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	host_show_message("OpenCE cannot go on", message);
	end_process();
}

void host_abort(const char *reason)
{
	host_logf(HOST_LOG_ERROR, "guest abort: %s", reason);
	host_show_message("OpenCE stopped",
		"The game stopped because of an error.\n\nThe reason is in debug.txt and opence.log, in the folder of the game data.");
	end_process();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	end_process();
}

/* (whatever else ends the program ends the process too: see above) */
void userAppExit(void)
{
	end_process();
}

/* ---------- paths */

static char data_root[512];
static char save_root[520];

const char *host_data_folder(void)
{
	return data_root[0] ? data_root : DEFAULT_FOLDER;
}

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

static int directory_has_maps(const char *root)
{
	char path[600];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* the folder the program is in, as a path on the SD card (without the
device's name, "sdmc:": the guest's paths are POSIX ones, and the card is
where one without a device leads) */
static void program_folder(const char *program, char *folder, size_t size)
{
	const char *start = program ? strchr(program, ':') : NULL;
	char *separator;

	folder[0] = 0;
	if (!program || strncmp(program, "sdmc:", 5))
		return;
	snprintf(folder, size, "%s", start + 1);
	separator = strrchr(folder, '/');
	if (!separator || separator == folder)
		folder[0] = 0;
	else
		*separator = 0;
}

static void choose_data_root(const char *program)
{
	char folder[512];

	program_folder(program, folder, sizeof(folder));
	/* beside the program, if the maps are; else the usual folder, which is
	also where a program sent by nxlink (to /switch itself) looks */
	if (folder[0] && (directory_has_maps(folder) || (strcmp(folder, "/switch") && !directory_has_maps(DEFAULT_FOLDER))))
		snprintf(data_root, sizeof(data_root), "%s", folder);
	else
		snprintf(data_root, sizeof(data_root), "%s", DEFAULT_FOLDER);
	snprintf(save_root, sizeof(save_root), "%s/save", data_root);
	mkdir("/switch", 0777);
	mkdir(data_root, 0777);
	mkdir(save_root, 0777);
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;

	entry = malloc(length + strlen(value) + 2);
	if (!entry)
		return;
	sprintf(entry, "%s=%s", name, value);
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* display.render_thread (or true): read with the others, used once the
display has started */
static int render_thread = 1;

/* the settings of config.toml that are the host's: debug.sample_seconds (or
0), debug.memory_watch (or true), debug.profile (or false) and
display.render_thread */
static void config_read(const char *path, double *sample_seconds, int *memory_watch, int *profile)
{
	toml_result_t result = toml_parse_file_ex(path);

	*sample_seconds = 0.0;
	*memory_watch = 1;
	*profile = 0;
	if (!result.ok)
		return;
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		toml_datum_t watch = toml_seek(result.toptab, "debug.memory_watch");
		toml_datum_t profiled = toml_seek(result.toptab, "debug.profile");
		toml_datum_t threaded = toml_seek(result.toptab, "display.render_thread");

		*sample_seconds = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;
		if (watch.type == TOML_BOOLEAN)
			*memory_watch = watch.u.boolean;
		if (profiled.type == TOML_BOOLEAN)
			*profile = profiled.u.boolean;
		if (threaded.type == TOML_BOOLEAN)
			render_thread = threaded.u.boolean;
	}
	toml_free(result);
}

int host_time_zone_offset(void)
{
	TimeCalendarTime calendar;
	TimeCalendarAdditionalInfo information;
	u64 now = 0;

	if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &now)) ||
		R_FAILED(timeToCalendarTimeWithMyRule(now, &calendar, &information)))
	{
		return 0;
	}
	return information.offset;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	long offset = -(long)host_time_zone_offset();

	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, HOST_PROT_READ | HOST_PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = 0x1000;
	return (uint32_t)(uintptr_t)boot;
}

/* a file of the program's RomFS, whole; the caller frees it */
static void *program_file(const char *name, size_t *size)
{
	char path[128];
	FILE *file;
	void *data = NULL;
	long length;

	snprintf(path, sizeof(path), "romfs:/%s", name);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = memalign(0x1000, (size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

static void write_brokers(void)
{
	/* internet play's MQTT brokers (network.brokers_file): the program's
	list, written beside config.toml at each start, as a desktop update
	replaces the file beside its game */
	size_t size = 0;
	void *brokers = program_file("brokers.txt", &size);
	char path[600];
	FILE *file;

	snprintf(path, sizeof(path), "%s/brokers.txt", data_root);
	file = brokers ? fopen(path, "wb") : NULL;
	if (!file || fwrite(brokers, 1, size, file) != size)
		host_logf(HOST_LOG_WARN, "cannot write %s", path);
	if (file)
		fclose(file);
	free(brokers);
}

/* ---------- main */

/* The system keeps the buffers of a program's sockets in memory the program
gives it when it starts them: as much as the largest TCP buffers and the UDP
ones come to, sb_efficiency times (64 MB of these). All of the program's
sockets share it, and a socket that finds too little left is not made
(ENOBUFS). libnx's own numbers come to 2 MB, which internet play's tunnel
socket asks for by itself (p2p.c): the game then had no TCP socket to join a
game with. A connection of an internet game is three TCP sockets here, the
game's and the two ends of internet play's stand-in, and a host has one for
each machine. */
static const SocketInitConfig socket_configuration =
{
	.tcp_tx_buf_size = 0x8000,
	.tcp_rx_buf_size = 0x10000,
	.tcp_tx_buf_max_size = 0x400000,
	.tcp_rx_buf_max_size = 0x400000,
	.udp_tx_buf_size = 0x2400,
	.udp_rx_buf_size = 0xa500,
	.sb_efficiency = 8,
	.num_bsd_sessions = 3,
	.bsd_service_type = BsdServiceType_User,
};

/* starts the sockets; what to say of it once there is a log */
static void sockets_start(char *note, size_t note_size)
{
	Result result = socketInitialize(&socket_configuration);

	if (R_SUCCEEDED(result))
	{
		snprintf(note, note_size, "sockets: started with their larger buffers");
		return;
	}
	/* (libnx's own numbers, rather than no network: system link on a LAN
	needs few sockets) */
	if (R_SUCCEEDED(socketInitializeDefault()))
	{
		snprintf(note, note_size, "sockets: the system gave no larger buffers (0x%x): started with libnx's, "
			"which internet play can use up", (unsigned)result);
		return;
	}
	snprintf(note, note_size, "sockets: not started (0x%x): no network", (unsigned)result);
}

static void check_system(void)
{
	u64 total = 0, used = 0, address = 0, size = 0;

	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&address, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
	host_logf(HOST_LOG_INFO, "memory: %llu MB, %llu MB in use; address space %010llx-%010llx; applet type %d",
		(unsigned long long)(total >> 20), (unsigned long long)(used >> 20), (unsigned long long)address,
		(unsigned long long)(address + size), (int)appletGetAppletType());
	if (total < MINIMUM_MEMORY)
	{
		host_fatal("The game needs more memory than the album gives a program.\n\n"
			"Start the homebrew menu over a game instead: hold R while you start any game, then start OpenCE from there.");
	}
	/* (a 36-bit address space has nothing mappable at the Xbox's addresses) */
	if (address + size <= 0x1000000000ULL)
	{
		host_fatal("The homebrew loader runs in a 36-bit address space, which the game cannot use.\n\n"
			"In atmosphere/config/override_config.ini, remove override_any_app_address_space and "
			"override_address_space, or set them to 39_bit.");
	}
}

static void *game_main(void *boot)
{
	host_run_guest_main((uint32_t)(uintptr_t)boot);
}

/* everything the game needs before its first instruction; returns the
guest's description of the process (make_boot) */
static uint32_t game_prepare(void)
{
	struct environment environment = { { 0 }, 0 };
	char text[64];
	char path[600];
	size_t image_size = 0;
	void *image;
	uint32_t boot;
	double seconds;
	int profile;
	int memory_watch;
	int width = 0, height = 0;

	if (!directory_has_maps(data_root))
	{
		host_fatal("The Halo game data was not found.\n\nCopy the maps folder of the game into\nsdmc:%s\n"
			"(so that sdmc:%s/maps/ui.map is there), then start OpenCE again.", data_root, data_root);
	}

	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	/* the game renders 480 lines at the display's aspect ratio unless
	display.screen_width says otherwise (d3d8_gl.c) */
	host_display_size(&width, &height);
	if (width > 0 && height > 0)
	{
		snprintf(text, sizeof(text), "%d", (480 * width / height) & ~1);
		environment_set(&environment, "HALO_DISPLAY_WIDTH", text);
	}
	time_zone(text, sizeof(text));
	environment_set(&environment, "TZ", text);
	write_brokers();

	image = program_file("halo_guest.elf", &image_size);
	if (!image)
		host_fatal("cannot read the game image from the program's RomFS");
	if (host_load_image(image, image_size) != 0)
	{
		host_fatal("The game could not get the memory addresses it needs this time.\n\n"
			"Start OpenCE again: the system places a program's memory differently each time. "
			"The details are in opence.log, in the folder of the game data.");
	}
	free(image);

	snprintf(path, sizeof(path), "%s/config.toml", data_root);
	config_read(path, &seconds, &memory_watch, &profile);
	if (!memory_watch)
		host_memory_watch_use_hashes();
	if (seconds > 0.0)
		host_debug_start_sampler(seconds);
	if (profile)
		host_profile_start(data_root);
	boot = make_boot(&environment);
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	return boot;
}

int main(int argc, char *argv[])
{
	char path[600];
	char sockets_note[160];
	uint32_t boot;

	/* (the network first: nxlink's console is a socket; the game's lines go
	to it by host_log, and the standard error stream for what else writes,
	Mesa say. The standard output stays the screen's console) */
	sockets_start(sockets_note, sizeof(sockets_note));
	nxlink_socket = nxlinkConnectToHost(false, true);
	romfsInit();
	choose_data_root(argc > 0 ? argv[0] : NULL);
	snprintf(path, sizeof(path), "%s/opence.log", data_root);
	log_file = fopen(path, "w");
	host_logf(HOST_LOG_INFO, "OpenCE %s for Nintendo Switch starting (%s)", HALO_SWITCH_VERSION,
		argc > 0 && argv[0] ? argv[0] : "no path");
	host_logf(HOST_LOG_INFO, "%s", sockets_note);
	check_system();
	host_thread_initialize();
	host_exception_initialize();
	/* (the image before the display: what most often stops a start is the
	game data or the addresses not being there, which the screen's console
	then says while nothing else has the screen) */
	boot = game_prepare();
	if (!host_video_prepare())
		host_fatal("cannot start the display (EGL)");
	/* (before the image's OpenGL imports are filled in: they are the render
	thread's functions then) */
	if (render_thread)
		host_render_enable();
	else
		host_logf(HOST_LOG_INFO, "render thread: display.render_thread is false; the game's thread draws");
	host_link_image();
	host_run_on_guest_stack(game_main, (void *)(uintptr_t)boot, MAIN_STACK_SIZE);
	/* the game ends the process itself (host_exit) */
	host_fatal("the game's stack could not be made");
}
