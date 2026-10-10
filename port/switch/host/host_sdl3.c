/*
HOST_SDL3.C

The guest's SDL3, for the host's SDL2 (host_sdl3.h has why they are apart).
This file includes SDL3's headers and nothing of SDL2's: it reads the
guest's constants and writes the guest's events with SDL3's own
definitions.
*/

#include "host_sdl3.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_messagebox.h>

#include <string.h>

/* (the guest's window: host_sdl.c's handle of it) */
#define WINDOW_ID 1

_Static_assert(sizeof(SDL_Event) == 128, "the guest's SDL_Event is 128 bytes");
_Static_assert(SDL_SCANCODE_RGUI == GUEST_SCANCODE_LAST_COMMON, "the last key SDL2 and SDL3 number alike");

int guest_gl_attribute(int attribute)
{
	switch (attribute)
	{
	case SDL_GL_RED_SIZE: return _guest_gl_red_size;
	case SDL_GL_GREEN_SIZE: return _guest_gl_green_size;
	case SDL_GL_BLUE_SIZE: return _guest_gl_blue_size;
	case SDL_GL_ALPHA_SIZE: return _guest_gl_alpha_size;
	case SDL_GL_DEPTH_SIZE: return _guest_gl_depth_size;
	case SDL_GL_STENCIL_SIZE: return _guest_gl_stencil_size;
	case SDL_GL_DOUBLEBUFFER: return _guest_gl_doublebuffer;
	case SDL_GL_MULTISAMPLEBUFFERS: return _guest_gl_multisample_buffers;
	case SDL_GL_MULTISAMPLESAMPLES: return _guest_gl_multisample_samples;
	case SDL_GL_CONTEXT_MAJOR_VERSION: return _guest_gl_context_major_version;
	case SDL_GL_CONTEXT_MINOR_VERSION: return _guest_gl_context_minor_version;
	case SDL_GL_CONTEXT_FLAGS: return _guest_gl_context_flags;
	case SDL_GL_CONTEXT_PROFILE_MASK: return _guest_gl_context_profile;
	default: return _guest_gl_other;
	}
}

int guest_gl_profile(int value)
{
	switch (value)
	{
	case SDL_GL_CONTEXT_PROFILE_CORE: return _guest_gl_profile_core;
	case SDL_GL_CONTEXT_PROFILE_COMPATIBILITY: return _guest_gl_profile_compatibility;
	case SDL_GL_CONTEXT_PROFILE_ES: return _guest_gl_profile_es;
	default: return _guest_gl_profile_other;
	}
}

int guest_gl_flags_debug(int value)
{
	return (value & SDL_GL_CONTEXT_DEBUG_FLAG) != 0;
}

int guest_gamepad_axis(int axis)
{
	switch (axis)
	{
	case SDL_GAMEPAD_AXIS_LEFTX: return _guest_axis_left_x;
	case SDL_GAMEPAD_AXIS_LEFTY: return _guest_axis_left_y;
	case SDL_GAMEPAD_AXIS_RIGHTX: return _guest_axis_right_x;
	case SDL_GAMEPAD_AXIS_RIGHTY: return _guest_axis_right_y;
	case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return _guest_axis_left_trigger;
	case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return _guest_axis_right_trigger;
	default: return -1;
	}
}

int guest_gamepad_button(int button)
{
	switch (button)
	{
    case SDL_GAMEPAD_BUTTON_SOUTH: return _guest_button_east;
    case SDL_GAMEPAD_BUTTON_EAST:  return _guest_button_south;
    case SDL_GAMEPAD_BUTTON_WEST:  return _guest_button_north;
    case SDL_GAMEPAD_BUTTON_NORTH: return _guest_button_west;
	
	case SDL_GAMEPAD_BUTTON_BACK: return _guest_button_back;
	case SDL_GAMEPAD_BUTTON_GUIDE: return _guest_button_guide;
	case SDL_GAMEPAD_BUTTON_START: return _guest_button_start;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return _guest_button_left_stick;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return _guest_button_right_stick;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return _guest_button_left_shoulder;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return _guest_button_right_shoulder;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return _guest_button_dpad_up;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return _guest_button_dpad_down;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return _guest_button_dpad_left;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return _guest_button_dpad_right;
	default: return -1;
	}
}

int guest_gamepad_type(int pro_controller)
{
	return pro_controller ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO : SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
}

int guest_audio_spec(const void *spec, int *channels, int *frequency)
{
	const SDL_AudioSpec *format = spec;

	if (!format)
		return 0;
	*channels = format->channels;
	*frequency = format->freq;
	return format->format == SDL_AUDIO_F32;
}

int guest_message_is_error(uint32_t flags)
{
	return (flags & SDL_MESSAGEBOX_ERROR) != 0;
}

/* ---------- events */

static SDL_Event *begin(void *buffer, SDL_EventType type, uint64_t time)
{
	SDL_Event *event = buffer;

	memset(event, 0, sizeof(*event));
	event->common.type = type;
	event->common.timestamp = time;
	return event;
}

void guest_event_quit(void *buffer, uint64_t time)
{
	begin(buffer, SDL_EVENT_QUIT, time);
}

void guest_event_focus(void *buffer, uint64_t time, int gained)
{
	SDL_Event *event = begin(buffer, gained ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST, time);

	event->window.windowID = WINDOW_ID;
}

void guest_event_key(void *buffer, uint64_t time, int scancode, int down, int repeat)
{
	SDL_Event *event = begin(buffer, down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, time);

	event->key.windowID = WINDOW_ID;
	event->key.scancode = (SDL_Scancode)scancode;
	event->key.down = down != 0;
	event->key.repeat = repeat != 0;
}

void guest_event_mouse_motion(void *buffer, uint64_t time, uint32_t buttons, float x, float y, float dx, float dy)
{
	SDL_Event *event = begin(buffer, SDL_EVENT_MOUSE_MOTION, time);

	event->motion.windowID = WINDOW_ID;
	/* (the masks of the buttons are SDL2's too: the left one is bit 0) */
	event->motion.state = buttons;
	event->motion.x = x;
	event->motion.y = y;
	event->motion.xrel = dx;
	event->motion.yrel = dy;
}

void guest_event_mouse_button(void *buffer, uint64_t time, int button, int down, float x, float y)
{
	SDL_Event *event = begin(buffer, down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP, time);

	event->button.windowID = WINDOW_ID;
	/* (the buttons' numbers are SDL2's too: left 1, middle 2, right 3) */
	event->button.button = (Uint8)button;
	event->button.down = down != 0;
	event->button.clicks = 1;
	event->button.x = x;
	event->button.y = y;
}

void guest_event_mouse_wheel(void *buffer, uint64_t time, float x, float y)
{
	SDL_Event *event = begin(buffer, SDL_EVENT_MOUSE_WHEEL, time);

	event->wheel.windowID = WINDOW_ID;
	event->wheel.x = x;
	event->wheel.y = y;
	event->wheel.direction = SDL_MOUSEWHEEL_NORMAL;
	event->wheel.integer_x = (Sint32)x;
	event->wheel.integer_y = (Sint32)y;
}

void guest_event_gamepad(void *buffer, uint64_t time, int added, uint32_t id)
{
	SDL_Event *event = begin(buffer, added ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED, time);

	event->gdevice.which = (SDL_JoystickID)id;
}
