/*
HOST_SDL.C

What the guest asks of SDL3 (port/android/guest/runtime/guest_sdl.c), done
with SDL2: devkitPro has SDL2 for the Switch and no SDL3. The guest is
compiled against SDL3's headers, whose names SDL2's define differently:
host_sdl3.c reads the guest's values and writes the guest's events, and this
file includes SDL2 only. Windows, contexts, gamepads and audio streams are
small handles to the guest, as on Android.

- Video is SDL2's window and its OpenGL ES context (EGL and Mesa beneath).
  The window has the size the system shows, 1920x1080 in the dock and
  1280x720 in the hand, and SDL2 resizes it when that changes. With the
  render thread (host_render.c) the context is made on that thread, which
  is then the one that shows each frame and resizes the window.
- Events are SDL2's: the keyboard and the mouse (USB ones), and the applet's
  request to close.
- Gamepads are SDL2's game controllers, the system's players 1 to 4 (player
  1 is also the console in the hand). SDL2's mapping of them is by where a
  button is: the bottom one (the Switch's B) is SDL's A, the guest's south.
- Audio is an SDL2 device, which converts the game's 48 kHz floats. SDL2
  calls for samples on a thread of its own, which cannot run guest code: it
  hands each request to a thread that has a guest stack, as the Android
  host does.

Three things SDL2's Switch drivers do not do are done with libnx here:

- which controllers are connected. SDL2 lists eight joysticks whatever is
  connected, and says nothing when one comes or goes;
- rumble. SDL2's sends one strength, to one side of the controller, and not
  to the console's own Joy-Con in the hand;
- the applet's focus, and the size of the first window.
*/

#include "host.h"
#include "host_sdl3.h"

#include <switch.h>

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the guest's handles */
#define WINDOW_HANDLE 1
#define CONTEXT_HANDLE 1
#define AUDIO_HANDLE 1

static int sdl_started;
static char error_text[256];

static void set_error(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(error_text, sizeof(error_text), format, arguments);
	va_end(arguments);
}

static uint64_t now_nanoseconds(void)
{
	return armTicksToNs(armGetSystemTick());
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	/* (host_video_prepare has started all of SDL2 the game uses) */
	(void)flags;
	return sdl_started;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	/* (SDL2 has most of SDL3's hints by the same names, and ignores the rest) */
	return SDL_SetHint(name, value) ? 1 : 0;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", error_text);
}

/* the keyboard's keys by name: SDL2's names, of the keys SDL2 and the guest
number alike */
void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	const char *name = scancode > 0 && scancode <= GUEST_SCANCODE_LAST_COMMON ?
		SDL_GetScancodeName((SDL_Scancode)scancode) : "";

	snprintf(buffer, size, "%s", name ? name : "");
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	SDL_Scancode scancode = name ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;

	return scancode <= GUEST_SCANCODE_LAST_COMMON ? (int32_t)scancode : 0;
}

int64_t host_sdl_ticks(void)
{
	static u64 start;
	u64 now = armGetSystemTick();

	if (!start)
		start = now;
	return (int64_t)(armTicksToNs(now - start) / 1000000ULL);
}

int64_t host_sdl_thread_id(void)
{
	return host_thread_id();
}

/* ---------- video */

static SDL_Window *window;
static SDL_GLContext context;
/* the thread the context is current on */
static int video_thread;
/* the size the window was last given (with the render thread, which gives
it: window_resize) */
static int shown_width, shown_height;
/* the alpha bits of the window's framebuffer */
static int surface_alpha;
static int swap_interval = 1;

/* the size the system shows: the television's in the dock, the console's
own screen's in the hand */
void host_display_size(int *width, int *height)
{
	int docked = appletGetOperationMode() == AppletOperationMode_Console;

	*width = docked ? 1920 : 1280;
	*height = docked ? 1080 : 720;
}

/* SDL2 itself, and EGL, before the image is linked: the image's OpenGL
imports are looked up through EGL (host_gl.c) */
int host_video_prepare(void)
{
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
	{
		host_logf(HOST_LOG_ERROR, "SDL does not start: %s", SDL_GetError());
		return 0;
	}
	/* a framebuffer of 8 bits a channel unless the game says otherwise, as
	SDL3 has it (SDL2's is of 3, 3 and 2), and without alpha: the game
	leaves what it likes in the alpha of its picture */
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
	if (SDL_GL_LoadLibrary(NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "SDL does not start EGL: %s", SDL_GetError());
		SDL_Quit();
		return 0;
	}
	sdl_started = 1;
	return 1;
}

