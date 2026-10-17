#!/usr/bin/env python3
"""Combine PsyQ signature matches across all SDK versions, keeping the
longest non-overlapping matches. Output: analysis symbol file."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe
from psyq_match import load_version, match_version

exe = PsxExe(sys.argv[1]); sigdir = sys.argv[2]
allm = {}
for v in sorted(x for x in os.listdir(sigdir) if x.isdigit()):
    for addr, ln, lib, obj, labels in match_version(exe, load_version(sigdir, v), 12):
        key = (addr, lib, obj)
        if key not in allm or ln > allm[key][0]:
            allm[key] = (ln, v, labels)
# count locations per obj
locs = {}
for (addr, lib, obj), (ln, v, labels) in allm.items():
    locs.setdefault((lib, obj), set()).add(addr)
cands = sorted(allm.items(), key=lambda kv: -kv[1][0])
taken = []  # (start,end)
chosen = []
for (addr, lib, obj), (ln, v, labels) in cands:
    if ln <= 32 and len(locs[(lib, obj)]) > 1:
        continue
    if ln < 16:
        continue
    end = addr + ln
    if any(not (end <= s or addr >= e) for s, e in taken):
        continue
    taken.append((addr, end)); chosen.append((addr, ln, lib, obj, v, labels))
syms = {}
for addr, ln, lib, obj, v, labels in sorted(chosen):
    print(f"# {addr:08x} {ln:6d} {lib}:{obj} (v{v})")
    for lab in labels:
        nm = lab["name"]
        if nm.startswith(("loc_", "locret_")):
            continue
        a = addr + lab["offset"]
        if a not in syms:
            syms[a] = nm
            print(f"{a:08x} {nm} {lib}:{obj}")
