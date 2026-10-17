#!/usr/bin/env python3
"""Control-flow analysis of a PS-X EXE: discovers functions, basic blocks,
jump tables and indirect call sites. Writes analysis/<name>.functions.json.

Usage: analyze.py <exe> <code_end_hex> <out.json> [symbols.txt ...]
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe  # noqa: E402
from psx.mips import decode  # noqa: E402

N = PsxExe.norm


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.insns = set()      # addresses of instructions (incl delay slots)
        self.block_starts = {entry}
        self.calls = set()      # direct jal targets
        self.tailcalls = set()  # j to other function entries
        self.indirect_calls = []  # jalr sites
        self.jumptables = {}    # jr site -> {"table": addr, "targets": [...]}
        self.unresolved_jr = []  # jr sites not resolved
        self.returns = []


class Analyzer:
    def __init__(self, exe, code_end, extra_ranges=()):
        self.exe = exe
        self.code_start = exe.base
        self.code_end = code_end
        # additional code regions (e.g. hand-written asm placed after .data)
        self.ranges = [(exe.base, code_end)] + [tuple(r) for r in extra_ranges]
        self.entries = set()
        self.funcs = {}
        self.names = {}

    def in_code(self, a):
        a = N(a)
        if a & 3:
            return False
        for lo, hi in self.ranges:
            if lo <= a < hi:
                return True
        return False

    def insn(self, a):
        return decode(self.exe.word(a), N(a))

    # --- jump table resolution -------------------------------------
    def find_def(self, reg, pc, limit=32):
        """Walk backwards from pc (exclusive) to the instruction writing reg."""
        from psx.mips import reads_writes
        a = pc - 4
        for _ in range(limit):
            if not self.in_code(a):
                return None
            i = self.insn(a)
            _, wr = reads_writes(i)
            if reg in wr:
                # a write in a branch delay slot is fine; a write right after
                # a branch belongs to another path, keep it simple
                return i
            a -= 4
        return None

    def const_value(self, reg, pc, depth=0):
        if depth > 6:
            return None
        i = self.find_def(reg, pc)
        if i is None:
            return None
        if i.mnem == "lui":
            return (i.imm << 16) & 0xFFFFFFFF
        if i.mnem in ("addiu", "addi"):
            b = self.const_value(i.rs, i.pc, depth + 1) if i.rs else 0
            return None if b is None else (b + i.simm) & 0xFFFFFFFF
        if i.mnem == "ori":
            b = self.const_value(i.rs, i.pc, depth + 1) if i.rs else 0
            return None if b is None else (b | i.imm)
        return None

    def resolve_jumptable(self, f, jr_pc, reg):
        """gcc 2.x switch: sltiu c, idx, N; beqz c, default; base=&table;
        off = idx<<2; p = base+off; t = lw lo(p); jr t"""
        lw = self.find_def(reg, jr_pc)
        if lw is None or lw.mnem != "lw":
            return None
        addu = self.find_def(lw.rs, lw.pc)
        if addu is None or addu.mnem != "addu":
            return None
        table = None
        idx_shift = None
        for a, b in ((addu.rs, addu.rt), (addu.rt, addu.rs)):
            c = self.const_value(a, addu.pc)
            if c is not None:
                sh = self.find_def(b, addu.pc)
                if sh is not None and sh.mnem == "sll" and sh.sa == 2:
                    table = c
                    idx_shift = sh
                    break
        if table is None:
            return None
        table = N((table + lw.simm) & 0xFFFFFFFF)
        # bound: sltiu x, idx, N before the sll
        bound = None
        a = idx_shift.pc - 4
        idx = idx_shift.rt
        for _ in range(16):
            if not self.in_code(a):
                break
            i = self.insn(a)
            if i.mnem == "sltiu" and i.rs == idx:
                bound = i.imm
                break
            a -= 4
        targets = []
        n = bound if bound else 256
        for k in range(n):
            ta = table + 4 * k
            if not self.exe.contains(ta):
                break
            t = self.exe.word(ta)
            if not self.in_code(t):
                if bound:
                    return None
                break
            targets.append(N(t))
        if not targets:
            return None
        return {"table": table, "targets": targets, "bounded": bool(bound)}

    # --- function exploration --------------------------------------
    def explore(self, entry):
        f = Func(entry)
        work = [entry]
        seen = set()
        while work:
            pc = work.pop()
            if pc in seen:
                continue
            f.block_starts.add(pc)
            while True:
                if pc in seen:
                    break
                if not self.in_code(pc):
                    break
                seen.add(pc)
                f.insns.add(pc)
                i = self.insn(pc)
                if i.kind == "invalid":
                    break
                if i.is_branch:
                    ds = pc + 4
                    # the delay slot is not marked seen: it may also be a
                    # branch target in its own right (entered directly)
                    f.insns.add(ds)
                    nxt = pc + 8
                    k = i.kind
                    if k == "branch":
                        if i.mnem in ("bltzal", "bgezal"):
                            f.calls.add(i.target)
                            work.append(nxt)
                        else:
                            work.append(i.target)
                            # unconditional "b" = beq zero,zero
                            if not (i.mnem == "beq" and i.rs == 0 and i.rt == 0):
                                work.append(nxt)
                        f.block_starts.add(nxt)
                    elif k == "jump":
                        t = i.target
                        if t in self.entries and t != entry:
                            f.tailcalls.add(t)
                        else:
                            work.append(t)
                    elif k == "jal":
                        f.calls.add(i.target)
                        work.append(nxt)
                        f.block_starts.add(nxt)
                    elif k == "jalr":
                        f.indirect_calls.append(pc)
                        work.append(nxt)
                        f.block_starts.add(nxt)
                    elif k == "jr":
                        if i.rs == 31:
                            f.returns.append(pc)
                        else:
                            jt = self.resolve_jumptable(f, pc, i.rs)
                            if jt:
                                f.jumptables[pc] = jt
                                work.extend(jt["targets"])
                                f.block_starts.update(jt["targets"])
                            else:
                                f.unresolved_jr.append(pc)
                    break
                if i.kind == "break":
                    break
                pc += 4
        return f

    def run(self, roots):
        self.entries.update(N(r) for r in roots)
        changed = True
        while changed:
            changed = False
            for e in sorted(self.entries):
                if e in self.funcs:
                    continue
                f = self.explore(e)
                self.funcs[e] = f
                for c in f.calls:
                    c = N(c)
                    if self.in_code(c) and c not in self.entries:
                        self.entries.add(c)
                        changed = True
        # Re-explore so that 'j' to entries discovered later become tail calls
        for e in sorted(self.entries):
            self.funcs[e] = self.explore(e)

    # --- extra root discovery --------------------------------------
    def scan_pointer_roots(self, data_start, data_end):
        """Words in data that point into code at plausible function starts."""
        self._jt_cache = self.jumptable_targets()
        roots = set()
        for a in range(data_start, data_end, 4):
            if any(lo <= a < hi for lo, hi in self.ranges):
                continue
            w = self.exe.word(a)
            if self.in_code(w) and self.plausible_start(N(w)):
                roots.add(N(w))
        return roots

    def scan_immediate_roots(self):
        """lui/addiu pairs in code that build code addresses."""
        self._jt_cache = self.jumptable_targets()
        roots = set()
        hi = {}
        for a in (x for lo, h in self.ranges for x in range(lo, h, 4)):
            i = self.insn(a)
            if i.mnem == "lui":
                hi[i.rt] = (i.imm << 16, a)
            elif i.mnem in ("addiu", "ori") and i.rs in hi and \
                    a - hi[i.rs][1] < 64:
                v = hi[i.rs][0] + (i.simm if i.mnem == "addiu" else i.imm)
                v &= 0xFFFFFFFF
                if self.in_code(v) and self.plausible_start(N(v)):
                    roots.add(N(v))
            if i.is_branch:
                pass
        return roots

    def jumptable_targets(self):
        t = set()
        for f in self.funcs.values():
            for jt in f.jumptables.values():
                t.update(jt["targets"])
        return t

    def plausible_start(self, a):
        """Function start heuristic: preceded by jr ra/delay or nops, or
        starts with a stack adjust."""
        if a in getattr(self, "_jt_cache", ()):
            return False
        i = self.insn(a)
        if i.mnem == "addiu" and i.rt == 29 and i.rs == 29 and i.simm < 0:
            return True
        p2 = a - 8
        if self.in_code(p2):
            if self.exe.word(p2) == 0x03E00008:
                return True
            j = self.insn(p2)
            # after any unconditional transfer + its delay slot (only in the
            # extra hand-written regions: in the main region plain integers
            # in data would produce too many false positives)
            in_extra = any(lo <= a < hi for lo, hi in self.ranges[1:])
            if in_extra and (j.kind in ("jump", "jr") or (
                    j.kind == "branch" and j.rs == 0 and
                    (j.mnem == "bgez" or (j.mnem == "beq" and j.rt == 0)))):
                return True
        return False

    def gap_roots(self):
        """Look in uncovered code for function prologues following a
        return or padding."""
        covered = set()
        for f in self.funcs.values():
            covered |= f.insns
        roots = set()
        for lo, end in self.ranges:
            a = lo
            while a < end:
                if a not in covered:
                    i = self.insn(a)
                    if i.word != 0 and i.kind != "invalid":
                        prev = a - 4
                        pw = self.exe.word(prev) if self.in_code(prev) else 0
                        if prev in covered or pw == 0:
                            roots.add(a)
                            # skip to next covered
                            while a < end and a not in covered:
                                a += 4
                            continue
                a += 4
        return roots


def load_symbols(paths):
    names = {}
    for p in paths:
        for line in open(p):
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            a = int(parts[0], 16)
            nm = parts[1]
            if nm.startswith("text_"):
                continue
            names[N(a)] = nm
    return names


def main():
    exe = PsxExe(sys.argv[1])
    code_end = int(sys.argv[2], 16)
    out = sys.argv[3]
    names = load_symbols(sys.argv[4:])
    an = Analyzer(exe, code_end)
    roots = {exe.pc0}
    roots |= {a for a in names if an.in_code(a)}
    an.run(roots)
    for rnd in range(6):
        extra = an.scan_pointer_roots(code_end, exe.end)
        extra |= an.scan_immediate_roots()
        extra |= an.gap_roots()
        extra -= an.entries
        print(f"round {rnd}: {len(an.entries)} funcs, +{len(extra)} roots",
              file=sys.stderr)
        if not extra:
            break
        an.run(extra)
    # report
    covered = set()
    for f in an.funcs.values():
        covered |= f.insns
    total = (code_end - exe.base) // 4
    nonzero_uncovered = [a for a in range(exe.base, code_end, 4)
                         if a not in covered and exe.word(a) != 0]
    print(f"functions: {len(an.funcs)}  coverage {len(covered)}/{total}  "
          f"uncovered non-zero words: {len(nonzero_uncovered)}",
          file=sys.stderr)
    unresolved = sum(len(f.unresolved_jr) for f in an.funcs.values())
    jts = sum(len(f.jumptables) for f in an.funcs.values())
    print(f"jumptables: {jts}, unresolved jr: {unresolved}, indirect calls: "
          f"{sum(len(f.indirect_calls) for f in an.funcs.values())}",
          file=sys.stderr)
    res = {"exe": sys.argv[1], "code_start": exe.base, "code_end": code_end,
           "entry": exe.pc0, "functions": []}
    for e in sorted(an.funcs):
        f = an.funcs[e]
        res["functions"].append({
            "entry": e,
            "name": names.get(e, f"func_{e:08X}"),
            "start": min(f.insns) if f.insns else e,
            "end": (max(f.insns) + 4) if f.insns else e,
            "ninsns": len(f.insns),
            "blocks": sorted(f.block_starts),
            "calls": sorted(N(c) for c in f.calls),
            "tailcalls": sorted(f.tailcalls),
            "indirect_calls": f.indirect_calls,
            "jumptables": {str(k): v for k, v in f.jumptables.items()},
            "unresolved_jr": f.unresolved_jr,
            "returns": f.returns,
        })
    res["uncovered"] = nonzero_uncovered
    json.dump(res, open(out, "w"), indent=0)


if __name__ == "__main__":
    main()