/* gives the screen up for the text console (host_main.c's messages), which
only the thread that draws can do; 1 if the screen is free */
static void video_release(void *unused)
{
	(void)unused;
	if (context)
		SDL_GL_DeleteContext(context);
	if (window)
		SDL_DestroyWindow(window);
	context = NULL;
	window = NULL;
}

/* how long the render thread has to give the screen up: it may be what
stopped */
#define RELEASE_NANOSECONDS 2000000000ull

int host_video_release(void)
{
	if (!sdl_started)
		return 1;
	if (host_render_remote())
	{
		if (!host_render_call_within(video_release, NULL, RELEASE_NANOSECONDS))
			return 0;
	}
	else
	{
		if (context && video_thread != host_thread_id())
			return 0;
		video_release(NULL);
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	sdl_started = 0;
	return 1;
}

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	(void)width;
	(void)height;
	(void)flags;
	if (!sdl_started)
	{
		set_error("SDL has not started");
		return 0;
	}
	if (!window)
	{
		host_display_size(&shown_width, &shown_height);
		/* (resizable: SDL2 then gives it the other size when the console
		goes into the dock or comes out of it. It does that where it reads
		the system's messages, on the game's thread, and gives the new
		surface to the context of that thread: with the render thread, which
		has the context, the window is resized there instead, system_scan) */
		window = SDL_CreateWindow(title ? title : "Halo", 0, 0, shown_width, shown_height,
			SDL_WINDOW_OPENGL | (host_render_remote() ? 0 : SDL_WINDOW_RESIZABLE));
		if (!window)
		{
			set_error("%s", SDL_GetError());
			host_logf(HOST_LOG_ERROR, "SDL_CreateWindow: %s", error_text);
			return 0;
		}
	}
	return WINDOW_HANDLE;
}

void host_sdl_window_size_in_pixels(uint32_t handle, int *width, int *height)
{
	(void)handle;
	*width = 0;
	*height = 0;
	if (window)
		SDL_GL_GetDrawableSize(window, width, height);
	if (*width <= 0 || *height <= 0)
		host_display_size(width, height);
}

int host_sdl_set_relative_mouse(uint32_t handle, int enabled)
{
	(void)handle;
	return SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	SDL_GLattr name;

	switch (guest_gl_attribute(attribute))
	{
	case _guest_gl_red_size: name = SDL_GL_RED_SIZE; break;
	case _guest_gl_green_size: name = SDL_GL_GREEN_SIZE; break;
	case _guest_gl_blue_size: name = SDL_GL_BLUE_SIZE; break;
	case _guest_gl_alpha_size: name = SDL_GL_ALPHA_SIZE; break;
	case _guest_gl_depth_size: name = SDL_GL_DEPTH_SIZE; break;
	case _guest_gl_stencil_size: name = SDL_GL_STENCIL_SIZE; break;
	case _guest_gl_doublebuffer: name = SDL_GL_DOUBLEBUFFER; break;
	case _guest_gl_multisample_buffers: name = SDL_GL_MULTISAMPLEBUFFERS; break;
	case _guest_gl_multisample_samples: name = SDL_GL_MULTISAMPLESAMPLES; break;
	case _guest_gl_context_major_version: name = SDL_GL_CONTEXT_MAJOR_VERSION; break;
	case _guest_gl_context_minor_version: name = SDL_GL_CONTEXT_MINOR_VERSION; break;
	case _guest_gl_context_flags:
		name = SDL_GL_CONTEXT_FLAGS;
		value = guest_gl_flags_debug(value) ? SDL_GL_CONTEXT_DEBUG_FLAG : 0;
		break;
	case _guest_gl_context_profile:
		name = SDL_GL_CONTEXT_PROFILE_MASK;
		switch (guest_gl_profile(value))
		{
		case _guest_gl_profile_core: value = SDL_GL_CONTEXT_PROFILE_CORE; break;
		case _guest_gl_profile_compatibility: value = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY; break;
		default: value = SDL_GL_CONTEXT_PROFILE_ES; break;
		}
		break;
	default:
		/* (one SDL2 has not, or the game has no use of here) */
		return 1;
	}
	return SDL_GL_SetAttribute(name, value) == 0;
}

/* (the context is made, and so current, on the thread that draws: the
render thread if there is one, or the game's) */
static void context_create(void *result)
{
	int major = 0, minor = 0, alpha = 0;

	if (context)
	{
		SDL_GL_DeleteContext(context);
		context = NULL;
	}
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
	SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
	context = SDL_GL_CreateContext(window);
	if (!context)
	{
		set_error("%s", SDL_GetError());
		host_logf(HOST_LOG_WARN, "no OpenGL %d.%d context: %s", major, minor, error_text);
		return;
	}
	/* (SDL2 makes a new context current) */
	video_thread = host_thread_id();
	/* (when SDL2 cannot tell, as if it had alpha: host_sdl_gl_swap_window) */
	surface_alpha = SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &alpha) == 0 ? alpha : 8;
	host_logf(HOST_LOG_INFO, "OpenGL %d.%d context, window framebuffer with %d bits of alpha", major, minor,
		surface_alpha);
	*(uint32_t *)result = CONTEXT_HANDLE;
}

