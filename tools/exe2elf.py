#!/usr/bin/env python3
"""Wrap a PS-X EXE into a little-endian MIPS ELF32 so generic disassemblers
(Ghidra via REA, objdump, ...) can load it. Function symbols from a
functions.json produced by analyze.py are emitted as STT_FUNC entries.

Usage: exe2elf.py <exe> <functions.json> <out.elf>
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe  # noqa: E402


def main():
    exe = PsxExe(sys.argv[1])
    funcs = json.load(open(sys.argv[2]))
    out = sys.argv[3]
    code_end = funcs["code_end"]
    text = exe.text
    base = exe.base
    ncode = code_end - base

    # string tables
    shstr = b"\0.text\0.data\0.bss\0.symtab\0.strtab\0.shstrtab\0"

    def shname(n):
        return shstr.index(n.encode() + b"\0")

    strtab = bytearray(b"\0")
    syms = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]
    for f in funcs["functions"]:
        off = len(strtab)
        strtab += f["name"].encode() + b"\0"
        size = max(0, f["end"] - f["entry"])
        # st_name, st_value, st_size, st_info(GLOBAL|FUNC), st_other, shndx=1
        syms.append(struct.pack("<IIIBBH", off, f["entry"], size,
                                (1 << 4) | 2, 0, 1))
    symtab = b"".join(syms)

    data_off = 0x1100  # congruent with vaddr 0x80010100 mod 0x1000
    blob = bytearray(data_off)
    blob += text
    while len(blob) % 4:
        blob += b"\0"
    symtab_off = len(blob)
    blob += symtab
    strtab_off = len(blob)
    blob += strtab
    shstr_off = len(blob)
    blob += shstr
    while len(blob) % 4:
        blob += b"\0"
    shoff = len(blob)

    bss_start = base + len(text)
    bss_end = 0x80120000

    def sh(name, typ, flags, addr, off, size, link=0, info=0, align=4,
           entsize=0):
        return struct.pack("<10I", name, typ, flags, addr, off, size, link,
                           info, align, entsize)

    shdrs = [
        sh(0, 0, 0, 0, 0, 0, align=0),
        sh(shname(".text"), 1, 6, base, data_off, ncode),
        sh(shname(".data"), 1, 3, code_end, data_off + ncode,
           len(text) - ncode),
        sh(shname(".bss"), 8, 3, bss_start, data_off + len(text),
           bss_end - bss_start),
        sh(shname(".symtab"), 2, 0, 0, symtab_off, len(symtab), link=5,
           info=1, entsize=16),
        sh(shname(".strtab"), 3, 0, 0, strtab_off, len(strtab), align=1),
        sh(shname(".shstrtab"), 3, 0, 0, shstr_off, len(shstr), align=1),
    ]
    blob += b"".join(shdrs)

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    # e_flags: EF_MIPS_ARCH_32 | EF_MIPS_ABI_O32. The code is MIPS I, a
    # subset of MIPS32; REA's Ghidra lane only admits the MIPS32 header.
    ehdr = ident + struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, exe.pc0, 52, shoff,
                               0x50001000, 52, 32, 1, 40, len(shdrs), 6)
    phdr = struct.pack("<8I", 1, data_off, base, base, len(text),
                       bss_end - base, 7, 0x1000)
    blob[0:52] = ehdr
    blob[52:84] = phdr
    open(out, "wb").write(blob)
    print(f"wrote {out}: {len(funcs['functions'])} symbols")


if __name__ == "__main__":
    main()
