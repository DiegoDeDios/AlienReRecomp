#!/usr/bin/env python3
"""Static recompiler: PS1 MIPS (R3000A) executable -> C.

Every discovered function becomes `void <PFX>_XXXXXXXX(CPU *c)` operating on
the shared CPU context (see src/runtime/recomp.h). Branch delay slots and
(most) load delay slots are honoured; switch jump tables become C switches;
calls to the BIOS vectors (A0/B0/C0) become HLE calls.

Usage:
  recomp.py --exe disc/ALIEN.BIN --code-end 80091300 --prefix ALIEN \
            --syms analysis/psyq_symbols_alien.txt --out src/gen/alien \
            [--overrides overrides.txt] [--split 48]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx.exe import PsxExe  # noqa: E402
from psx.mips import decode, reads_writes, REG  # noqa: E402
from analyze import Analyzer, load_symbols  # noqa: E402

N = PsxExe.norm


def hx(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


class FuncEmitter:
    def __init__(self, rc, f):
        self.rc = rc
        self.f = f
        self.exe = rc.exe
        self.lines = []
        self.labels = set()
        self.warnings = []

    def R(self, n):
        return "0u" if n == 0 else f"R({n})"

    def W(self, n, expr):
        if n == 0:
            return f"(void)({expr});"
        return f"R({n}) = {expr};"

    def insn(self, pc):
        return decode(self.exe.word(pc), pc)

    # ---------------------------------------------------------------
    def emit_simple(self, i, wr_override=None):
        """Emit a non-control-flow instruction. wr_override: if given,
        the loaded value goes into this C variable instead of R(rt)."""
        R, W = self.R, self.W
        m = i.mnem
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        imm, simm = i.imm, i.simm

        def dst(n, expr):
            if wr_override is not None and n == wr_override[0]:
                return f"{wr_override[1]} = {expr};"
            return W(n, expr)

        k = i.kind
        if k == "alu":
            if i.op == 0:
                if i.word == 0:
                    return "/* nop */"
                ops = {
                    "sll": f"{R(rt)} << {sa}",
                    "srl": f"{R(rt)} >> {sa}",
                    "sra": f"(uint32_t)((int32_t){R(rt)} >> {sa})",
                    "sllv": f"{R(rt)} << ({R(rs)} & 31)",
                    "srlv": f"{R(rt)} >> ({R(rs)} & 31)",
                    "srav": f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))",
                    "add": f"{R(rs)} + {R(rt)}",
                    "addu": f"{R(rs)} + {R(rt)}",
                    "sub": f"{R(rs)} - {R(rt)}",
                    "subu": f"{R(rs)} - {R(rt)}",
                    "and": f"{R(rs)} & {R(rt)}",
                    "or": f"{R(rs)} | {R(rt)}",
                    "xor": f"{R(rs)} ^ {R(rt)}",
                    "nor": f"~({R(rs)} | {R(rt)})",
                    "slt": f"(uint32_t)((int32_t){R(rs)} < (int32_t){R(rt)})",
                    "sltu": f"(uint32_t)({R(rs)} < {R(rt)})",
                    "mfhi": "c->hi",
                    "mflo": "c->lo",
                }
                if m in ops:
                    code = dst(rd, ops[m])
                    # register moves keep PGXP precision data
                    if wr_override is None and m in ("addu", "or") and rd != 0 \
                            and (rs == 0) != (rt == 0):
                        code += f" PGXP_MOV({rd}, {rs or rt});"
                    return code
                if m == "mthi":
                    return f"c->hi = {R(rs)};"
                if m == "mtlo":
                    return f"c->lo = {R(rs)};"
            else:
                ops = {
                    "addi": f"{R(rs)} + {hx(simm)}",
                    "addiu": f"{R(rs)} + {hx(simm)}",
                    "slti": f"(uint32_t)((int32_t){R(rs)} < {simm})",
                    "sltiu": f"(uint32_t)({R(rs)} < {hx(simm)})",
                    "andi": f"{R(rs)} & 0x{imm:X}u",
                    "ori": f"{R(rs)} | 0x{imm:X}u",
                    "xori": f"{R(rs)} ^ 0x{imm:X}u",
                    "lui": f"{hx(imm << 16)}",
                }
                if rs == 0 and m in ("addi", "addiu"):
                    ops[m] = hx(simm)
                if rs == 0 and m == "ori":
                    ops[m] = f"0x{imm:X}u"
                code = dst(rt, ops[m])
                if wr_override is None and m in ("addiu", "addi", "ori") \
                        and imm == 0 and rs != 0 and rt != 0:
                    code += f" PGXP_MOV({rt}, {rs});"
                return code
        if k == "mult":
            if m == "mult":
                return (f"{{ int64_t p = (int64_t)(int32_t){R(rs)} * "
                        f"(int32_t){R(rt)}; c->lo = (uint32_t)p; "
                        f"c->hi = (uint32_t)((uint64_t)p >> 32); }}")
            if m == "multu":
                return (f"{{ uint64_t p = (uint64_t){R(rs)} * {R(rt)}; "
                        f"c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}")
            if m == "div":
                return f"cpu_div(c, {R(rs)}, {R(rt)});"
            if m == "divu":
                return f"cpu_divu(c, {R(rs)}, {R(rt)});"
        ea = f"{R(rs)} + {hx(simm)}" if simm else R(rs)
        if k == "load":
            fn = {"lb": "(uint32_t)(int32_t)(int8_t)MEM_R8",
                  "lbu": "(uint32_t)MEM_R8",
                  "lh": "(uint32_t)(int32_t)(int16_t)MEM_R16",
                  "lhu": "(uint32_t)MEM_R16",
                  "lw": "MEM_R32"}
            if m == "lw" and wr_override is None and rt != 0:
                return (f"{{ uint32_t a_ = {ea}; {self.W(rt, 'MEM_R32(a_)')} "
                        f"PGXP_LW({rt}, a_); }}")
            if m in fn:
                return dst(rt, f"{fn[m]}({ea})")
            if m == "lwl":
                return dst(rt, f"mem_lwl({R(rt)}, {ea})")
            if m == "lwr":
                return dst(rt, f"mem_lwr({R(rt)}, {ea})")
        if k == "store":
            fn = {"sb": "MEM_W8", "sh": "MEM_W16", "sw": "MEM_W32",
                  "swl": "mem_swl", "swr": "mem_swr"}
            if m == "sw":
                return (f"{{ uint32_t a_ = {ea}; MEM_W32(a_, {R(rt)}); "
                        f"PGXP_SW(a_, {rt}); }}")
            return f"{fn[m]}({ea}, {R(rt)});"
        if k == "lwc2":
            if 12 <= rt <= 15:
                return (f"{{ uint32_t a_ = {ea}; gte_write_data(c, {rt}, "
                        f"MEM_R32(a_)); PGXP_LWC2({rt}, a_); }}")
            return f"gte_write_data(c, {rt}, MEM_R32({ea}));"
        if k == "swc2":
            if 12 <= rt <= 15:
                return (f"{{ uint32_t a_ = {ea}; MEM_W32(a_, "
                        f"gte_read_data(c, {rt})); PGXP_SWC2(a_, {rt}); }}")
            return f"MEM_W32({ea}, gte_read_data(c, {rt}));"
        if k == "cop2":
            if m == "mfc2":
                code = dst(rt, f"gte_read_data(c, {rd})")
                if 12 <= rd <= 15 and wr_override is None and rt != 0:
                    code += f" PGXP_MFC2({rt}, {rd});"
                return code
            if m == "cfc2":
                return dst(rt, f"gte_read_ctrl(c, {rd})")
            if m == "mtc2":
                if 12 <= rd <= 15:
                    return (f"gte_write_data(c, {rd}, {R(rt)}); "
                            f"PGXP_MTC2({rd}, {rt});")
                return f"gte_write_data(c, {rd}, {R(rt)});"
            if m == "ctc2":
                return f"gte_write_ctrl(c, {rd}, {R(rt)});"
        if k == "gte":
            return f"gte_command(c, 0x{i.word & 0x1FFFFFF:07X}u);"
        if k == "cop0":
            if m == "mfc0":
                return dst(rt, f"cop0_read(c, {rd})")
            if m == "mtc0":
                return f"cop0_write(c, {rd}, {R(rt)}); CHECK_IRQ();"
            if m == "rfe":
                return "cop0_rfe(c);"
        if k == "syscall":
            return (f"c->pc = {hx(i.pc)}; "
                    f"hle_syscall(c, 0x{(i.word >> 6) & 0xFFFFF:X}u); "
                    f"CHECK_IRQ();")
        if k == "break":
            return (f"hle_break(c, {hx(i.pc)}, "
                    f"0x{(i.word >> 6) & 0xFFFFF:X}u);")
        if k == "invalid":
            self.warnings.append(f"invalid insn at {i.pc:08x}")
            return f"hle_invalid(c, {hx(i.pc)}, {hx(i.word)});"
        raise ValueError(f"unhandled {m} at {i.pc:08x}")

    def is_load_like(self, i):
        return i.kind == "load" or i.mnem in ("mfc0", "mfc2", "cfc2")

    def emit_seq(self, i, nxt):
        """Emit instruction i honouring a load delay hazard with nxt (a
        non-branch instruction that immediately follows). Returns
        (code, consumed_next)."""
        if self.is_load_like(i) and nxt is not None and i.rt != 0 and \
                not nxt.is_branch and nxt.kind != "invalid":
            rd_, wr = reads_writes(nxt)
            pair = (i.mnem in ("lwl", "lwr") and nxt.mnem in ("lwl", "lwr")
                    and nxt.rt == i.rt)
            if i.rt in rd_ and not pair:
                self.warnings.append(
                    f"load-delay hazard at {i.pc:08x} ({REG[i.rt]})")
                code = "{ uint32_t ld_ = 0; "
                code += self.emit_simple(i, wr_override=(i.rt, "ld_")) + " "
                code += self.emit_simple(nxt) + " "
                if i.rt not in wr:
                    code += self.W(i.rt, "ld_")
                code += " }"
                return code, True
        return self.emit_simple(i), False

    # ---------------------------------------------------------------
    def label(self, a):
        return f"L_{a:08X}"

    def goto(self, a, backward):
        if backward:
            return f"{{ CHECK_IRQ(); goto {self.label(a)}; }}"
        return f"goto {self.label(a)};"

    def call(self, target, ret_addr):
        """Direct call to a function entry."""
        t = N(target)
        name = self.rc.func_name(t)
        if name is None:
            return f"R(31) = {hx(ret_addr)}; call_indirect(c, {hx(t)});"
        return f"R(31) = {hx(ret_addr)}; {name}(c);"

    def bios_vector(self, pc, reg):
        """Return 0xA0/0xB0/0xC0 if reg holds a BIOS vector constant."""
        v = self.rc.an.const_value(reg, pc)
        if v is not None and (v & 0x1FFFFFFF) in (0xA0, 0xB0, 0xC0):
            return v & 0xFF
        return None

    def emit_branch(self, i, ds_pc, out):
        """Emit branch/jump at i with delay slot at ds_pc."""
        R = self.R
        f = self.f
        d = self.insn(ds_pc)
        if d.is_branch:
            self.warnings.append(f"branch in delay slot at {ds_pc:08x}")
        dcode = self.emit_simple(d) if not d.is_branch else \
            f"hle_invalid(c, {hx(ds_pc)}, {hx(d.word)}); /* branch in ds */"
        m, k = i.mnem, i.kind
        pc = i.pc
        if k == "branch":
            conds = {
                "beq": f"{R(i.rs)} == {R(i.rt)}",
                "bne": f"{R(i.rs)} != {R(i.rt)}",
                "blez": f"(int32_t){R(i.rs)} <= 0",
                "bgtz": f"(int32_t){R(i.rs)} > 0",
                "bltz": f"(int32_t){R(i.rs)} < 0",
                "bgez": f"(int32_t){R(i.rs)} >= 0",
                "bltzal": f"(int32_t){R(i.rs)} < 0",
                "bgezal": f"(int32_t){R(i.rs)} >= 0",
            }
            t = i.target
            back = t <= pc
            if m in ("bltzal", "bgezal"):
                out.append(f"{{ int cond_ = {conds[m]}; R(31) = {hx(pc + 8)}; "
                           f"{dcode} if (cond_) {{ {self.call(t, pc + 8)} }} }}")
                return
            if m == "beq" and i.rs == 0 and i.rt == 0:
                out.append(f"{dcode} {self.goto(t, back)}")
                return
            out.append(f"{{ int cond_ = {conds[m]}; {dcode} "
                       f"if (cond_) {self.goto(t, back)} }}")
            return
        if k == "jump":
            t = i.target
            if t in f.tailcalls and t not in f.insns:
                # tail call: ra is left untouched, callee returns for us
                out.append(f"{dcode} {self.rc.func_name(t)}(c); return;")
                return
            out.append(f"{dcode} {self.goto(t, t <= pc)}")
            return
        if k == "jal":
            if (pc + 8) in getattr(self, "local_links", {}):
                out.append(f"{dcode} R(31) = {hx(pc + 8)}; "
                           f"goto {self.label(i.target)}; /* local jal */")
                return
            out.append(f"{dcode} {self.call(i.target, pc + 8)}")
            return
        if k == "jalr":
            out.append(f"{{ uint32_t tgt_ = {R(i.rs)}; {dcode} "
                       f"{self.W(i.rd, hx(pc + 8))} call_indirect(c, tgt_); }}")
            return
        if k == "jr":
            if i.rs == 31:
                links = getattr(self, "local_links", {})
                if links:
                    cases = " ".join(f"case {hx(l)}: goto {self.label(l)};"
                                     for l in sorted(links))
                    out.append(f"{{ uint32_t ra_ = R(31); {dcode} "
                               f"if (ra_ != entry_ra_) {{ switch (ra_ | 0x80000000u) "
                               f"{{ {cases} default: break; }} }} RETURN(ra_); }}")
                    return
                out.append(f"{{ uint32_t ra_ = R(31); {dcode} RETURN(ra_); }}")
                return
            jt = f.jumptables.get(pc)
            if jt:
                cases = " ".join(
                    f"case {hx(t)}: goto {self.label(t)};"
                    for t in sorted(set(jt["targets"])))
                out.append(f"{{ uint32_t tgt_ = R({i.rs}) | 0x80000000u; {dcode} "
                           f"switch (tgt_) {{ {cases} default: "
                           f"bad_jump(c, {hx(pc)}, tgt_); }} }}")
                return
            vec = self.bios_vector(pc, i.rs)
            if vec is not None:
                out.append(f"{dcode} bios_call_{vec:02X}(c); return;")
                return
            self.warnings.append(f"unresolved jr at {pc:08x}")
            out.append(f"{{ uint32_t tgt_ = {R(i.rs)}; {dcode} "
                       f"jump_indirect(c, tgt_); return; }}")
            return
        raise ValueError(m)

    # ---------------------------------------------------------------
    def emit(self, resume_points=()):
        f = self.f
        addrs = sorted(f.insns)
        resume_points = sorted(resume_points)
        # labels: branch/jump targets inside function + jumptable targets
        targets = set()
        for a in addrs:
            i = self.insn(a)
            if i.kind == "branch" and i.mnem not in ("bltzal", "bgezal"):
                targets.add(i.target)
            elif i.kind == "jump" and (i.target not in f.tailcalls or
                                       i.target in f.insns):
                targets.add(i.target)
        for jt in f.jumptables.values():
            targets.update(jt["targets"])
        targets.update(resume_points)
        # "local" jal: a call into this function's own body (hand-written
        # code that uses jal/jr ra as a computed goto). Compiled as
        # ra=link; goto target, with jr ra dispatching over the links.
        self.local_links = {}
        for a in addrs:
            i = self.insn(a)
            if i.kind == "jal" and i.target in f.insns and i.target != f.entry:
                self.local_links[a + 8] = i.target
                targets.add(i.target)
                targets.add(a + 8)
        # delay slot addresses (emitted inside their branch)
        delay = set()
        for a in addrs:
            if self.insn(a).is_branch:
                delay.add(a + 4)
        out = []
        name = self.rc.func_name(f.entry)
        out.append(f"/* {self.rc.sym_comment(f.entry)} */")
        if resume_points:
            out.append(f"static void {name}__body(CPU *c, uint32_t resume_at) {{")
            out.append("    FUNC_ENTER_RESUMABLE(resume_at);")
            cases = " ".join(f"case {hx(r)}: goto {self.label(r)};"
                             for r in resume_points)
            out.append(f"    if (resume_at) {{ switch (resume_at) {{ {cases} "
                       f"default: bad_jump(c, {hx(f.entry)}, resume_at); }} }}")
        else:
            out.append(f"void {name}(CPU *c) {{")
            out.append(f"    FUNC_ENTER({hx(f.entry)}, {name});")
        if f.entry != addrs[0]:
            out.append(f"    goto {self.label(f.entry)};")
        idx = 0
        prev_term = True
        prev_addr = None
        consumed = set()
        n = len(addrs)
        while idx < n:
            a = addrs[idx]
            idx += 1
            if a in consumed:
                continue
            is_label = a in targets or a == f.entry
            if a in delay and not is_label:
                continue
            if prev_addr is not None and a != prev_addr + 4 and not prev_term:
                out.append(f"    goto {self.label(prev_addr + 4)};")
                targets.add(prev_addr + 4)
            if is_label:
                out.append(f"{self.label(a)}:")
                cyc = self.block_len(a, set(addrs), targets)
                out.append(f"    ADD_CYCLES({cyc}); TRACE_BLOCK({hx(a)});")
            i = self.insn(a)
            if a in delay and is_label:
                # entered directly (jump into delay slot); runs then falls
                # through to a+4
                pass
            if i.is_branch:
                buf = []
                self.emit_branch(i, a + 4, buf)
                out.append(f"    /* {a:08X} */ " + buf[0])
                if (a + 4) in targets:
                    # someone jumps into the delay slot: emit it standalone
                    if i.kind in ("branch",) and not (
                            i.mnem == "beq" and i.rs == 0 and i.rt == 0):
                        out.append(f"    goto {self.label(a + 8)};")
                        targets.add(a + 8)
                    out.append(f"{self.label(a + 4)}:")
                    out.append(f"    ADD_CYCLES(1); TRACE_BLOCK({hx(a + 4)});")
                    d = self.insn(a + 4)
                    out.append(f"    /* {a + 4:08X} */ " + self.emit_simple(d))
                    out.append(f"    goto {self.label(a + 8)};")
                    targets.add(a + 8)
                    consumed.add(a + 4)
                    prev_term = True
                    prev_addr = a + 4
                    continue
                consumed.add(a + 4)
                prev_addr = a + 4
                prev_term = i.kind in ("jump", "jr") or (
                    i.kind == "branch" and i.mnem == "beq" and i.rs == 0
                    and i.rt == 0) or (
                    i.kind == "jal" and (a + 8) in self.local_links)
                if i.kind in ("jal", "jalr", "branch") and not prev_term:
                    if (a + 8) not in f.insns:
                        # call that never returns into this function body
                        out.append("    return; /* fallthrough not reached */")
                        prev_term = True
                continue
            nxt = None
            if idx < n and addrs[idx] == a + 4 and (a + 4) not in targets \
                    and (a + 4) not in delay:
                nxt = self.insn(a + 4)
            code, used = self.emit_seq(i, nxt)
            out.append(f"    /* {a:08X} */ {code}")
            if used:
                consumed.add(a + 4)
                prev_addr = a + 4
            else:
                prev_addr = a
            prev_term = i.kind == "break"
            if i.kind == "break":
                out.append("    return;")
        if not prev_term:
            out.append(f"    goto {self.label(prev_addr + 4)}; /* runs off */")
            self.warnings.append(f"{f.entry:08x}: runs off end")
        # Labels referenced but not defined (targets outside emitted set)
        defined = {ln[:-1] for ln in out if ln.startswith("L_") and ln.endswith(":")}
        referenced = set()
        for ln in out:
            for tok in ln.replace(";", " ").split():
                if tok.startswith("L_") and len(tok) == 10:
                    referenced.add(tok)
        for lab in sorted(referenced - defined):
            a = int(lab[2:], 16)
            out.append(f"{lab}: missing_label(c, {hx(a)}); return;")
            self.warnings.append(f"{f.entry:08x}: missing label {lab}")
        out.append("}")
        out.append("")
        if resume_points:
            out.append(f"void {name}(CPU *c) {{ {name}__body(c, 0); }}")
            out.append(f"void {name}__resume(CPU *c, uint32_t at) "
                       f"{{ {name}__body(c, at); }}")
            out.append("")
        return out

    def block_len(self, start, aset, targets):
        n = 0
        a = start
        while a in aset:
            n += 1
            i = self.insn(a)
            if i.is_branch:
                return n + 1
            a += 4
            if a in targets:
                break
        return max(n, 1)


class Recompiler:
    def __init__(self, exe, code_end, prefix, names, overrides, extra=()):
        self.exe = exe
        self.prefix = prefix
        self.names = names
        self.overrides = overrides
        self.an = Analyzer(exe, code_end, extra)
        roots = {exe.pc0} | {a for a in names if self.an.in_code(a)}
        self.an.run(roots)
        for _ in range(8):
            extra = self.an.scan_pointer_roots(code_end, exe.end)
            extra |= self.an.scan_immediate_roots()
            extra |= self.an.gap_roots()
            extra -= self.an.entries
            if not extra:
                break
            self.an.run(extra)
        self.funcs = self.an.funcs
        self.resume = {}  # func entry -> set of resume addresses
        setjmps = {a for a, nm in names.items() if nm == "setjmp"}
        for e, f in self.funcs.items():
            for a in f.insns:
                i = decode(exe.word(a), a)
                if i.kind == "jal" and N(i.target) in setjmps:
                    self.resume.setdefault(e, set()).add(a + 8)

    def func_name(self, a):
        a = N(a)
        if a not in self.funcs:
            return None
        return f"{self.prefix}_{a:08X}"

    def sym_comment(self, a):
        nm = self.names.get(a)
        return f"{a:08X}" + (f" {nm}" if nm else "")

    def generate(self, outdir, split):
        os.makedirs(outdir, exist_ok=True)
        entries = sorted(self.funcs)
        warnings = []
        chunks = [entries[k::split] for k in range(split)]
        hdr = os.path.join(outdir, f"{self.prefix.lower()}_funcs.h")
        with open(hdr, "w") as h:
            h.write("/* generated by tools/recomp.py - do not edit */\n")
            h.write("#pragma once\n#include \"recomp.h\"\n\n")
            for e in entries:
                nm = self.names.get(e)
                h.write(f"void {self.func_name(e)}(CPU *c);"
                        f"{' /* ' + nm + ' */' if nm else ''}\n")
                if e in self.resume:
                    h.write(f"void {self.func_name(e)}__resume(CPU *c, "
                            f"uint32_t at);\n")
                if e in getattr(self, "wraps", ()):
                    h.write(f"void {self.func_name(e)}__orig(CPU *c);\n")
        for ci, chunk in enumerate(chunks):
            path = os.path.join(outdir, f"{self.prefix.lower()}_{ci:02d}.c")
            with open(path, "w") as o:
                o.write("/* generated by tools/recomp.py - do not edit */\n")
                o.write(f"#include \"{self.prefix.lower()}_funcs.h\"\n\n")
                for e in sorted(chunk):
                    if e in self.overrides:
                        o.write(f"/* {self.sym_comment(e)}: overridden in "
                                f"runtime */\n\n")
                        continue
                    fe = FuncEmitter(self, self.funcs[e])
                    lines = fe.emit(self.resume.get(e, ()))
                    if e in getattr(self, "wraps", ()):
                        nm = self.func_name(e)
                        lines = [ln.replace(f"void {nm}(CPU *c)", f"void {nm}__orig(CPU *c)")
                                   .replace(f"FUNC_ENTER({hx(e)}, {nm});",
                                            f"FUNC_ENTER({hx(e)}, {nm}__orig);")
                                 for ln in lines]
                    o.write("\n".join(lines))
                    o.write("\n")
                    warnings += fe.warnings
        tab = os.path.join(outdir, f"{self.prefix.lower()}_table.c")
        with open(tab, "w") as o:
            o.write("/* generated by tools/recomp.py - do not edit */\n")
            o.write(f"#include \"{self.prefix.lower()}_funcs.h\"\n\n")
            o.write(f"const FuncEntry {self.prefix.lower()}_func_table[] = {{\n")
            for e in entries:
                nm = self.names.get(e, "")
                o.write(f"    {{ {hx(e)}, {self.func_name(e)}, \"{nm}\" }},\n")
            o.write("};\n")
            o.write(f"const unsigned {self.prefix.lower()}_func_count = "
                    f"{len(entries)};\n\n")
            o.write(f"const ResumeEntry {self.prefix.lower()}_resume_table[] = {{\n")
            nres = 0
            for e in sorted(self.resume):
                for r in sorted(self.resume[e]):
                    o.write(f"    {{ {hx(r)}, {self.func_name(e)}__resume }},\n")
                    nres += 1
            o.write("    { 0, 0 }\n};\n")
            o.write(f"const unsigned {self.prefix.lower()}_resume_count = "
                    f"{nres};\n")
        with open(os.path.join(outdir, "warnings.txt"), "w") as o:
            o.write("\n".join(warnings) + "\n")
        print(f"{len(entries)} functions, {len(warnings)} warnings",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--code-end", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--syms", nargs="*", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--overrides", default=None)
    ap.add_argument("--split", type=int, default=32)
    ap.add_argument("--extra-code", nargs="*", default=[],
                    help="additional code ranges START:END (hex)")
    a = ap.parse_args()
    exe = PsxExe(a.exe)
    names = load_symbols(a.syms) if a.syms else {}
    ov = set()
    wraps = set()
    if a.overrides and os.path.exists(a.overrides):
        for line in open(a.overrides):
            line = line.split("#")[0].strip()
            if line:
                parts = line.split()
                addr = N(int(parts[0], 16))
                # "ADDR wrap": keep the recompiled body as NAME__orig so a
                # native NAME can wrap it; otherwise native replaces it
                if len(parts) > 1 and parts[1] == "wrap":
                    wraps.add(addr)
                else:
                    ov.add(addr)
    extra = []
    for r in a.extra_code:
        lo, hi = r.split(":")
        extra.append((N(int(lo, 16)), N(int(hi, 16))))
    rc = Recompiler(exe, int(a.code_end, 16), a.prefix, names, ov, extra)
    rc.wraps = wraps
    rc.generate(a.out, a.split)


if __name__ == "__main__":
    main()
