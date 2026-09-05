#!/usr/bin/env python3
"""Name the callers the heap tracer reports.

The tracer logs "libBully.so+0x<offset>" for whoever asked for each block. The
.so keeps a full dynamic symbol table, so those offsets can be turned back into
C++ function names -- which is the difference between "something leaks" and
"CStreaming::RequestModel leaks".
"""
import subprocess, sys, re, bisect, os

SO = os.environ.get("BULLY_SO", "libBully.so")

def load():
    out = subprocess.run(["readelf", "-sW", "--dyn-syms", SO], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) < 8 or p[3] != "FUNC":
            continue
        addr = int(p[1], 16)
        if addr == 0:
            continue
        size = int(p[2], 16) if p[2].startswith("0x") else int(p[2])
        syms.append((addr & ~1, size, p[7].split("@")[0]))
    syms.sort()
    return syms

def demangle(names):
    if not names:
        return {}
    r = subprocess.run(["/home/user/vitasdk/bin/arm-vita-eabi-c++filt"],
                       input="\n".join(names), capture_output=True, text=True)
    return dict(zip(names, r.stdout.splitlines()))

def main():
    syms = load()
    starts = [s[0] for s in syms]
    text = sys.stdin.read()
    offsets = sorted({int(m, 16) for m in re.findall(r"libBully\.so\+0x([0-9a-fA-F]+)", text)})
    if not offsets:
        print("no libBully.so+0x... offsets found on stdin", file=sys.stderr)
        return 1
    rows = []
    for off in offsets:
        i = bisect.bisect_right(starts, off) - 1
        if i < 0:
            rows.append((off, None, None))
            continue
        addr, size, name = syms[i]
        # A size of 0 means the symbol table did not say how long it is, so an
        # offset past it may belong to an unnamed function rather than this one.
        inside = size == 0 or off < addr + size
        rows.append((off, name if inside else None, off - addr if inside else None))
    names = demangle([r[1] for r in rows if r[1]])
    for off, name, delta in rows:
        if not name:
            print(f"0x{off:08x}  <no symbol covers this address>")
        else:
            print(f"0x{off:08x}  {names.get(name, name)}  +0x{delta:x}")
    return 0

sys.exit(main())
