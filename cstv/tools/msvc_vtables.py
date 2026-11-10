#!/usr/bin/env python3
"""msvc_vtables.py — find a class's vtable in an MSVC x64 DLL through its RTTI and look at given slots.

  msvc_vtables.py <dll> <ClassName> [slot ...]     list the vtable size, disassemble the given slots
  msvc_vtables.py <dll> --classes <substring>      list RTTI class names containing <substring>

RTTI path: the type descriptor holds the decorated name ".?AVClassName@@"; a Complete Object Locator
(signature 1, x64) points at it by RVA; the vtable is preceded by a pointer to that COL. For each slot
the target function is disassembled briefly, with the strings it references (rip-relative lea/mov) and
its direct calls / jumps -- enough to tell a getter from a string-heavy function.
Needs pefile + capstone (a venv is fine).
"""
import struct
import sys

import capstone
import pefile


class Dll:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.text = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in self.pe.sections
                     if s.Characteristics & 0x20000000]   # executable
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def is_code(self, rva):
        return any(a <= rva < b for a, b in self.text)

    def u32(self, rva):
        return struct.unpack_from("<I", self.data, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.data, rva)[0]

    def cstr(self, rva, n=120):
        if not (0 <= rva < len(self.data)):
            return None
        end = self.data.find(b"\0", rva, rva + n)
        if end < 0:
            return None
        b = self.data[rva:end]
        if len(b) < 3 or any(c < 0x20 or c > 0x7e for c in b):
            return None
        return b.decode()

    def classes(self, sub):
        out, i = [], 0
        while True:
            i = self.data.find(b".?AV", i)
            if i < 0:
                return out
            name = self.cstr(i, 200)
            if name and sub in name:
                out.append(name)
            i += 4

    def vtables(self, cls):
        """[(col_offset, vtable_rva)] for the class's COLs."""
        td_name = (".?AV%s@@" % cls).encode() + b"\0"
        td_rvas, i = [], 0
        while True:
            i = self.data.find(td_name, i)
            if i < 0:
                break
            td_rvas.append(i - 16)   # TypeDescriptor: pVFTable, spare, name
            i += 1
        res = []
        for td in td_rvas:
            # COL: signature(1), offset, cdOffset, pTypeDescriptor RVA, pClassDescriptor RVA, pSelf RVA
            pat = struct.pack("<I", td)
            j = 0
            while True:
                j = self.data.find(pat, j)
                if j < 0:
                    break
                col = j - 12
                if col >= 0 and self.u32(col) == 1 and self.u32(col + 20) == col:
                    off = self.u32(col + 4)
                    ptr = struct.pack("<Q", self.base + col)
                    k = 0
                    while True:
                        k = self.data.find(ptr, k)
                        if k < 0:
                            break
                        if k % 8 == 0:
                            res.append((off, k + 8))
                        k += 8
                j += 1
        return res

    def slots(self, vt):
        n = []
        while True:
            v = self.u64(vt + 8 * len(n))
            rva = v - self.base
            if not self.is_code(rva):
                return n
            n.append(rva)

    def describe(self, rva, max_insn=60):
        """first instructions of the function at rva: strings referenced, direct calls, size estimate"""
        code = self.data[rva:rva + 1200]
        strs, calls, n, last = [], [], 0, ""
        for ins in self.md.disasm(code, self.base + rva):
            n += 1
            last = "%s %s" % (ins.mnemonic, ins.op_str)
            if "rip" in ins.op_str and ins.mnemonic in ("lea", "mov"):
                # target = next instruction + disp
                try:
                    disp = int(ins.op_str.split("rip")[1].split("]")[0].replace(" ", ""), 16)
                except ValueError:
                    disp = None
                if disp is not None:
                    tgt = ins.address + ins.size + disp - self.base
                    s = self.cstr(tgt)
                    if s:
                        strs.append(s)
            if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
                calls.append(int(ins.op_str, 16) - self.base)
            if ins.mnemonic in ("ret", "int3") or n >= max_insn:
                break
        return n, strs, calls, last


def main():
    dll = Dll(sys.argv[1])
    if sys.argv[2] == "--classes":
        for c in dll.classes(sys.argv[3]):
            print(c)
        return
    cls = sys.argv[2]
    vts = dll.vtables(cls)
    if not vts:
        print("no vtable found for", cls)
        return
    for off, vt in vts:
        s = dll.slots(vt)
        print("%s: vtable rva 0x%x (this-offset %d), %d slots" % (cls, vt, off, len(s)))
    off, vt = min(vts)
    s = dll.slots(vt)
    for a in sys.argv[3:]:
        i = int(a)
        if i >= len(s):
            print("  slot %d: beyond the vtable" % i)
            continue
        n, strs, calls, last = dll.describe(s[i])
        sub = []
        for c in calls[:3]:
            _, cs, _, _ = dll.describe(c, 200)
            sub += cs[:3]
        print("  slot %3d -> rva 0x%x: %d insns, ends '%s'" % (i, s[i], n, last))
        if strs:
            print("            strings:", strs[:4])
        if sub:
            print("            strings in callees:", sub[:4])


if __name__ == "__main__":
    main()
