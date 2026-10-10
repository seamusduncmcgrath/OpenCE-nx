# Nintendo Switch

`ninja switch` builds the game as a homebrew program for the Nintendo
Switch: `build/switch/OpenCE.nro`.

The game shows its graphics with OpenGL ES 3 (Mesa). It plays sound and
reads the controllers through SDL2. devkitPro has SDL2 for the Switch, and
no SDL3. The program uses libnx for the other functions of the system.

**Status: the game runs on a console**, with the SDL2 host, at 1280x720 in
handheld mode and at 1920x1080 in the dock, and joins internet games. The
tests were made with older sources of the game. The build from the sources
of this repository is not tested on a console. Refer to "The tests".

## Requirements

To play:

- A Switch that runs homebrew with Atmosphère.
- The homebrew menu started over a game: hold R while you start a game.
  The homebrew menu of the album gives a program too little memory.
- The default address space of Atmosphère for homebrew (39 bits). If
  `atmosphere/config/override_config.ini` has `override_address_space` or
  `override_any_app_address_space`, they must be `39_bit`.

To build, you need the tools of the Linux build (Python, ninja) and these
items:

- devkitPro with devkitA64, libnx, Mesa and SDL2 (`dkp-pacman -S switch-dev
  switch-mesa switch-sdl2`). `configure.py` looks for it in `DEVKITPRO`, then in
  `C:/devkitPro` and `/opt/devkitpro`. The option `--devkitpro` selects a
  different folder.
- A clang with the `arm64_32` target, with `ld.lld`, `llvm-ar` and
  `llvm-objcopy` in the same folder. The releases of LLVM have them.
  `configure.py` looks for `clang` in `PATH`, then in `Program Files/LLVM`.
  The option `--switch-guest-cc` selects a different compiler. The
  optimization profile needs clang 22 or later.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 and the headers of SDL 3.4.16 to `build/switch/third_party`.

You do not need the Android NDK or the Xbox SDK.