uint32_t host_sdl_gl_create_context(uint32_t handle)
{
	uint32_t result = 0;

	if (handle != WINDOW_HANDLE || !window)
		return 0;
	host_render_call(context_create, &result);
	return result;
}

int host_sdl_gl_make_current(uint32_t window_handle, uint32_t context_handle)
{
	if (!window)
		return 0;
	/* (the render thread's from when it was made, and no other's) */
	if (host_render_remote())
		return window_handle == WINDOW_HANDLE && context_handle == CONTEXT_HANDLE && context;
	if (window_handle != WINDOW_HANDLE || context_handle != CONTEXT_HANDLE || !context)
		return SDL_GL_MakeCurrent(window, NULL) == 0;
	if (SDL_GL_MakeCurrent(window, context) != 0)
	{
		set_error("%s", SDL_GetError());
		return 0;
	}
	video_thread = host_thread_id();
	return 1;
}

/* how the window's surface waits for the display, which each new surface
has to be told */
static void swap_interval_set(void *result)
{
	int set = context && SDL_GL_SetSwapInterval(swap_interval) == 0;

	if (result)
		*(int *)result = set;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	int result = 0;

	swap_interval = interval;
	host_render_call(swap_interval_set, &result);
	return result;
}

static void swap_window(const void *unused)
{
	u64 start;

	(void)unused;
	if (!window || !context)
		return;
	/* (the system shows a framebuffer that has alpha with it) */
	if (surface_alpha > 0 && video_thread == host_thread_id())
		host_gl_make_opaque();
	start = armGetSystemTick();
	SDL_GL_SwapWindow(window);
	host_profile_frame(start, armGetSystemTick());
}

int host_sdl_gl_swap_window(uint32_t handle)
{
	(void)handle;
	if (!window || !context)
		return 0;
	if (host_render_remote())
	{
		/* the frame is shown when the render thread gets to it: the game
		goes on, a frame ahead of it at most */
		host_render_begin(swap_window, 0);
		host_render_send();
		host_render_frame();
	}
	else
	{
		swap_window(NULL);
	}
	return 1;
}

/* the window's other size, on the thread that has the context: SDL2 makes
the window a new surface and makes the context current with it */
static void window_resize(void *size)
{
	SDL_SetWindowSize(window, ((const int *)size)[0], ((const int *)size)[1]);
	swap_interval_set(NULL);
}

/* ---------- gamepads */

/* the game's ports */
#define GAMEPAD_COUNT 4
/* the frequencies of the two motors an Xbox controller has, about */
#define RUMBLE_LOW_HZ 160.0f
#define RUMBLE_HIGH_HZ 320.0f

struct gamepad
{
	/* SDL2's joystick of the same number, as a game controller, or as a
	joystick if SDL2 has no mapping of it */
	SDL_GameController *controller;
	SDL_Joystick *joystick;
	int connected, opened;
	/* the kind of controller its vibration handles are for, and when its
	rumble ends (ticks; 0 when it is not rumbling) */
	u32 vibration_style;
	HidNpadIdType vibration_id;
	HidVibrationDeviceHandle vibration[2];
	u64 rumble_end;
};

static struct gamepad gamepads[GAMEPAD_COUNT];

/* SDL2's joystick i is the system's player i + 1, and the first also the
console in the hand (its Switch joystick driver) */
static HidNpadIdType gamepad_player(int index)
{
	return (HidNpadIdType)(HidNpadIdType_No1 + index);
}

static int gamepad_handheld(int index)
{
	return index == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) != 0;
}

/* whether there is a controller: SDL2 has a joystick for each player,
connected or not */
static int gamepad_present(int index)
{
	return hidGetNpadStyleSet(gamepad_player(index)) != 0 || gamepad_handheld(index);
}

static struct gamepad *gamepad_of(uint32_t handle)
{
	return handle >= 1 && handle <= GAMEPAD_COUNT ? &gamepads[handle - 1] : NULL;
}

