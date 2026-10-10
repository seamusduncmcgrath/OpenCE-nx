"""Ninja rules for the Nintendo Switch homebrew build (``ninja switch``).

The Switch port (port/switch/README.md) runs the game as the Android port
does (port/android/README.md): as ILP32 AArch64 code - 32-bit pointers, as
the game's data formats require - inside an ordinary 64-bit program. This
graph builds

- the guest image, build/switch/halo_guest.elf: the same sources as the
  Android guest (the game, the platform layer shared with the Linux port,
  the guest runtime in port/android/guest with a subset of musl), compiled
  by clang for arm64_32-apple-watchos, converted to ELF assembly
  (tools/android_asm_convert.py), assembled for AArch64 and linked at a fixed
  address below 4 GB;
- the host, with devkitA64 and libnx (port/switch/host): the loader and the
  services the guest calls, over Horizon, SDL2 and Mesa (and, generated
  from the guest's OpenGL imports, what queues each of them for the host's
  render thread: tools/switch_gl_queue.py);

and packs both into build/switch/OpenCE.nro, the image in its RomFS.

Every command is one program (tools/switch_tool.py does the steps that take
several): ninja starts commands without a shell on Windows, where devkitPro
is most often installed.
"""

import io
import json
import os
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional

from .android_build import (EXPAT_DIR, EXPAT_SOURCES, GUEST_ABI_FLAGS, GUEST_CODE_FLAGS, KCP_DIR, MONOCYPHER_DIR,
                            MUSL_URL, MUSL_VERSION, SDL_TAG, TOML_DIR, VARIADIC_PROTOTYPE_FILES, ZLIB_DEFINES,
                            ZLIB_DIR, ZLIB_SOURCES, musl_sources)