## Build and install the program

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja switch`.
4. Make the folder `switch/OpenCE` on the SD card.
5. Copy `build/switch/OpenCE.nro` to that folder.

`ninja switch_guest` builds only the game image, `build/switch/halo_guest.elf`.

## Game data

The game needs the `maps/` folder from an Xbox disc image of any version of
the game. The program does not extract a disc image. Extract `maps/` on a
computer (the Linux and Windows builds do it), and copy it to the SD card:

| Item | Location on the SD card |
| --- | --- |
| The program | `switch/OpenCE/OpenCE.nro` |
| Game data | `switch/OpenCE/maps/` |
| Saved games (`z:\` and `u:\`) | `switch/OpenCE/save/` |
| Settings | `switch/OpenCE/config.toml` |
| Log of the game | `switch/OpenCE/debug.txt` |
| Log of the program | `switch/OpenCE/opence.log` |
| Report of a crash | `switch/OpenCE/crash.txt` |

The program uses the folder that it is in. If that folder has no `maps/`,
the program uses `switch/OpenCE`.

## Controls

The first controller, or the console in handheld mode, is player 1. The
other controllers are players 2 to 4 (split screen). The buttons agree with
the positions on the Xbox controller, not with the letters:

| Switch | Xbox | Function in the game |
| --- | --- | --- |
| left stick, right stick | left stick, right stick | move, look |
| ZR | right trigger | fire |
| ZL | left trigger | throw a grenade |
| B (bottom) | A | jump, accept |
| A (right) | B | melee, back |
| Y (left) | X | action, reload |
| X (top) | Y | change the weapon |
| L | white | flashlight |
| R | black | change the grenade |
| stick clicks | stick clicks | crouch, zoom |
| D-pad | D-pad | |
| + | start | pause menu |
| - | back | |

The controller gets the rumble. A USB keyboard and a USB mouse operate as
on Linux (refer to [port/linux/README.md](../linux/README.md#controls)).
The game needs two sticks: a single Joy-Con held sideways does not operate
it. The screen does not accept touch input.

If the view goes down when you push the right stick up, the profile has
"Invert Look" on. This is a setting of the game, for each profile: the first
campaign level sets it from the direction in which you first move the stick.
To change it, go to Settings, then the player profile, then the gamepad
setup, and set INVERT LOOK.

When the type of a controller changes (for example, when you remove the
Joy-Con from the console), SDL2 shows the controller dialog of the system.

To stop the game, close it from the HOME menu. When the game stops, the
program ends: the console goes back to the HOME menu, not to the homebrew
menu.

## Display

The game draws its picture at the resolution of the display: 1920x1080 in
the dock and 1280x720 in handheld mode. When the console goes into the dock
or comes out of it, SDL2 changes the size of the window, and the game
changes the size of its picture at the next frame. `opence.log` shows the
size, for example `screen: 852x480 drawn at 1920x1080`.

If the game is slow, decrease the number of lines that it draws with
`display.docked_height` and `display.handheld_height` (refer to
"Settings"). The program then scales the picture to the display. With 480
lines, the game draws 852x480, as the Android build does.

The layout of the game is 480 lines in the shape of the display, as on
Android (refer to "Widescreen" in
[port/android/README.md](../android/README.md#widescreen)). The 3D view, the
text and the HUD get the pixels of the display.

## Shader programs

The game makes a shader program for each combination of materials and
effects that it draws. On the Switch, Mesa takes 0.1 s or more to compile
one program, and it does not keep the result when the game stops. Thus
the game stopped for a moment each time it drew something new, in every
session.

The program writes each shader program that the game uses to
`shader_list.bin` in the folder of the game data. At the start of the
next session, it compiles all the programs of that file before the first
frame. A bar shows the progress. The start is longer, and the game does not
stop for those programs.

- The first session after you install a new build has no list: the game
  stops for each new program, as before. A list from a different build is
  not used.
- When the game draws something for the first time in a later session, it
  stops one time, and adds the program to the list.
- To start without this step, set `precompile_shaders = false` in
  `[display]` of `config.toml`.
- `opence.log` shows the number of programs and the time, for example
  `shader list: 240 programs of earlier runs made in 21.4 s`.

## Settings

The settings are in `config.toml` in the folder of the game data. At the
first start, the game writes the file with the default values. To get the
default values again, delete the file.

The settings are the settings of Android. Refer to
[port/android/README.md](../android/README.md#settings) and
[port/linux/README.md](../linux/README.md#settings). These settings are only
for the Switch:

| Setting | Function |
| --- | --- |
| `display.precompile_shaders` | `true` (the default): the program compiles the shader programs of earlier sessions at the start. Refer to "Shader programs". |
| `display.render_thread` | `true` (the default): a second thread, on a different core, makes the OpenGL calls of the game. `false`: the thread of the game makes them. Use `false` if the picture is incorrect or the game stops. Refer to "Render thread" in "How the port operates". |
| `display.docked_height` | The number of lines of pixels that the game draws in the dock. `1080` (the default): the resolution of the television, 1920x1080. `720`: 1280x720. `480`: 852x480. |
| `display.handheld_height` | The number of lines of pixels that the game draws in handheld mode. `720` (the default): the resolution of the screen, 1280x720. `480`: 852x480. |
| `debug.profile` | `true`: the program measures where the time goes. Refer to "Find a performance problem". The default is `false`. This is not the profiling build of `configure.py --profile`. |

`debug.sample_seconds` and `debug.memory_watch` of Android operate on the
Switch also. The settings of the touch controls of Android
(`input.touch_controls`, `input.touch_aim_assist`) are not in the file: the
program has no touch controls.

## Internet play

System link and internet play use the same code as Android. There is no
UPnP and no Discord. The Switch has no clipboard: an invite link stays in
the game.

On a console, the list of internet games and a join operate. System link
on a local network and hosting are not tested.

If a join fails, look for the lines that start with `sockets:` in
`opence.log`. The first of them shows if the system gave the program the
larger socket memory.

## How the port operates

The Switch build is the Android build with a different host. Refer to
[port/android/README.md](../android/README.md#how-the-port-operates) for the
guest: the game as ILP32 AArch64 code, with a part of musl as its C
library. The guest image of the Switch is built from the same sources
(`port/android/guest`, `port/linux/src`) with the three macros of the
Android guest (`HALO_ARM64_GUEST`, `HALO_GLES`, `HALO_ANDROID`) and with
`HALO_SWITCH`. Code for the Switch only is in `#ifdef HALO_SWITCH`. The NRO
contains the image in its RomFS.

The guest calls the host through the imports of
`port/android/host_imports.list`. When the Android build gets a new import,
the Switch host must have a function with that name, or the program does
not link. The imports for the touch controls of the Android app are in
`host_sdl.c`: they do nothing.

The host is `port/switch/host`, a libnx program:

- **Memory** (`host_memory.c`). A Horizon process cannot ask for memory at
  an address. But it can map pages of its heap again at an address, as a
  program loader does (`svcMapProcessCodeMemory`). hbloader gives each
  program a handle to its process, which that call needs. The host maps the
  Xbox memory at `0x80000000`, the image at `0x88000000`, and pools for the
  other memory of the guest below 4 GB that way. `svcSetMemoryPermission`
  changes the protection of this memory.