/* the vibration handles of the controller a gamepad is now; 1 if it has them */
static int gamepad_vibration(struct gamepad *gamepad, int index)
{
	u32 styles = hidGetNpadStyleSet(gamepad_player(index));
	HidNpadIdType id = gamepad_player(index);
	HidNpadStyleTag style;

	if (styles & HidNpadStyleTag_NpadFullKey)
	{
		style = HidNpadStyleTag_NpadFullKey;
	}
	else if (styles & HidNpadStyleTag_NpadJoyDual)
	{
		style = HidNpadStyleTag_NpadJoyDual;
	}
	else if (gamepad_handheld(index))
	{
		id = HidNpadIdType_Handheld;
		style = HidNpadStyleTag_NpadHandheld;
	}
	else
	{
		return 0;
	}
	if (gamepad->vibration_style != (u32)style || gamepad->vibration_id != id)
	{
		if (R_FAILED(hidInitializeVibrationDevices(gamepad->vibration, 2, id, style)))
		{
			gamepad->vibration_style = 0;
			return 0;
		}
		gamepad->vibration_style = (u32)style;
		gamepad->vibration_id = id;
	}
	return 1;
}

static void gamepad_vibrate(struct gamepad *gamepad, int index, float low, float high)
{
	HidVibrationValue values[2];

	if (!gamepad_vibration(gamepad, index))
		return;
	values[0].amp_low = low;
	values[0].freq_low = RUMBLE_LOW_HZ;
	values[0].amp_high = high;
	values[0].freq_high = RUMBLE_HIGH_HZ;
	values[1] = values[0];
	hidSendVibrationValues(gamepad->vibration, values, 2);
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;

	for (index = 0; index < GAMEPAD_COUNT && count < capacity; index++)
	{
		if (gamepads[index].connected)
			ids[count++] = (uint32_t)index + 1;
	}
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	struct gamepad *gamepad = gamepad_of(id);
	int index = (int)id - 1;

	if (!gamepad || !gamepad->connected)
		return 0;
	if (!gamepad->controller && !gamepad->joystick)
	{
		if (SDL_IsGameController(index))
			gamepad->controller = SDL_GameControllerOpen(index);
		if (!gamepad->controller)
		{
			host_logf(HOST_LOG_WARN, "gamepad %u is not a game controller to SDL (%s): its buttons by number",
				(unsigned)id, SDL_GetError());
			gamepad->joystick = SDL_JoystickOpen(index);
		}
		if (!gamepad->controller && !gamepad->joystick)
		{
			host_logf(HOST_LOG_ERROR, "gamepad %u does not open: %s", (unsigned)id, SDL_GetError());
			return 0;
		}
	}
	if (!gamepad->opened)
	{
		host_logf(HOST_LOG_INFO, "gamepad %u: %s%s", (unsigned)id,
			gamepad->controller ? SDL_GameControllerName(gamepad->controller) : SDL_JoystickName(gamepad->joystick),
			gamepad_handheld(index) ? " (the console)" : "");
	}
	gamepad->opened = 1;
	return id;
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	struct gamepad *gamepad = gamepad_of(id);

	return gamepad && gamepad->connected && gamepad->opened ? id : 0;
}

/* (SDL2's joystick of that number stays open, for the player's next
controller: host_sdl_open_gamepad) */
void host_sdl_close_gamepad(uint32_t handle)
{
	struct gamepad *gamepad = gamepad_of(handle);

	if (gamepad)
		gamepad->opened = 0;
}

int host_sdl_gamepad_axis(uint32_t handle, int axis)
{
	static const SDL_GameControllerAxis axes[NUMBER_OF_GUEST_AXES] =
	{
		[_guest_axis_left_x] = SDL_CONTROLLER_AXIS_LEFTX,
		[_guest_axis_left_y] = SDL_CONTROLLER_AXIS_LEFTY,
		[_guest_axis_right_x] = SDL_CONTROLLER_AXIS_RIGHTX,
		[_guest_axis_right_y] = SDL_CONTROLLER_AXIS_RIGHTY,
		[_guest_axis_left_trigger] = SDL_CONTROLLER_AXIS_TRIGGERLEFT,
		[_guest_axis_right_trigger] = SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
	};
	struct gamepad *gamepad = gamepad_of(handle);
	int which = guest_gamepad_axis(axis);

	if (!gamepad || !gamepad->connected || which < 0)
		return 0;
	if (gamepad->controller)
		return SDL_GameControllerGetAxis(gamepad->controller, axes[which]);
	if (!gamepad->joystick)
		return 0;
	/* (the Switch joystick driver's numbers: the sticks are axes 0 to 3, y
	downwards as SDL has it, and ZL and ZR buttons 8 and 9) */
	if (which == _guest_axis_left_trigger || which == _guest_axis_right_trigger)
		return SDL_JoystickGetButton(gamepad->joystick, which == _guest_axis_left_trigger ? 8 : 9) ? 32767 : 0;
	return SDL_JoystickGetAxis(gamepad->joystick, which);
}

