#!/usr/bin/env python3
"""Names the addresses of a Switch profile and adds them up by function.

The Switch build writes profile.txt to the folder of the game data when
config.toml has `profile = true` in [debug] (port/switch/host/host_debug.c
has what the file holds). This reads it against the two programs of the same
build:

    python tools/switch_profile.py profile.txt

For each thread it prints where its samples were (the game, the program,
system calls, waits), the functions with the most samples, the system calls
and what made them, the waits with the game's functions that waited, and,
for the samples outside the game, the game's functions that they were for.
"""

import argparse
import collections
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

SYSTEM_CALLS = {
    0x02: "SetMemoryPermission", 0x06: "QueryMemory", 0x0B: "SleepThread", 0x18: "WaitSynchronization",
    0x1A: "ArbitrateLock", 0x1B: "ArbitrateUnlock", 0x1C: "WaitProcessWideKeyAtomic",
    0x1D: "SignalProcessWideKey", 0x21: "SendSyncRequest", 0x22: "SendSyncRequestWithUserBuffer",
    0x28: "ReturnFromException", 0x32: "SetThreadActivity", 0x33: "GetThreadContext3", 0x34: "WaitForAddress",
    0x35: "SignalToAddress", 0x73: "SetProcessMemoryPermission", 0x77: "MapProcessCodeMemory",
}
WAITS = {"SleepThread", "WaitSynchronization", "ArbitrateLock", "WaitProcessWideKeyAtomic", "WaitForAddress"}
# the C library's and the port's own functions on the way to a system call: a wait is for what called them
PLUMBING_PREFIXES = ("__", "pthread_", "sem_", "nanosleep", "usleep", "clock_", "syscall", "guest_", "host_")
PLUMBING = {"EnterCriticalSection", "LeaveCriticalSection", "WaitForSingleObject", "WaitForSingleObjectEx", "Sleep",
            "SleepEx", "SetEvent", "futex", "a_spin"}

Sample = collections.namedtuple("Sample", "thread where origin caller count chain outer")


def find_tool(given, name):
    candidates = [given] if given else []
    candidates += [shutil.which(name), f"C:/Program Files/LLVM/bin/{name}.exe", f"/usr/bin/{name}"]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists():
            return str(candidate)
    return None


def symbolize(symbolizer, image, addresses):
    """The function of each address of an image."""
    addresses = sorted(addresses)
    if not addresses:
        return {}
    if not image.exists():
        return {address: f"{image.name}+{address:#x}" for address in addresses}
    result = subprocess.run(
        [symbolizer, f"--obj={image}", "--inlining=false", "--functions=linkage"],
        input="\n".join(f"{address:#x}" for address in addresses) + "\n",
        capture_output=True, text=True, check=True)
    blocks = [block.splitlines() for block in result.stdout.replace("\r\n", "\n").strip().split("\n\n")]
    names = {}
    for address, block in zip(addresses, blocks):
        name = block[0].strip() if block and block[0].strip() else "??"
        names[address] = name if name != "??" else f"{image.name}+{address:#x}"
    return names


