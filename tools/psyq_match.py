#!/usr/bin/env python3
"""Identify PsyQ library objects inside a PS-X EXE using lab313ru's
psx_psyq_signatures JSON database.

Usage: psyq_match.py <exe> <sigdir> [version] > symbols.txt
Without a version it scores every version and reports the best one.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe  # noqa: E402


def compile_sig(sig):
    parts = sig.split()
    rx = b""
    fixed = 0
    for p in parts:
        if p == "??":
            rx += b"."
        else:
            rx += re.escape(bytes([int(p, 16)]))
            fixed += 1
    return re.compile(rx, re.DOTALL), len(parts), fixed


def load_version(sigdir, ver):
    objs = []
    d = os.path.join(sigdir, ver)
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".json"):
            continue
        for e in json.load(open(os.path.join(d, fn))):
            if not e.get("sig"):
                continue
            objs.append((fn[:-5], e["name"], e["sig"], e.get("labels", [])))
    return objs


def match_version(exe, objs, min_fixed=16):
    found = []
    for lib, obj, sig, labels in objs:
        rx, ln, fixed = compile_sig(sig)
        if fixed < min_fixed:
            continue
        for m in rx.finditer(exe.text):
            if m.start() & 3:
                continue
            found.append((exe.base + m.start(), ln, lib, obj, labels))
    return found


def main():
    exe = PsxExe(sys.argv[1])
    sigdir = sys.argv[2]
    vers = [sys.argv[3]] if len(sys.argv) > 3 else sorted(
        v for v in os.listdir(sigdir) if v.isdigit())
    best = None
    for v in vers:
        objs = load_version(sigdir, v)
        found = match_version(exe, objs)
        cover = sum(f[1] for f in found)
        print(f"# version {v}: {len(found)} objs, {cover} bytes",
              file=sys.stderr)
        if best is None or cover > best[0]:
            best = (cover, v, found)
    cover, v, found = best
    print(f"# best version {v}", file=sys.stderr)
    print(f"# psyq version {v}")
    seen = {}
    for addr, ln, lib, obj, labels in sorted(found):
        print(f"# {addr:08x} {ln:6d} {lib}:{obj}")
        for lab in labels:
            nm = lab["name"]
            if nm.startswith(("loc_", "text_", "locret_")):
                continue
            a = addr + lab["offset"]
            if a in seen:
                continue
            seen[a] = nm
            print(f"{a:08x} {nm} {lib}:{obj}")


if __name__ == "__main__":
    main()
