#!/usr/bin/env python3
# vtcalls.py <lib.so> <disasm.txt> <interface string>: find where the client stores that interface
# (CreateInterface result) and histogram the vtable offsets called through that global.
# disasm.txt: objdump -d --no-show-raw-insn -M intel <lib.so>. A heuristic: the first virtual call within
# 25 instructions after each load of the global is counted.
import subprocess, sys, re, collections
lib, dis, name = sys.argv[1:4]
data = open(lib, "rb").read()
segs = [(int(p[1],16), int(p[2],16), int(p[4],16)) for p in (l.split() for l in subprocess.check_output(["readelf","-lW",lib],text=True).splitlines()) if p and p[0]=="LOAD"]
va = lambda off: next(off - o + v for o, v, fs in segs if o <= off < o + fs)
sa = va(data.find(b"\0" + name.encode() + b"\0") + 1)
lines = open(dis).read().splitlines()
rx = re.compile(r"^\s*([0-9a-f]+):\s*(.*?)(?:\s*#\s*([0-9a-f]+).*)?$")
P = [rx.match(l) for l in lines]
globs = collections.Counter()
for i, m in enumerate(P):
    if m and m.group(3) and int(m.group(3), 16) == sa:
        for j in range(i, i + 12):
            n = P[j]
            if n and n.group(3) and n.group(2).startswith("mov    QWORD PTR [rip") and n.group(2).endswith("rax"):
                globs[int(n.group(3), 16)] += 1; break
print("globals:", [hex(g) for g in globs])
rel = {}
for l in subprocess.check_output(["readelf", "-rW", lib], text=True).splitlines():
    q = l.split()
    if len(q) >= 4 and q[2] == "R_X86_64_RELATIVE":
        rel.setdefault(int(q[3], 16), []).append(int(q[0], 16))
for g0 in list(globs):
    for got in rel.get(g0, []): globs[got] += 1
for g in globs:
    hist = collections.Counter()
    for i, m in enumerate(P):
        if m and m.group(3) and int(m.group(3), 16) == g and (m.group(2).startswith("mov    r") or m.group(2).startswith("lea    r")):
            reg = m.group(2).split()[1].split(",")[0]
            for j in range(i + 1, min(i + 25, len(P))):
                n = P[j]
                if not n: continue
                c = re.match(r"call\s+QWORD PTR \[r..(?:\+0x([0-9a-f]+))?\]", n.group(2))
                if c:
                    hist[int(c.group(1) or "0", 16) // 8] += 1; break
    print(hex(g), sorted(hist.items()))