int host_sdl_gamepad_button(uint32_t handle, int button)
{
	/* SDL2's mapping of the Switch's controllers is by position: its A is
	the bottom button (the Switch's B), where the Xbox's A is */
	static const SDL_GameControllerButton buttons[NUMBER_OF_GUEST_BUTTONS] =
	{
		[_guest_button_south] = SDL_CONTROLLER_BUTTON_A,
		[_guest_button_east] = SDL_CONTROLLER_BUTTON_B,
		[_guest_button_west] = SDL_CONTROLLER_BUTTON_X,
		[_guest_button_north] = SDL_CONTROLLER_BUTTON_Y,
		[_guest_button_back] = SDL_CONTROLLER_BUTTON_BACK,
		[_guest_button_guide] = SDL_CONTROLLER_BUTTON_GUIDE,
		[_guest_button_start] = SDL_CONTROLLER_BUTTON_START,
		[_guest_button_left_stick] = SDL_CONTROLLER_BUTTON_LEFTSTICK,
		[_guest_button_right_stick] = SDL_CONTROLLER_BUTTON_RIGHTSTICK,
		[_guest_button_left_shoulder] = SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
		[_guest_button_right_shoulder] = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
		[_guest_button_dpad_up] = SDL_CONTROLLER_BUTTON_DPAD_UP,
		[_guest_button_dpad_down] = SDL_CONTROLLER_BUTTON_DPAD_DOWN,
		[_guest_button_dpad_left] = SDL_CONTROLLER_BUTTON_DPAD_LEFT,
		[_guest_button_dpad_right] = SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
	};
	/* the same by the Switch joystick driver's numbers (A 0, B 1, X 2, Y 3,
	the sticks 4 and 5, L 6, R 7, plus 10, minus 11, left 12, up 13, right
	14, down 15), for a controller SDL2 has no mapping of */
	static const int numbers[NUMBER_OF_GUEST_BUTTONS] =
	{
		[_guest_button_south] = 1,
		[_guest_button_east] = 0,
		[_guest_button_west] = 3,
		[_guest_button_north] = 2,
		[_guest_button_back] = 11,
		[_guest_button_guide] = -1,
		[_guest_button_start] = 10,
		[_guest_button_left_stick] = 4,
		[_guest_button_right_stick] = 5,
		[_guest_button_left_shoulder] = 6,
		[_guest_button_right_shoulder] = 7,
		[_guest_button_dpad_up] = 13,
		[_guest_button_dpad_down] = 15,
		[_guest_button_dpad_left] = 12,
		[_guest_button_dpad_right] = 14,
	};
	struct gamepad *gamepad = gamepad_of(handle);
	int which = guest_gamepad_button(button);

	if (!gamepad || !gamepad->connected || which < 0)
		return 0;
	if (gamepad->controller)
		return SDL_GameControllerGetButton(gamepad->controller, buttons[which]) != 0;
	if (!gamepad->joystick || numbers[which] < 0)
		return 0;
	return SDL_JoystickGetButton(gamepad->joystick, numbers[which]) != 0;
}

int host_sdl_gamepad_type(uint32_t handle)
{
	struct gamepad *gamepad = gamepad_of(handle);

	if (!gamepad || !gamepad->connected)
		return 0;
	return guest_gamepad_type((hidGetNpadStyleSet(gamepad_player((int)handle - 1)) & HidNpadStyleTag_NpadFullKey) != 0);
}

int host_sdl_rumble_gamepad(uint32_t handle, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	struct gamepad *gamepad = gamepad_of(handle);

	if (!gamepad || !gamepad->connected)
		return 0;
	if (!low && !high)
	{
		if (gamepad->rumble_end)
			gamepad_vibrate(gamepad, (int)handle - 1, 0.0f, 0.0f);
		gamepad->rumble_end = 0;
		return 1;
	}
	gamepad_vibrate(gamepad, (int)handle - 1, (float)(low > 65535 ? 65535 : low) / 65535.0f,
		(float)(high > 65535 ? 65535 : high) / 65535.0f);
	gamepad->rumble_end = armGetSystemTick() + armNsToTicks((u64)milliseconds * 1000000ULL);
	return 1;
}

/* ---------- events */

/* what this file tells the guest itself, which SDL2 does not: a controller
that came or went, and the applet's focus */
enum
{
	_notice_gamepad_added,
	_notice_gamepad_removed,
	_notice_focus_gained,
	_notice_focus_lost,
};

#define NOTICE_QUEUE_SIZE 16

static struct
{
	int type;
	uint32_t id;
} notices[NOTICE_QUEUE_SIZE];
static int notice_first, notice_count;
/* 1 once the system's state has been looked at since SDL2's queue last ran
dry: the game reads events until there are none, each frame */
static int system_scanned;
static AppletFocusState focus_state = AppletFocusState_InFocus;

