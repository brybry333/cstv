#!/usr/bin/env python3
"""Count the vtable slots of C++ classes in a stripped x86-64 .so, using only its relocations.

usage: vtdump.py <lib.so> <mangled typeinfo name, e.g. 8CFunctor> [...]

How: the typeinfo name string -> the typeinfo object (a RELATIVE relocation whose addend is the
string's address, stored at typeinfo+8) -> vtables (relocation at vtable+8 pointing at the
typeinfo) -> consecutive relocated 8-byte slots after vtable+16 (RELATIVE = defined function,
symbol relocation = e.g. __cxa_pure_virtual). Re-run after game updates to confirm slot counts.
"""
import subprocess
import sys


def load(path):
    data = open(path, "rb").read()
    rel = {}   # address -> (type, addend or symbol)
    out = subprocess.check_output(["readelf", "-rW", path], text=True)
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[2].startswith("R_X86_64_"):
            try:
                off = int(p[0], 16)
            except ValueError:
                continue
            if p[2] == "R_X86_64_RELATIVE":
                rel[off] = ("rel", int(p[3], 16))
            else:
                rel[off] = ("sym", " ".join(p[4:]))
    segs = []
    for line in subprocess.check_output(["readelf", "-lW", path], text=True).splitlines():
        p = line.split()
        if p and p[0] == "LOAD":
            segs.append((int(p[1], 16), int(p[2], 16), int(p[4], 16)))
    return data, rel, segs


def va_of(segs, off):
    for o, v, fs in segs:
        if o <= off < o + fs:
            return off - o + v
    return None


def dump(path, names):
    data, rel, segs = load(path)
    by_addend = {}
    for a, (t, x) in rel.items():
        if t == "rel":
            by_addend.setdefault(x, []).append(a)
    for name in names:
        needle = name.encode() + b"\0"
        i = data.find(b"\0" + needle)
        if i < 0:
            print(f"{name}: typeinfo name not found")
            continue
        s_va = va_of(segs, i + 1)
        for ti_name_slot in by_addend.get(s_va, []):
            ti = ti_name_slot - 8
            for vt_ti_slot in by_addend.get(ti, []):
                vt = vt_ti_slot - 8
                slots = []
                a = vt + 16
                while a in rel:
                    t, x = rel[a]
                    if t == "rel" and x in by_addend.get(x, []):   # never true; keeps structure simple
                        break
                    slots.append(hex(x) if t == "rel" else x)
                    a += 8
                print(f"{name}: vtable {hex(vt)} -> {len(slots)} slots: {slots[:8]}{' ...' if len(slots) > 8 else ''}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dump(sys.argv[1], sys.argv[2:])
