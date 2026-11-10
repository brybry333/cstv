#!/usr/bin/env python3
# slots.py <lib.so> <vtable addr hex> <slot>...: function address in each slot (relocated pointers)
# vtable addr: from vtdump.py
import subprocess, sys
lib, vt = sys.argv[1], int(sys.argv[2], 16)
rel = {}
for l in subprocess.check_output(["readelf", "-rW", lib], text=True).splitlines():
    q = l.split()
    if len(q) >= 4 and q[2] == "R_X86_64_RELATIVE": rel[int(q[0], 16)] = int(q[3], 16)
for s in sys.argv[3:]:
    print(s, hex(rel.get(vt + 16 + 8 * int(s), 0)))