def link_end(symbolizer, image):
    """The address the linker gave the end of the program (__end__)."""
    tool = find_tool(str(pathlib.Path(symbolizer).with_name("llvm-nm" + pathlib.Path(symbolizer).suffix)), "llvm-nm")
    if not image.exists() or not tool:
        return None
    for line in subprocess.run([tool, str(image)], capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == "__end__":
            return int(fields[0], 16)
    return None


def parse(path):
    threads = {}
    samples = []
    header = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "thread" and len(fields) >= 3:
            threads[int(fields[1])] = " ".join(fields[2:])
        elif fields[0].isdigit() and len(fields) >= 5 and fields[4].isdigit():
            rest = fields[5:]
            split = rest.index("/") if "/" in rest else len(rest)
            samples.append(Sample(int(fields[0]), fields[1], fields[2], fields[3], int(fields[4]),
                                  tuple(rest[:split]), tuple(rest[split + 1:])))
        else:
            header.append(line)
    return header, threads, samples


def plumbing(name):
    return name.startswith(PLUMBING_PREFIXES) or name in PLUMBING


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("profile", type=pathlib.Path)
    parser.add_argument("--game", type=pathlib.Path, default=ROOT / "build/switch/halo_guest.elf")
    parser.add_argument("--program", type=pathlib.Path, default=ROOT / "build/switch/OpenCE.elf")
    parser.add_argument("--symbolizer")
    parser.add_argument("--top", type=int, default=40, help="lines of each list")
    arguments = parser.parse_args()

    header, threads, samples = parse(arguments.profile)
    symbolizer = find_tool(arguments.symbolizer, "llvm-symbolizer")
    if not symbolizer:
        sys.exit("llvm-symbolizer was not found: give it with --symbolizer")

    # the first profiles gave the program's addresses as they were in memory, not from its start: the
    # header's range then starts at 0, and its end says where the program was
    base = 0
    for line in header:
        fields = line.split()
        if len(fields) == 4 and fields[0] == "game" and fields[2] == "program":
            start, end = (int(value, 16) for value in fields[3].split("-"))
            linked = link_end(symbolizer, arguments.program)
            if start == 0 and linked is not None:
                base = end - linked

    def offset(text):
        return f"h+{int(text[2:], 16) - base:x}" if text.startswith("h+") else text

    if base:
        samples = [Sample(sample.thread, offset(sample.where), offset(sample.origin), offset(sample.caller),
                          sample.count, tuple(offset(text) for text in sample.chain),
                          tuple(offset(text) for text in sample.outer)) for sample in samples]

    wanted = {"g": set(), "h": set()}
    for sample in samples:
        for text in (sample.where, sample.origin, sample.caller, *sample.chain, *sample.outer):
            if text.startswith("g:"):
                wanted["g"].add(int(text[2:], 16))
            elif text.startswith("h+"):
                wanted["h"].add(int(text[2:], 16))
    names = {"g": symbolize(symbolizer, arguments.game, wanted["g"]),
             "h": symbolize(symbolizer, arguments.program, wanted["h"])}

    def name(text):
        if text.startswith("g:"):
            return names["g"][int(text[2:], 16)]
        if text.startswith("h+"):
            return names["h"][int(text[2:], 16)]
        return text

    def waiter(sample):
        """The game's function that a system call is for: the first caller that is not the C library's."""
        callers = [name(text) for text in (sample.caller, *sample.chain) if text != "-"]
        for caller in callers:
            if not plumbing(caller):
                return caller
        return callers[-1] if callers else "(none found)"

    for line in header:
        print(line)
    by_thread = collections.defaultdict(list)
    for sample in samples:
        by_thread[sample.thread].append(sample)
    for thread in sorted(by_thread, key=lambda index: -sum(sample.count for sample in by_thread[index])):
        rows = by_thread[thread]
        total = sum(row.count for row in rows)
        kinds = collections.Counter()
        functions = collections.Counter()
        calls = collections.Counter()
        waits = collections.Counter()
        callers = collections.Counter()
        parts = collections.Counter()
        outside = 0
        for sample in rows:
            where, count = sample.where, sample.count
            call = None
            # (each of the outermost callers once: all the time under it, waits too)
            for part in {name(text) for text in sample.outer}:
                parts[part] += count
            if where.startswith("svc"):
                call = SYSTEM_CALLS.get(int(where[3:], 16), where)
            elif where.startswith("h+") and name(where).startswith("svc"):
                # (a thread stopped at the call itself, which the first profiles took for the program's code)
                call = name(where)[3:]
            if call in WAITS:
                kinds["waiting"] += count
                waits[(call, waiter(sample))] += count
                continue
            if call:
                kinds["system calls"] += count
                calls[(call, name(sample.origin) if sample.origin != "-" else "-", waiter(sample))] += count
            elif where.startswith("g:"):
                kinds["the game"] += count
                functions[name(where) + "  [game]"] += count
                continue
            elif where.startswith("h+"):
                kinds["the program"] += count
                functions[name(where)] += count
            else:
                kinds["elsewhere"] += count
                functions[where] += count
            outside += count
            callers[name(sample.caller) if sample.caller != "-" else "(none found)"] += count

        print(f"\n=== thread {thread} ({threads.get(thread, '?')}): {total} samples")
        print("  " + ", ".join(f"{kind} {count * 100 / total:.1f}%" for kind, count in kinds.most_common()))
        if parts:
            print("  all the time under each of the outermost functions of the stack (the game's first eight, or the"
                  " program's first four on a thread that runs none of the game's code):")
            for part, count in parts.most_common(arguments.top):
                print(f"    {count * 100 / total:5.1f}%  {part}")
        if functions:
            print("  functions:")
            for function, count in functions.most_common(arguments.top):
                print(f"    {count * 100 / total:5.1f}%  {function}")
        if waits:
            print("  waits (and the game's function that waited):")
            for (call, caller), count in waits.most_common(arguments.top // 2):
                print(f"    {count * 100 / total:5.1f}%  {call}  in {caller}")
        if calls:
            print("  system calls (what made each, and the game's function it was for):")
            for (call, origin, caller), count in calls.most_common(arguments.top // 2):
                print(f"    {count * 100 / total:5.1f}%  {call}  from {origin}  for {caller}")
        if outside:
            print(f"  the game's functions that the {outside * 100 / total:.1f}% outside the game (waits apart) were for:")
            for caller, count in callers.most_common(arguments.top):
                print(f"    {count * 100 / total:5.1f}%  {caller}")


if __name__ == "__main__":
    main()
