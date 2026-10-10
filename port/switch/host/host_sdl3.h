/*
HOST_SDL3.H

The guest's SDL, for the host's (host_sdl3.c).

The guest is compiled against SDL3's headers: its events have SDL3's layout
and its constants SDL3's values. The host's SDL is SDL2 (host_sdl.c), whose
headers define the same names differently, so no file can include both.
host_sdl3.c includes SDL3's, and gives host_sdl.c what the guest means in
values of neither: the ones below.
*/

#ifndef __HALO_SWITCH_HOST_SDL3_H
#define __HALO_SWITCH_HOST_SDL3_H

#include <stdint.h>

/* an SDL_GLAttr */
enum
{
	_guest_gl_other,
	_guest_gl_red_size,
	_guest_gl_green_size,
	_guest_gl_blue_size,
	_guest_gl_alpha_size,
	_guest_gl_depth_size,
	_guest_gl_stencil_size,
	_guest_gl_doublebuffer,
	_guest_gl_multisample_buffers,
	_guest_gl_multisample_samples,
	_guest_gl_context_major_version,
	_guest_gl_context_minor_version,
	_guest_gl_context_flags,
	_guest_gl_context_profile,
};

/* an SDL_GL_CONTEXT_PROFILE_MASK value */
enum
{
	_guest_gl_profile_other,
	_guest_gl_profile_core,
	_guest_gl_profile_compatibility,
	_guest_gl_profile_es,
};

/* an SDL_GamepadAxis */
enum
{
	_guest_axis_left_x,
	_guest_axis_left_y,
	_guest_axis_right_x,
	_guest_axis_right_y,
	_guest_axis_left_trigger,
	_guest_axis_right_trigger,
	NUMBER_OF_GUEST_AXES,
};

/* an SDL_GamepadButton, by its place on the controller */
enum
{
	_guest_button_south,
	_guest_button_east,
	_guest_button_west,
	_guest_button_north,
	_guest_button_back,
	_guest_button_guide,
	_guest_button_start,
	_guest_button_left_stick,
	_guest_button_right_stick,
	_guest_button_left_shoulder,
	_guest_button_right_shoulder,
	_guest_button_dpad_up,
	_guest_button_dpad_down,
	_guest_button_dpad_left,
	_guest_button_dpad_right,
	NUMBER_OF_GUEST_BUTTONS,
};

/* the keys whose scancodes SDL2 and SDL3 number alike: all of a keyboard's
(the USB usages), up to the right GUI key */
#define GUEST_SCANCODE_LAST_COMMON 231

/* each of these gives the value above for the guest's, or -1 */
int guest_gl_attribute(int attribute);
int guest_gl_profile(int value);
int guest_gamepad_axis(int axis);
int guest_gamepad_button(int button);
/* whether an SDL_GL_CONTEXT_FLAGS value asks for a debug context */
int guest_gl_flags_debug(int value);
/* the guest's SDL_GamepadType of the Switch's controllers */
int guest_gamepad_type(int pro_controller);
/* 1 if an SDL_AudioSpec is of 32-bit float samples, with its channels and
its rate */
int guest_audio_spec(const void *spec, int *channels, int *frequency);
/* whether SDL_MessageBoxFlags say an error */
int guest_message_is_error(uint32_t flags);

/* an SDL_Event in the guest's buffer (time: nanoseconds) */
void guest_event_quit(void *event, uint64_t time);
void guest_event_focus(void *event, uint64_t time, int gained);
void guest_event_key(void *event, uint64_t time, int scancode, int down, int repeat);
void guest_event_mouse_motion(void *event, uint64_t time, uint32_t buttons, float x, float y, float dx, float dy);
void guest_event_mouse_button(void *event, uint64_t time, int button, int down, float x, float y);
void guest_event_mouse_wheel(void *event, uint64_t time, float x, float y);
void guest_event_gamepad(void *event, uint64_t time, int added, uint32_t id);

#endif
