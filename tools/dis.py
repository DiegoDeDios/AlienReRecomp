#!/usr/bin/env python3
"""Quick disassembly: dis.py <exe> <start_hex> <count>"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe
from psx.mips import decode, disasm
exe = PsxExe(sys.argv[1])
a = int(sys.argv[2], 16); n = int(sys.argv[3])
for k in range(n):
    pc = a + 4*k
    print(f"{pc:08x}: {exe.word(pc):08x}  {disasm(decode(exe.word(pc), pc))}")