from .embed_assets import hud_assets_build, hud_configure_inputs
from .linux_build import (LINUX_PROFILE, MUSL_MATH_DIR, XDK_INCLUDE, configuration_defines,
                          game_defines_and_includes, game_sources, musl_math_sources, opus_cflags, opus_sources,
                          pgo_mode, pgo_profile, profile_use_flags, updater_defines, xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/switch")
# the guest's runtime, C library port and ABI, which are the Android port's
GUEST_DIR = Path("port/android/guest")
GUEST_INCLUDE = Path("port/android/include")
GUEST_IMPORTS = Path("port/android/host_imports.list")
# the Android host's write tracking by page hashes, which this host has too
# (debug.memory_watch = false: port/switch/host/host_memory.c)
WATCH_HASH_SOURCE = Path("port/android/host/host_watch_hash.c")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/switch")
THIRD_PARTY = BUILD / "third_party"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
# SDL3's headers only: the guest is compiled against them, and the host
# serves its calls with devkitPro's SDL2 (port/switch/host/host_sdl.c)
SDL_DIR = THIRD_PARTY / f"SDL3-{SDL_TAG}"
SDL_ARCHIVE_URL = f"https://github.com/libsdl-org/SDL/archive/refs/tags/{SDL_TAG}.tar.gz"

APP_TITLE = "OpenCE"
APP_AUTHOR = "OpenCommunityEdition"
OUTPUT = BUILD / f"{APP_TITLE}.nro"

# the Android guest's, on the Switch's processor, and marked as this port's
# for the few places it differs (HALO_ANDROID stays defined: the guest is
# the same ILP32 OpenGL ES program)
SWITCH_GUEST_ABI_FLAGS = [
    "-mcpu=cortex-a57" if flag == "-mcpu=cortex-a53" else flag for flag in GUEST_ABI_FLAGS
] + ["-DHALO_SWITCH=1"]

HOST_ARCH_FLAGS = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE"]
# (as SDL2's sdl2.pc lists them; Mesa's libraries are static, and need the
# C++ runtime the link gives them)
HOST_LIBRARIES = ["SDL2", "EGL", "glapi", "drm_nouveau", "nx", "m", "pthread"]


def _posix(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _find_devkitpro(explicit: Optional[str]) -> Optional[Path]:
    candidates = [explicit, os.environ.get("DEVKITPRO")]
    # (devkitPro's own shell names it /opt/devkitpro, which is not a path
    # for programs outside that shell)
    if os.name == "nt":
        candidates += ["C:/devkitPro"]
    candidates += ["/opt/devkitpro"]
    for candidate in candidates:
        if candidate and (Path(candidate) / "libnx" / "switch.specs").is_file():
            return Path(candidate)
    return None


def _find_program(name: str, directories: List[Path]) -> Optional[Path]:
    found = shutil.which(name)
    if found:
        return Path(found)
    suffix = ".exe" if os.name == "nt" else ""
    for directory in directories:
        if (directory / (name + suffix)).is_file():
            return directory / (name + suffix)
    return None


def _llvm_directories() -> List[Path]:
    directories = []
    for variable in ("ProgramFiles", "ProgramW6432"):
        if os.environ.get(variable):
            directories.append(Path(os.environ[variable]) / "LLVM" / "bin")
    return directories


def _download(url: str) -> bytes:
    print(f"Downloading {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "OpenCE-configure"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def _extract(archive: bytes, destination: Path, keep: Optional[str] = None) -> None:
    """unpacks a .tar.gz without its top folder: all of it, or what is under
    keep (regular files only: neither archive needs its links)"""
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar:
            parts = Path(member.name).parts[1:]
            if not member.isfile() or not parts or ".." in parts:
                continue
            if keep and parts[0] != keep:
                continue
            target = destination.joinpath(*parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            if source:
                target.write_bytes(source.read())


def fetch_third_party() -> None:
    """Download musl and SDL3's headers (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    for directory, url, keep in ((MUSL_DIR, MUSL_URL, None), (SDL_DIR, SDL_ARCHIVE_URL, "include")):
        if directory.is_dir():
            continue
        partial = directory.with_name(directory.name + ".partial")
        if partial.exists():
            shutil.rmtree(partial)
        _extract(_download(url), partial, keep)
        partial.rename(directory)


def _copy_gl_headers(portlibs_include: Path, destination: Path) -> None:
    """Mesa's OpenGL ES headers (C declarations only) for the guest, apart
    from the host's other libraries' headers"""
    for name in ("GLES2", "GLES3", "KHR"):
        target = destination / name
        target.mkdir(parents=True, exist_ok=True)
        for header in sorted((portlibs_include / name).glob("*.h")):
            data = header.read_bytes()
            if not (target / header.name).is_file() or (target / header.name).read_bytes() != data:
                (target / header.name).write_bytes(data)


def switch_configure_inputs() -> List[Path]:
    return [Path(__file__), Path("tools/switch_tool.py"), GUEST_DIR / "runtime", PORT_DIR / "host",
            LINUX_DIR / "src", *hud_configure_inputs()]


def generate_switch_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return
    devkitpro = _find_devkitpro(getattr(sln, "devkitpro", None))
    if not devkitpro:
        n.comment("Switch build: devkitPro with libnx was not found (set DEVKITPRO or pass --devkitpro)")
        return
    guest_cc = _find_program(getattr(sln, "switch_guest_cc", None) or "clang", _llvm_directories())
    if not guest_cc:
        n.comment("Switch build: no clang found for the guest (pass --switch-guest-cc)")
        return
    llvm_bin = guest_cc.parent
    tools: Dict[str, Optional[Path]] = {name: _find_program(name, [llvm_bin]) for name in
                                        ("ld.lld", "llvm-ar", "llvm-objcopy")}
    missing = [name for name, path in tools.items() if not path]
    if missing:
        n.comment(f"Switch build: {', '.join(missing)} not found beside {guest_cc}")
        return
    portlibs = devkitpro / "portlibs" / "switch"
    missing_packages = [package for package, library in (("switch-mesa", "libEGL.a"), ("switch-sdl2", "libSDL2.a"))
                        if not (portlibs / "lib" / library).is_file()]
    if missing_packages:
        n.comment(f"Switch build: not installed: {' '.join(missing_packages)} "
                  f"(dkp-pacman -S {' '.join(missing_packages)})")
        return
    try:
        fetch_third_party()
    except (OSError, tarfile.TarError) as error:
        print(f"Switch build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    release = getattr(sln, "port_release", False)

    devkita64_bin = devkitpro / "devkitA64" / "bin"
    suffix = ".exe" if os.name == "nt" else ""
    host_cc = devkita64_bin / f"aarch64-none-elf-gcc{suffix}"
    host_cxx = devkita64_bin / f"aarch64-none-elf-g++{suffix}"
    switch_tools = devkitpro / "tools" / "bin"

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    gl_include = guest_dir / "gl_include"
    arch = GUEST_DIR / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    romfs = BUILD / "romfs"
    tool = "$python tools/switch_tool.py"
    tool_script = Path("tools/switch_tool.py")

    _copy_gl_headers(portlibs / "include", gl_include)

    n.comment("Nintendo Switch build (ninja switch); see port/switch/README.md")
    n.variable("switch_guest_cc", _posix(guest_cc))
    n.variable("switch_lld", _posix(tools["ld.lld"]))
    n.variable("switch_ar", _posix(tools["llvm-ar"]))
    n.variable("switch_objcopy", _posix(tools["llvm-objcopy"]))
    n.variable("switch_host_cc", _posix(host_cc))
    n.variable("switch_host_cxx", _posix(host_cxx))
    rsp_quoting = "--rsp-quoting=windows " if os.name == "nt" else ""

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="switch_musl_headers",
        command=f"{tool} musl-headers {MUSL_DIR} {arch} {MUSL_VERSION} {libc_include} {libc_internal}",
        description="SWITCH MUSL HEADERS",
        restat=True,
    )
    n.build(outputs=[alltypes, syscall_h, version_h], rule="switch_musl_headers",
            implicit=[tool_script, arch / "bits" / "alltypes.h.in", arch / "bits" / "syscall.h.in",
                      MUSL_DIR / "include" / "alltypes.h.in"])

    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    n.rule(
        name="switch_gl_stubs",
        command=(f"$python tools/android_gl_stubs.py {LINUX_DIR}/src/gl.h {gl_include}/GLES3/gl32.h "
                 f"{gl_include}/GLES2/gl2ext.h {guest_gl_c} {gl_imports}"),
        description="SWITCH GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="switch_gl_stubs",
            implicit=[Path("tools/android_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="switch_posix_stubs",
        command=f"$python tools/android_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="SWITCH POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="switch_posix_stubs",
            implicit=[Path("tools/android_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_gen = BUILD / "host" / "gen"
    host_table_c = host_gen / "host_import_table.c"
    n.rule(
        name="switch_imports",
        command=f"$python tools/android_imports.py --host-table {host_table_c} {imports_s} $in",
        description="SWITCH IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="switch_imports",
            inputs=[GUEST_IMPORTS, posix_imports, gl_imports],
            implicit=[Path("tools/android_imports.py")])

    # the host's side of the same OpenGL imports, for its render thread
    # (port/switch/host/host_render.c includes it)
    gl_queue_inc = host_gen / "host_gl_queue.inc"
    n.rule(
        name="switch_gl_queue",
        command=(f"$python tools/switch_gl_queue.py {gl_include}/GLES3/gl32.h {gl_include}/GLES2/gl2ext.h "
                 f"{gl_imports} {gl_queue_inc}"),
        description="SWITCH GL QUEUE",
    )
    n.build(outputs=gl_queue_inc, rule="switch_gl_queue", inputs=gl_imports,
            implicit=[Path("tools/switch_gl_queue.py"), Path("tools/android_gl_stubs.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header,
                         platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly -> object

    n.rule(
        name="switch_guest_cc",
        command=f"{tool} guest-cc $switch_guest_cc $out $in -- $cflags",
        description="SWITCH CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="switch_guest_as",
        command=f"{tool} guest-as $switch_guest_cc $out $in",
        description="SWITCH AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    guest_abi = " ".join(SWITCH_GUEST_ABI_FLAGS + configuration_defines(sln))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [tool_script, Path("tools/android_asm_convert.py"), *generated_headers]
    # profile-guided optimisation with the Linux build's profile, as the
    # Android guest: the game and platform code are the same, and functions
    # that differ simply go without
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE],
                          str(guest_cc))
    profile_flags = " ".join(profile_use_flags(profile))
    if profile:
        tool_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        if BUILD in source.parents:
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        else:
            obj = obj_dir / prefix / source.with_suffix(".o")
        n.build(outputs=obj, rule="switch_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{GUEST_DIR}/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in musl_sources(MUSL_DIR)]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="switch_ar",
        command=f"$switch_ar {rsp_quoting}rcs $out @$out.rsp",
        description="SWITCH AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="switch_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() == "source/main/main.c":
            cflags += " " + updater_defines(release)
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {GUEST_INCLUDE}/halo_android_variadic_prototypes.h"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{GUEST_DIR}/runtime",
        f"-I{GUEST_INCLUDE}", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    guest_host_only = {"memory_watch.c"}  # replaced by guest_memory_watch.c
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        cflags = platform_cflags
        if source.name == "updater.c":
            cflags += " " + updater_defines(release)
        objects.append(guest_object(source, cflags))
    for source in hud_assets_build(n, "switch", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    # voice chat's codec (port/third_party/opus), with the guest's ABI and C
    # library
    for source in opus_sources():
        objects.append(guest_object(source, " ".join([opus_cflags(guest_abi), *libc_includes])))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    for name in ZLIB_SOURCES:
        # (not the CPU's CRC32 instructions, which the guest's assembly step
        # is not told it may use)
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES,
                                                               "-U__ARM_FEATURE_CRC32"])))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", profile_flags, *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{GUEST_DIR}/runtime", f"-I{GUEST_INCLUDE}",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{GUEST_DIR}/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{GUEST_DIR}/runtime", f"-I{GUEST_INCLUDE}", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", f"-I{gl_include}", *libc_includes,
    ])
    for source in sorted((GUEST_DIR / "runtime").glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="switch_guest_as", inputs=imports_s, implicit=[tool_script])
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = GUEST_DIR / "guest.ld"
    n.rule(
        name="switch_guest_link",
        command=(f"$switch_lld {rsp_quoting}-m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="SWITCH LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="switch_guest_link", inputs=objects, implicit=[libguestc, linker_script])
    # the image the program carries, without the symbols (the one above
    # keeps them, for llvm-symbolizer)
    packed_image = romfs / "halo_guest.elf"
    n.rule(name="switch_strip", command="$switch_objcopy --strip-all $in $out", description="SWITCH STRIP $out")
    n.build(outputs=packed_image, rule="switch_strip", inputs=image)

    # ---------- the host

    host_objects: List[Path] = []
    host_obj_dir = BUILD / "host" / "obj"
    guest_syscalls_h = host_gen / "guest_syscall_numbers.h"
    n.rule(
        name="switch_syscall_numbers",
        command=f"{tool} syscall-numbers $in $out",
        description="SWITCH SYSCALL NUMBERS",
        restat=True,
    )
    n.build(outputs=guest_syscalls_h, rule="switch_syscall_numbers", inputs=arch / "bits" / "syscall.h.in",
            implicit=[tool_script])
    n.rule(
        name="switch_host_cc",
        command="$switch_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="SWITCH HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_cflags = " ".join([
        "-O2", "-g", *HOST_ARCH_FLAGS, "-ffunction-sections", "-Wall", "-Wno-unused-function", "-std=gnu11",
        "-D__SWITCH__", "-D_GNU_SOURCE", "-DHALO_SWITCH=1",
        f'-DHALO_SWITCH_VERSION=\\"{_version()}\\"',
        f"-I{GUEST_INCLUDE}", f"-I{PORT_DIR}/host", f"-I{host_gen}", f"-I{LINUX_DIR}/src", f"-I{TOML_DIR}",
        # (SDL3's headers, for the guest's constants and events:
        # host_sdl3.c. SDL2's, the host's own SDL, are <SDL2/...> among the
        # portlibs')
        f"-I{SDL_DIR}/include",
        f"-I{_posix(devkitpro / 'libnx' / 'include')}", f"-I{_posix(portlibs / 'include')}",
    ])
    host_sources = sorted((PORT_DIR / "host").glob("*.c")) + sorted((PORT_DIR / "host").glob("*.S")) + [
        # the app reads debug.sample_seconds from config.toml (host_main.c)
        TOML_DIR / "tomlc17.c", host_table_c, WATCH_HASH_SOURCE,
    ]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="switch_host_cc", inputs=source, order_only=[guest_syscalls_h, gl_queue_inc],
                variables={"cflags": host_cflags + (" -w" if source.name == "tomlc17.c" else "")})
        host_objects.append(obj)
    host_elf = BUILD / f"{APP_TITLE}.elf"
    n.rule(
        name="switch_host_link",
        # (the specs file finds libnx's linker script through DEVKITPRO)
        command=(f"{tool} run DEVKITPRO={_posix(devkitpro)} -- $switch_host_cxx "
                 f"-specs={_posix(devkitpro / 'libnx' / 'switch.specs')} -g {' '.join(HOST_ARCH_FLAGS)} "
                 f"-Wl,-Map,$out.map -o $out $in -L{_posix(portlibs / 'lib')} -L{_posix(devkitpro / 'libnx' / 'lib')} "
                 + " ".join(f"-l{library}" for library in HOST_LIBRARIES)),
        description="SWITCH HOST LINK $out",
    )
    n.build(outputs=host_elf, rule="switch_host_link", inputs=host_objects, implicit=[tool_script])

    # ---------- the program

    n.rule(name="switch_copy", command=f"{tool} copy $in $out", description="SWITCH STAGE $out")
    # internet play's MQTT brokers, in the RomFS: the host writes them
    # beside config.toml (port/switch/host/host_main.c)
    packed_brokers = romfs / "brokers.txt"
    n.build(outputs=packed_brokers, rule="switch_copy", inputs=Path("port/assets/network/brokers.txt"),
            implicit=[tool_script])
    nacp = BUILD / f"{APP_TITLE}.nacp"
    n.rule(
        name="switch_nacp",
        command=(f"{_posix(switch_tools / ('nacptool' + suffix))} --create {APP_TITLE} {APP_AUTHOR} "
                 f"{_version()} $out"),
        description="SWITCH NACP $out",
    )
    n.build(outputs=nacp, rule="switch_nacp")
    icon = PORT_DIR / "icon.jpg"
    if not icon.is_file():
        icon = devkitpro / "libnx" / "default_icon.jpg"
    n.rule(
        name="switch_nro",
        command=(f"{_posix(switch_tools / ('elf2nro' + suffix))} $in $out --icon={_posix(icon)} "
                 f"--nacp={_posix(nacp)} --romfsdir={_posix(romfs)}"),
        description="SWITCH NRO $out",
    )
    n.build(outputs=OUTPUT, rule="switch_nro", inputs=host_elf, implicit=[nacp, icon, packed_image, packed_brokers])
    n.build(outputs="switch", rule="phony", inputs=OUTPUT)
    n.build(outputs="switch_guest", rule="phony", inputs=image)
    n.newline()


def _version() -> str:
    """the program's version, as the HOME menu and hbmenu show it: the
    build's number (tools/ci_build.py), or 0 for a local build"""
    number = os.environ.get("HALO_BUILD_NUMBER", "0")
    return f"0.{number if number.isdigit() else '0'}.0"