static void notice_push(int type, uint32_t id)
{
	if (notice_count == NOTICE_QUEUE_SIZE)
		return;
	notices[(notice_first + notice_count) % NOTICE_QUEUE_SIZE].type = type;
	notices[(notice_first + notice_count) % NOTICE_QUEUE_SIZE].id = id;
	notice_count++;
}

static int notice_pop(void *event)
{
	int type;
	uint32_t id;

	if (!notice_count)
		return 0;
	type = notices[notice_first].type;
	id = notices[notice_first].id;
	notice_first = (notice_first + 1) % NOTICE_QUEUE_SIZE;
	notice_count--;
	switch (type)
	{
	case _notice_gamepad_added: guest_event_gamepad(event, now_nanoseconds(), 1, id); break;
	case _notice_gamepad_removed: guest_event_gamepad(event, now_nanoseconds(), 0, id); break;
	case _notice_focus_gained: guest_event_focus(event, now_nanoseconds(), 1); break;
	default: guest_event_focus(event, now_nanoseconds(), 0); break;
	}
	return 1;
}

static void system_scan(void)
{
	u64 now = armGetSystemTick();
	AppletFocusState focus = appletGetFocusState();
	int index;

	if ((focus == AppletFocusState_InFocus) != (focus_state == AppletFocusState_InFocus))
		notice_push(focus == AppletFocusState_InFocus ? _notice_focus_gained : _notice_focus_lost, 0);
	focus_state = focus;
	/* into the dock or out of it, with the render thread (without, SDL2 has
	resized the window by now: host_sdl_create_window) */
	if (window && host_render_remote())
	{
		int size[2];

		host_display_size(&size[0], &size[1]);
		if (size[0] != shown_width || size[1] != shown_height)
		{
			shown_width = size[0];
			shown_height = size[1];
			host_render_call(window_resize, size);
		}
	}
	for (index = 0; index < GAMEPAD_COUNT; index++)
	{
		struct gamepad *gamepad = &gamepads[index];
		int connected = gamepad_present(index);

		if (connected != gamepad->connected)
		{
			notice_push(connected ? _notice_gamepad_added : _notice_gamepad_removed, (uint32_t)index + 1);
			gamepad->connected = connected;
			if (!connected)
			{
				gamepad->opened = 0;
				gamepad->rumble_end = 0;
				gamepad->vibration_style = 0;
			}
		}
		if (gamepad->rumble_end && now >= gamepad->rumble_end)
		{
			gamepad->rumble_end = 0;
			gamepad_vibrate(gamepad, index, 0.0f, 0.0f);
		}
	}
}

/* an SDL2 event as the guest's SDL3 one; 0 if it has none */
static int event_translate(const SDL_Event *event, void *guest)
{
	uint64_t time = now_nanoseconds();

	switch (event->type)
	{
	case SDL_QUIT:
		guest_event_quit(guest, time);
		return 1;
	case SDL_WINDOWEVENT:
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			guest_event_focus(guest, time, 1);
			return 1;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			guest_event_focus(guest, time, 0);
			return 1;
		case SDL_WINDOWEVENT_SIZE_CHANGED:
			/* the dock: the game asks for the size each frame. The
			window has a new surface, which has not been told how to wait
			for the display */
			host_logf(HOST_LOG_INFO, "display %dx%d", (int)event->window.data1, (int)event->window.data2);
			host_render_call(swap_interval_set, NULL);
			return 0;
		default:
			return 0;
		}
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		if (event->key.keysym.scancode > GUEST_SCANCODE_LAST_COMMON)
			return 0;
		guest_event_key(guest, time, (int)event->key.keysym.scancode, event->type == SDL_KEYDOWN, event->key.repeat != 0);
		return 1;
	case SDL_MOUSEMOTION:
		if (event->motion.which == SDL_TOUCH_MOUSEID)
			return 0;
		guest_event_mouse_motion(guest, time, event->motion.state, (float)event->motion.x, (float)event->motion.y,
			(float)event->motion.xrel, (float)event->motion.yrel);
		return 1;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		if (event->button.which == SDL_TOUCH_MOUSEID)
			return 0;
		guest_event_mouse_button(guest, time, event->button.button, event->type == SDL_MOUSEBUTTONDOWN,
			(float)event->button.x, (float)event->button.y);
		return 1;
	case SDL_MOUSEWHEEL:
	{
		float direction = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

		if (event->wheel.which == SDL_TOUCH_MOUSEID)
			return 0;
		guest_event_mouse_wheel(guest, time, event->wheel.preciseX * direction, event->wheel.preciseY * direction);
		return 1;
	}
	default:
		/* (SDL2's own notices of controllers among them: it has one of each
		of its eight joysticks at the start, and none afterwards) */
		return 0;
	}
}