- **Image** (`host_loader.c`). The host puts the segments of the image in
  heap pages and fills the import table. Then it maps the pages. The pages
  of code can be written only before they are mapped.
- **Exceptions** (`host_exception_asm.S`, `host_exception.c`). The renderer
  write-protects the memory of the textures that it keeps. Horizon sends a
  data abort to the entry point of the program. The host replaces the
  exception entry of libnx: it saves the registers, makes the page writable,
  and continues the thread with `svcReturnFromException`. Other exceptions
  go to the crash report. With `debug.memory_watch = false`, the host
  protects no page. It compares the contents of each page one time for each
  frame, with the code of the Android host
  (`port/android/host/host_watch_hash.c`).
- **Threads** (`host_thread.c`, `host_thread_asm.S`). libnx puts the stack
  of a thread above 4 GB. Each thread that runs guest code starts on such a
  stack and then moves its stack pointer to a stack in guest memory. A
  program can use cores 0 to 2. The main thread runs the game, and has one
  core. All the other threads use the other two cores: the system does not
  stop a thread for a different thread of the same priority, thus a thread
  on the core of the main thread stops the game while it runs.
- **Render thread** (`host_render.c`, `tools/switch_gl_queue.py`). The game
  draws on the thread that runs it. In a busy scene, a third of the time of
  that thread was in the OpenGL functions of Mesa, and the thread had no
  time left. With `display.render_thread`, a second thread has the OpenGL
  context:
  - Each OpenGL import of the guest is a function that writes the call into
    a queue and returns. `tools/switch_gl_queue.py` writes these functions
    from the OpenGL ES headers.
  - If a call gives memory to OpenGL (the values of a uniform, vertices, the
    pixels of a texture), the queue gets a copy of the memory. If the
    memory is more than 2 MB, or the call returns a value, the thread of
    the game waits until the render thread made the call.
  - The queue is 16 MB of memory that one thread writes and one thread
    reads. The two threads use no lock to move a call. A thread that has
    nothing to do looks for new work for a short time and then sleeps.
  - To show a frame is also a call in the queue. The thread of the game
    continues with the next frame. It waits only if the render thread did
    not yet show the frame before.
  - The render thread makes the context, shows each frame, and changes the
    size of the window for the dock. SDL2 changes the size on the thread
    that reads the events, where the context is not, thus the host does not
    let SDL2 do it.
  - The renderer reads the results of the visibility tests (lens flares)
    from a buffer one time for each frame. The host reads the buffer one
    frame earlier, on the render thread, thus the thread of the game does
    not wait (`host_gl.c`). The GPU can be one frame behind at that time.
    The render thread then sleeps until the GPU is at the fence of the
    frame, and reads the buffer after that. Without this, the driver of
    Mesa waits for the GPU in its loop that does not sleep.
  - The render thread starts on core 1, with a priority between the audio
    threads and the other threads of the game.
- **System calls** (`host_syscall.c`). musl makes Linux system calls. The
  host does each call with libnx and newlib, and gives Linux error numbers
  and structures. Futexes use the address arbiter of Horizon
  (`svcWaitForAddress`, `svcSignalToAddress`).
- **SDL** (`host_sdl.c`, `host_sdl3.c`). The guest is compiled with the
  headers of SDL3. The host does its calls with SDL2: the window and the
  OpenGL ES context, the events of the keyboard, the mouse and the applet,
  the game controllers, and the audio device (SDL2 changes the 48 kHz floats
  of the game to the format of the device). The headers of SDL2 and SDL3
  give the same names different values, thus one file cannot include the
  two. `host_sdl3.c` includes SDL3: it reads the constants of the guest and
  writes its events. `host_sdl.c` includes SDL2.
- **libnx in place of SDL2** (`host_sdl.c`). The Switch drivers of SDL2 do
  not do three functions correctly, thus the host does them with libnx:
  - Which controllers are connected. SDL2 always has eight joysticks, and
    sends no event when a controller connects or disconnects.
  - Rumble. SDL2 has one strength for the two motors, uses one side of the
    controller, and does not use the Joy-Con in handheld mode.
  - The focus of the applet, and the size of the first window.
