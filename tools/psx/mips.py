"""MIPS I (R3000A + PS1 GTE) instruction decoder and disassembler.

decode(word, pc) returns an Insn with structured fields used by both the
disassembler and the static recompiler.
"""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

GTE_CMDS = {
    0x01: "RTPS", 0x06: "NCLIP", 0x0C: "OP", 0x10: "DPCS", 0x11: "INTPL",
    0x12: "MVMVA", 0x13: "NCDS", 0x14: "CDP", 0x16: "NCDT", 0x1B: "NCCS",
    0x1C: "CC", 0x1E: "NCS", 0x20: "NCT", 0x28: "SQR", 0x29: "DCPL",
    0x2A: "DPCT", 0x2D: "AVSZ3", 0x2E: "AVSZ4", 0x30: "RTPT", 0x3D: "GPF",
    0x3E: "GPL", 0x3F: "NCCT",
}


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


class Insn:
    __slots__ = ("pc", "word", "op", "rs", "rt", "rd", "sa", "imm", "simm",
                 "target", "mnem", "kind", "cop")

    def __init__(self, pc, word):
        self.pc = pc
        self.word = word
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.imm = word & 0xFFFF
        self.simm = s16(word)
        self.target = None
        self.mnem = "???"
        # kind: alu, load, store, branch, jump, jal, jr, jalr, syscall,
        # break, cop0, cop2, gte, lwc2, swc2, mult, invalid
        self.kind = "invalid"
        self.cop = None

    @property
    def is_branch(self):
        return self.kind in ("branch", "jump", "jal", "jr", "jalr")

    @property
    def has_delay(self):
        return self.is_branch

    def __repr__(self):
        return f"{self.pc:08x}: {disasm(self)}"


SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv",
    0x07: "srav", 0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x18: "mult",
    0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu",
    0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor",
    0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}

IMM_OPS = {
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
    0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
}
LOAD_OPS = {0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
            0x25: "lhu", 0x26: "lwr"}