int host_sdl_poll_event(void *event)
{
	SDL_Event next;

	if (!sdl_started)
		return 0;
	if (notice_pop(event))
		return 1;
	/* (SDL2 reads the applet's messages, the keyboard, the mouse and the
	controllers when its queue is empty) */
	while (SDL_PollEvent(&next))
	{
		if (event_translate(&next, event))
			return 1;
	}
	if (!system_scanned)
	{
		system_scanned = 1;
		system_scan();
		if (notice_pop(event))
			return 1;
	}
	system_scanned = 0;
	return 0;
}

/* ---------- audio */

/* frames in each buffer of the device: the sound is about two of them behind
the game */
#define AUDIO_BUFFER_FRAMES 1024

/* SDL2 calls audio_callback on its own audio thread, which cannot run guest
code; it passes each request to audio_thread, which can, and waits for it to
be done. The game's callback puts its samples in the stream (which is the
buffer here) while it runs, and audio_callback takes them from there. */
struct audio_stream
{
	SDL_AudioDeviceID device;
	int open, channels;
	uint32_t callback, userdata;
	Mutex lock;
	CondVar requested, done;
	int pending;
	/* bytes the device is short of */
	uint32_t wanted;
	/* the game's samples (32-bit floats, interleaved), until they are played */
	float *samples;
	size_t sample_count, sample_capacity;
};

static struct audio_stream audio;
/* audio_thread runs, and audio's lock is made: once, however many times the
game opens the device (host_sdl_destroy_audio_stream) */
static int audio_started;

/* whether the game's callback is running on this thread */
static __thread int in_audio_callback;

static void *audio_thread(void *unused)
{
	(void)unused;
	host_thread_raise();
	host_debug_thread_name("mixer");
	mutexLock(&audio.lock);
	for (;;)
	{
		uint32_t wanted;
		u64 start;

		while (!audio.pending)
			condvarWait(&audio.requested, &audio.lock);
		wanted = audio.wanted;
		mutexUnlock(&audio.lock);
		/* void callback(void *userdata, SDL_AudioStream *stream, int additional, int total) */
		in_audio_callback = 1;
		start = armGetSystemTick();
		host_call_guest(audio.callback, audio.userdata, AUDIO_HANDLE, wanted, wanted);
		host_profile_add(_host_profile_audio, armGetSystemTick() - start, wanted);
		in_audio_callback = 0;
		mutexLock(&audio.lock);
		audio.pending = 0;
		condvarWakeOne(&audio.done);
	}
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
	static __thread int raised;
	size_t wanted = length > 0 ? (size_t)length / sizeof(float) : 0, taken;

	(void)userdata;
	if (!raised)
	{
		raised = 1;
		host_thread_raise();
		host_debug_thread_name("sdl audio");
	}
	mutexLock(&audio.lock);
	if (audio.callback && audio.sample_count < wanted)
	{
		audio.wanted = (uint32_t)((wanted - audio.sample_count) * sizeof(float));
		audio.pending = 1;
		condvarWakeOne(&audio.requested);
		while (audio.pending)
			condvarWait(&audio.done, &audio.lock);
	}
	taken = audio.sample_count < wanted ? audio.sample_count : wanted;
	memcpy(stream, audio.samples, taken * sizeof(float));
	memmove(audio.samples, audio.samples + taken, (audio.sample_count - taken) * sizeof(float));
	audio.sample_count -= taken;
	mutexUnlock(&audio.lock);
	/* (silence for what the game did not supply) */
	if (length > 0)
		memset(stream + taken * sizeof(float), 0, (size_t)length - taken * sizeof(float));
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	SDL_AudioSpec wanted, obtained;
	int channels = 0, frequency = 0;

	(void)device;
	/* (a stream without a callback is voice chat's microphone, which the
	game reads: port/linux/src/voice_audio.c) */
	if (!callback)
	{
		set_error("this build does not record sound: voice chat has no microphone");
		return 0;
	}
	if (audio.open)
	{
		set_error("the audio device is open already");
		return 0;
	}
	if (!guest_audio_spec(spec, &channels, &frequency) || channels < 1 || channels > 2 || frequency <= 0)
	{
		set_error("the audio device takes 32-bit float samples, mono or stereo");
		return 0;
	}
	if (!audio_started)
	{
		mutexInit(&audio.lock);
		condvarInit(&audio.requested);
		condvarInit(&audio.done);
	}
	audio.callback = callback;
	audio.userdata = userdata;
	audio.channels = channels;
	SDL_zero(wanted);
	wanted.freq = frequency;
	wanted.format = AUDIO_F32SYS;
	wanted.channels = (Uint8)channels;
	wanted.samples = AUDIO_BUFFER_FRAMES;
	wanted.callback = audio_callback;
	/* (the game's format whatever the device's is: SDL2 converts) */
	audio.device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
	if (!audio.device)
	{
		set_error("%s", SDL_GetError());
		return 0;
	}
	audio.open = 1;
	/* (the device starts paused, as SDL3's: the callback first runs after
	SDL_ResumeAudioStreamDevice) */
	if (!audio_started)
	{
		if (host_native_thread_create(audio_thread, NULL, 256 * 1024) != 0)
			host_fatal("cannot start the audio thread");
		audio_started = 1;
	}
	return AUDIO_HANDLE;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	size_t count = length > 0 ? (size_t)length / sizeof(float) : 0;
	/* (what is not played is the latest second at most) */
	const size_t maximum = (size_t)48000 * 2;
	int result = 1;

	if (stream != AUDIO_HANDLE || !audio.open || length < 0)
		return 0;
	mutexLock(&audio.lock);
	if (audio.sample_count + count > audio.sample_capacity)
	{
		size_t capacity = (audio.sample_count + count) * 2;
		float *samples = realloc(audio.samples, capacity * sizeof(float));

		if (samples)
		{
			audio.samples = samples;
			audio.sample_capacity = capacity;
		}
		else
		{
			result = 0;
		}
	}
	if (result)
	{
		memcpy(audio.samples + audio.sample_count, data, count * sizeof(float));
		audio.sample_count += count;
		if (audio.sample_count > maximum && !in_audio_callback)
		{
			size_t excess = audio.sample_count - maximum;

			excess -= excess % (size_t)audio.channels;
			memmove(audio.samples, audio.samples + excess, (audio.sample_count - excess) * sizeof(float));
			audio.sample_count -= excess;
		}
	}
	mutexUnlock(&audio.lock);
	return result;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	if (stream != AUDIO_HANDLE || !audio.open)
		return 0;
	SDL_PauseAudioDevice(audio.device, 0);
	return 1;
}