- **Mixer** (`port/linux/src/dsound_sdl.c`). The mixer of the game changes
  the sample rate of each sound with a filter. On the other platforms the
  filter has 48 taps. On the Switch it has 16 taps, because each tap of each
  sound uses time of a 1 GHz core. A sound of 22 kHz is 0.8 dB quieter at
  9 kHz, and the unwanted copies of its frequencies are 53 dB to 60 dB
  quieter than the sound below 9 kHz. The Switch mixer also mixes chunks of
  256 frames (1024 on the other platforms), and does not mix a sound that
  is 80 dB below full volume. `tools/harness/tests/test_mixer.py` has
  limits for the filter of 48 taps: the filter of the Switch does not
  satisfy its `reference`, `sines` and `aliases` cases.
- **Audio threads** (`host_thread.c`). SDL2 makes its audio thread on the
  core of the main thread, with the lowest priority. The host moves that
  thread, and the thread that runs the mixer of the game, to the other
  cores, and gives them a priority above the other threads of the game.
- **OpenGL ES** (`host_gl.c`). EGL gives the functions of Mesa by name. The
  driver of Mesa waits for the GPU in a loop that does not sleep. When the
  host waits for the GPU, it asks if the GPU is done and sleeps between
  the questions.
- **Files and sockets** (`host_files.c`, `host_net.c`). These files have the
  functions of `port/linux/src/posix.h` for newlib and the BSD sockets of
  the Switch. A socket address of the Switch starts with its length, thus
  the host converts each address.
- **Socket memory** (`host_main.c`, `host_net.c`). The system keeps the
  buffers of all the sockets of a program in memory that the program gives
  it at the start. The default of libnx is 2 MB. Internet play asks for
  that much for one socket, and then the game cannot make a socket for a
  connection. The host gives the system 64 MB, and gives a request for a
  buffer a maximum of 256 KB. The network stack of the Switch is from BSD,
  thus the host also makes two of its functions agree with Linux: a
  connection that a socket accepts blocks, and a connected datagram socket
  can send to the address that it is connected to.

The program ends its process when the game stops (`svcExitProcess`). The
heap pages that it mapped stay locked while the process lives, and hbloader
gives the same heap to the next program.

### Address space

The kernel puts the regions of a process (heap, alias, stack) at random
addresses in a 39-bit address space. Occasionally a region covers
`0x80000000`. Then the program cannot map the Xbox memory, and it shows a
message. Start it again.

## Find problems

- `opence.log` has the log of the program and the errors of the game.
  `debug.txt` is the log of the game.
- To see the log on a computer while the game runs, send the program with
  nxlink: start the homebrew menu, push Y, and enter
  `nxlink -s build/switch/OpenCE.nro` on the computer. nxlink puts the
  program in `switch/`. The program then uses the data in `switch/OpenCE`.
- If the program stops with an exception, `crash.txt` shows the registers
  and the frame chain. To find the functions, enter
  `llvm-symbolizer --obj=build/switch/halo_guest.elf <address>` for the
  addresses of `halo_guest.elf`, and
  `llvm-symbolizer --obj=build/switch/OpenCE.elf <address>` for the
  addresses of `OpenCE.elf`.
- Set `sample_seconds = <seconds>` in `[debug]` of `config.toml`. The log
  then shows the program counter and the frame chain of each guest thread
  at this interval. This finds hangs.
- Set `gl_debug = true` in `[debug]` of `config.toml`. The log then shows
  the OpenGL ES errors.
- Set `memory_watch = false` in `[debug]` if textures are incorrect or the
  game stops when it loads a level. The host then does not protect memory,
  and the game is slower.

## Find a performance problem

1. Set `profile = true` in `[debug]` of `config.toml`.
2. Play until the problem occurs, for 30 seconds or more.
3. Close the game.
4. Copy `opence.log` and `profile.txt` from the folder of the game data to
   the computer.
5. Enter `python tools/switch_profile.py profile.txt`. Use the
   `build/switch` folder of the same build: the addresses change with each
   build. Keep a copy of `halo_guest.elf` and `OpenCE.elf` for a build
   that you give to a different person.

Each two seconds, `opence.log` gets a line that starts with `profile:`. The
line shows:

- The frames in a second, the longest frame, and the number of frames that
  were longer than 25 ms.
- The part of one core that each thread used.
- The location of the main thread: the code of the game, the code of the
  program (Mesa, for the most part), the services of the system, the
  kernel, or a wait.
- The time to show a frame (the wait for the display is in it), the time
  that the host waited for the GPU, and the time in the maps of the buffer
  of the visibility tests (the driver waits for the GPU there if the host
  did not).
- The writes to memory that the renderer watches, the reads of files, the
  waits on locks and the time in the mixer of the game.
- With the render thread: the calls and the memory that the thread of the
  game put in the queue, and the time that it waited (for calls that return
  a value, for space in the queue, and for the frame before).