STORE_OPS = {0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr"}
BRANCH_OPS = {0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz"}


def decode(word, pc):
    i = Insn(pc, word)
    op = i.op
    if op == 0:
        fn = word & 0x3F
        m = SPECIAL.get(fn)
        if m is None:
            return i
        i.mnem = m
        if m == "jr":
            i.kind = "jr"
        elif m == "jalr":
            i.kind = "jalr"
        elif m == "syscall":
            i.kind = "syscall"
        elif m == "break":
            i.kind = "break"
        elif m in ("mult", "multu", "div", "divu"):
            i.kind = "mult"
        else:
            i.kind = "alu"
    elif op == 1:
        rt = i.rt
        names = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}
        # R3000 decodes bits 16 and 20 only
        m = names.get(rt & 0x11)
        i.mnem = m
        i.kind = "branch"
        i.target = (pc + 4 + (i.simm << 2)) & 0xFFFFFFFF
    elif op in (2, 3):
        i.mnem = "j" if op == 2 else "jal"
        i.kind = "jump" if op == 2 else "jal"
        i.target = ((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
    elif op in BRANCH_OPS:
        i.mnem = BRANCH_OPS[op]
        i.kind = "branch"
        i.target = (pc + 4 + (i.simm << 2)) & 0xFFFFFFFF
    elif op in IMM_OPS:
        i.mnem = IMM_OPS[op]
        i.kind = "alu"
    elif op in LOAD_OPS:
        i.mnem = LOAD_OPS[op]
        i.kind = "load"
    elif op in STORE_OPS:
        i.mnem = STORE_OPS[op]
        i.kind = "store"
    elif op == 0x10:  # COP0
        sub = i.rs
        if sub == 0:
            i.mnem, i.kind = "mfc0", "cop0"
        elif sub == 4:
            i.mnem, i.kind = "mtc0", "cop0"
        elif sub == 0x10 and (word & 0x3F) == 0x10:
            i.mnem, i.kind = "rfe", "cop0"
    elif op == 0x12:  # COP2 / GTE
        sub = i.rs
        if sub & 0x10:
            cmd = word & 0x3F
            i.mnem = GTE_CMDS.get(cmd, f"gte_{cmd:02x}")
            i.kind = "gte"
        elif sub == 0:
            i.mnem, i.kind = "mfc2", "cop2"
        elif sub == 2:
            i.mnem, i.kind = "cfc2", "cop2"
        elif sub == 4:
            i.mnem, i.kind = "mtc2", "cop2"
        elif sub == 6:
            i.mnem, i.kind = "ctc2", "cop2"
    elif op == 0x32:
        i.mnem, i.kind = "lwc2", "lwc2"
    elif op == 0x3A:
        i.mnem, i.kind = "swc2", "swc2"
    return i


def disasm(i, names=None):
    def sym(a):
        if names and a in names:
            return names[a]
        return f"0x{a:08x}"
    R = REG
    m = i.mnem
    k = i.kind
    if k == "invalid":
        return f".word 0x{i.word:08x}"
    if m == "sll" and i.word == 0:
        return "nop"
    if k == "alu" and i.op == 0:
        fn = i.word & 0x3F
        if fn in (0, 2, 3):
            return f"{m} {R[i.rd]}, {R[i.rt]}, {i.sa}"
        if fn in (4, 6, 7):
            return f"{m} {R[i.rd]}, {R[i.rt]}, {R[i.rs]}"
        if m in ("mfhi", "mflo"):
            return f"{m} {R[i.rd]}"
        if m in ("mthi", "mtlo"):
            return f"{m} {R[i.rs]}"
        if m == "addu" and i.rt == 0:
            return f"move {R[i.rd]}, {R[i.rs]}"
        if m == "or" and i.rt == 0:
            return f"move {R[i.rd]}, {R[i.rs]}"
        return f"{m} {R[i.rd]}, {R[i.rs]}, {R[i.rt]}"
    if k == "mult":
        return f"{m} {R[i.rs]}, {R[i.rt]}"
    if k == "jr":
        return f"jr {R[i.rs]}"
    if k == "jalr":
        return f"jalr {R[i.rd]}, {R[i.rs]}"
    if k in ("syscall", "break"):
        return f"{m} 0x{(i.word >> 6) & 0xFFFFF:x}"
    if k == "alu":
        if m == "lui":
            return f"lui {R[i.rt]}, 0x{i.imm:04x}"
        if m in ("andi", "ori", "xori"):
            return f"{m} {R[i.rt]}, {R[i.rs]}, 0x{i.imm:04x}"
        if m == "addiu" and i.rs == 0:
            return f"li {R[i.rt]}, {i.simm}"
        return f"{m} {R[i.rt]}, {R[i.rs]}, {i.simm}"
    if k in ("load", "store"):
        return f"{m} {R[i.rt]}, {i.simm}({R[i.rs]})"
    if k in ("lwc2", "swc2"):
        return f"{m} ${i.rt}, {i.simm}({R[i.rs]})"
    if k == "branch":
        if m in ("beq", "bne"):
            if m == "beq" and i.rs == 0 and i.rt == 0:
                return f"b {sym(i.target)}"
            return f"{m} {R[i.rs]}, {R[i.rt]}, {sym(i.target)}"
        return f"{m} {R[i.rs]}, {sym(i.target)}"
    if k in ("jump", "jal"):
        return f"{m} {sym(i.target)}"
    if k == "cop0":
        if m == "rfe":
            return "rfe"
        return f"{m} {R[i.rt]}, $cop0_{i.rd}"
    if k == "cop2":
        return f"{m} {R[i.rt]}, ${i.rd}"
    if k == "gte":
        return f"{m} 0x{i.word & 0x1FFFFFF:07x}"
    return m


def reads_writes(i):
    """Return (set of GPRs read, set of GPRs written) excluding r0."""
    rd_, wr = set(), set()
    k, m = i.kind, i.mnem
    if k == "alu":
        if i.op == 0:
            fn = i.word & 0x3F
            if fn in (0, 2, 3):
                rd_ = {i.rt}
            elif m in ("mfhi", "mflo"):
                rd_ = set()
            elif m in ("mthi", "mtlo"):
                rd_ = {i.rs}
            else:
                rd_ = {i.rs, i.rt}
            if m not in ("mthi", "mtlo"):
                wr = {i.rd}
        else:
            rd_ = set() if m == "lui" else {i.rs}
            wr = {i.rt}
    elif k == "mult":
        rd_ = {i.rs, i.rt}
    elif k == "load":
        rd_ = {i.rs}
        if m in ("lwl", "lwr"):
            rd_.add(i.rt)
        wr = {i.rt}
    elif k == "store":
        rd_ = {i.rs, i.rt}
    elif k in ("lwc2", "swc2"):
        rd_ = {i.rs}
    elif k == "branch":
        rd_ = {i.rs} if m not in ("beq", "bne") else {i.rs, i.rt}
        if m in ("bltzal", "bgezal"):
            wr = {31}
    elif k == "jal":
        wr = {31}
    elif k == "jr":
        rd_ = {i.rs}
    elif k == "jalr":
        rd_ = {i.rs}
        wr = {i.rd}
    elif k == "cop0" or k == "cop2":
        if m in ("mfc0", "mfc2", "cfc2"):
            wr = {i.rt}
        elif m in ("mtc0", "mtc2", "ctc2"):
            rd_ = {i.rt}
    rd_.discard(0)
    wr.discard(0)
    return rd_, wr