/* (the microphone's stream, of which there is none: host_sdl_open_audio_stream) */
int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	(void)stream;
	(void)data;
	(void)length;
	return -1;
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	(void)stream;
	return -1;
}

/* The game closes the device to open another (audio.output_device, which
the desktop builds have). audio_thread stays, for the next device's
requests: none is pending once SDL2 has closed this one. */
void host_sdl_destroy_audio_stream(uint32_t stream)
{
	if (stream != AUDIO_HANDLE || !audio.open)
		return;
	/* (returns once a callback that is running has finished) */
	SDL_CloseAudioDevice(audio.device);
	mutexLock(&audio.lock);
	audio.open = 0;
	audio.device = 0;
	audio.sample_count = 0;
	mutexUnlock(&audio.lock);
}

/* ---------- the Android app's touch controls

The Android app draws controls over the game and reads the fingers on them
(port/android/host/host_touch.c); the guest asks for them each time it reads
its controllers (port/linux/src/touch_input.c). This program has none: the
console has its own controllers. Nothing is ever held, and the screen has no
edges that the system keeps for its gestures. */

void host_gesture_insets(int32_t *insets)
{
	memset(insets, 0, 4 * sizeof(*insets));
}

void host_touch_read(int32_t *state)
{
	memset(state, 0, 7 * sizeof(*state));
}

void host_touch_look_read(float *delta)
{
	memset(delta, 0, 4 * sizeof(*delta));
}

void host_touch_rumble(uint32_t low, uint32_t high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	(void)scene;
}

void host_touch_bindings(const int32_t *controls)
{
	(void)controls;
}

/* ---------- the clipboard (internet play's invite links): the game's own,
as the system has none for programs */

static char clipboard[1024];
static Mutex clipboard_lock;

int host_sdl_set_clipboard_text(const char *text)
{
	mutexLock(&clipboard_lock);
	snprintf(clipboard, sizeof(clipboard), "%s", text ? text : "");
	mutexUnlock(&clipboard_lock);
	return 1;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	mutexLock(&clipboard_lock);
	snprintf(buffer, size, "%s", clipboard);
	mutexUnlock(&clipboard_lock);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "%s", message);
	return 1;
}

/* ---------- a message for the player (a host of another network version):
the log has it; the screen is the game's */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	host_logf(guest_message_is_error(flags) ? HOST_LOG_ERROR : HOST_LOG_WARN, "%s: %s", title ? title : "",
		message ? message : "");
	return 1;
}