`profile.txt` has the samples of the seconds in which the main thread used
70% or more of its core, or a frame was late. `tools/switch_profile.py`
changes the addresses to the names of functions and adds the samples
together. For the render thread, it also shows the OpenGL function that
each sample was in.

The profile makes the game a small amount slower. Set `profile = false`
when you do not use it.

If the game is slow, these settings decrease the work:

- `interpolation = false` in `[display]`: the game draws 30 frames each
  second, as on the Xbox, and not 60. This is half of the work to draw.
- `reverb = false` in `[audio]`: the mixer does less work.
- `anti_aliasing = "off"` in `[display]`.
- `docked_height` and `handheld_height` in `[display]`.

## The tests

The game ran on a console with the SDL2 host, at 1280x720 in handheld mode
and at 1920x1080 in the dock (OpenGL ES 3.2, Mesa 20.1 on the NV120
driver), and joined an internet game. The list of shader programs
operates: the long frames (0.1 s to 0.7 s) stopped.

Without the render thread, the game was slow in combat: 17 to 25 frames
each second. The main thread used all of its core, 55% to 60% in the code
of the game and 30% in Mesa and the driver.

With the render thread, a session of five minutes of play had these
results, from the `profile:` lines of `opence.log` (one line for each two
seconds):

| | Handheld, 1280x720 | Dock, 1920x1080 |
| --- | --- | --- |
| Frames each second, in the middle line | 53 | 58 |
| Frames each second, in the slowest tenth of the lines | 37 or less | 43 or less |
| Main thread, average part of its core | 66% | 55% |
| Render thread, average part of its core | 68% | 54% |

- In 17 of the 159 lines, the main thread used 85% or more of its core
  (77% to 92% of its time in the code of the game). In 16 lines, the render
  thread used 88% or more of its core. These lines had 33 to 58 frames
  each second. Other slow lines had a long frame while the game loaded data.
- The main thread used 6% of its time to put the calls in the queue. It
  put 8000 to 8500 calls and 0.8 MB in the queue for each frame.
- The console went into the dock while the game ran. The host changed the
  size of the window on the render thread, and the game continued.
- The mixer of the Switch build (a filter of 16 taps, chunks of 256
  frames) used 13% to 20% of a core on average, and 44% with 50 sounds.
  Before, it used 60% to 85%.
- In that session, the driver of Mesa waited for the GPU in its loop that
  does not sleep, for 5% of the time of the render thread. In Mesa, a map
  of a buffer that the GPU has not written yet has this wait. The read of
  the visibility tests one frame earlier is such a map, and no other such
  map was found.
  After that session, the host was changed to sleep until the GPU is at
  the fence of that frame. On a computer, a test with a GPU that is behind
  shows no wait in the map. The change is not tested on a console. It
  decreases the work of the render thread while it waits for the GPU. It
  does not make the GPU faster.

These parts are not tested on a console:

1. A change from the dock to handheld mode while the game runs.
2. Hosting a game, and system link on a local network.
3. A controller for which SDL2 has no mapping. `opence.log` says so, and the
   host reads the buttons of the controller by their numbers.

### After the tests

The tests above were made before the port was put on the sources of this
repository, which have 248 more commits of the game and its platform layer.
The build from these sources is not tested on a console. These parts are
different from the build of the tests:

- The game has the new functions of those commits (voice chat, votes to
  kick a player, the field of view settings, and others).
- The mixer has a new filter function, which calculates four taps at one
  time. The filter of the Switch has the same 16 taps as before. On a
  computer, the mixer of the other platforms gives the same samples as
  before the Switch changes, and the Switch mixer does the `impulse`,
  `silent-voice` and `mono` cases of `tools/harness/tests/test_mixer.py`
  correctly.
- `debug.memory_watch = false` compares the contents of the pages. Before,
  the setting was `debug.write_watch`, and with `false` the game drew less
  than one frame each second.
- The host has the functions for the new imports of the guest (the
  microphone, the touch controls, a controller that the game closes).

## Limits

- Bink video is not available. The game skips the movies.
- The program does not extract a disc image, and does not update itself.
- The album's homebrew menu (applet mode) has too little memory.
- A file larger than 2 GB cannot be read past 2 GB with the C library of
  the guest (as on Android).
- Keys that SDL2 and SDL3 number differently (media keys, for example) do
  not operate.
- Messages of the game for the player (for example a host with a different
  network version) go to `opence.log` only.
- The program does not record sound. In voice chat, the player hears the
  other players and cannot talk.
- The touch screen does nothing: there are no touch controls, and the menus
  do not take taps.
