#!/usr/bin/env python3
"""Keep docs/port/symbols.tsv in step with what the port link leaves undefined.

    tools/port/symbols.py <link log> [--tsv docs/port/symbols.tsv]

Reads the linker's "undefined reference to `X'" lines (from a link with
--unresolved-symbols=report-all), and merges them into the checklist:

- a symbol the link still needs and the TSV lacks is added as stub-todo;
- a symbol the TSV lists as stub-todo that the link no longer needs is marked
  real (somebody implemented it; fill in the file column);
- rows already marked real or stub-ok are left alone.

Prints a summary by area. Areas are a first guess from the name and can be
corrected in the TSV by hand; the script never overwrites a row's area or note.
"""
import argparse
import collections
import re
import subprocess

AREAS = [
    ("file", r"File|Directory|Path|Drive|Disk|Volume|_wfopen|_access|_mkdir|_fullpath"),
    ("heap", r"Heap|Virtual|Global(Alloc|Free|Lock|Unlock|Size|Memory)|Local(Alloc|Free)"),
    ("sync", r"CriticalSection|Event|Semaphore|Mutex|WaitFor|Interlocked|SignalObject"),
    ("thread", r"Thread|Tls|Fiber|_beginthread|_endthread"),
    ("time", r"Tick|PerformanceCounter|PerformanceFrequency|timeGetTime|timeBeginPeriod|timeEndPeriod|LocalTime|SystemTime|FileTime|TimeZone"),
    ("registry", r"^Reg[A-Z]"),
    ("window", r"Window|Message|Wnd|Cursor|Icon|Class|Focus|Capture|Clip|Rect|KeyState|KeyboardState|MapVirtualKey|ScreenToClient|ClientToScreen|SystemMetrics|Menu|Dialog|Imm|Clipboard"),
    ("gdi", r"DC$|DC\b|Bitmap|DIB|BitBlt|StretchDIBits|Object|Palette|Font|Text|Brush|Pen|Gdi|Rgn"),
    ("sound", r"DirectSound|wave|mixer|mmio|Wave|Mixer"),
    ("ddraw", r"DirectDraw|Direct3D|gr[A-Z]|Glide"),
    ("video", r"^Bink|^Smack|^ijl"),
    ("net", r"WSA|socket|connect|send|recv|bind|listen|accept|select|gethost|inet_|htons|ntohs|htonl|ntohl|closesocket|ioctlsocket|getsockname|setsockopt|getsockopt|shutdown"),
    ("module", r"Module|Library|ProcAddress|Version|Resource|CommandLine|Environment|StartupInfo|ExitProcess|TerminateProcess|GetCurrentProcess"),
    ("system", r"SystemInfo|MemoryStatus|ComputerName|UserName|Locale|CodePage|ACP|OEMCP|MultiByte|WideChar|LastError|DebugString|IsBad|FormatMessage|Shell|SH[A-Z]|CoInit|CoCreate|CoUninit|Crypt|WinVerify|Toolhelp|Process32|Module32|EnumProcess|GetProcessMemoryInfo"),
    ("crt", r"^_|^str|^mem|^wcs|^printf|^sprintf"),
]


def area_of(name):
    for area, pat in AREAS:
        if re.search(pat, name):
            return area
    return "other"


def demangle(names):
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True).stdout
        return out.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--tsv", default="docs/port/symbols.tsv")
    args = ap.parse_args()

    undefined = sorted(set(re.findall(r"undefined reference to [`'‘]([^'’]+)['’]", open(args.log, errors="replace").read())))
    pretty = dict(zip(undefined, demangle(undefined)))

    header = ["symbol", "area", "status", "file", "note"]
    rows = collections.OrderedDict()
    try:
        with open(args.tsv) as f:
            lines = f.read().splitlines()
        for line in lines[1:]:
            if line.strip():
                cols = (line.split("\t") + [""] * 5)[:5]
                rows[cols[0]] = cols
    except FileNotFoundError:
        pass

    needed = {pretty[s] for s in undefined}
    added = resolved = 0
    for sym in sorted(needed):
        if sym not in rows:
            base = re.sub(r"\(.*$", "", sym).split("::")[-1]
            rows[sym] = [sym, area_of(base), "stub-todo", "", ""]
            added += 1
    for sym, cols in rows.items():
        if cols[2] == "stub-todo" and sym not in needed:
            cols[2] = "real"
            resolved += 1

    with open(args.tsv, "w") as f:
        f.write("\t".join(header) + "\n")
        for sym in sorted(rows, key=lambda s: (rows[s][1], s.lower())):
            f.write("\t".join(rows[sym]) + "\n")

    by_area = collections.Counter(rows[s][1] for s in needed)
    print(f"link needs {len(needed)} symbols; {added} new, {resolved} newly resolved")
    for area, n in by_area.most_common():
        print(f"  {area:10s} {n}")


if __name__ == "__main__":
    main()
