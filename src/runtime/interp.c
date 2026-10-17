/*
 * MIPS I interpreter with branch- and load-delay semantics.
 *
 * Used (1) to run all of the game's code in interpreted builds, which
 * contain no game code (g_interp_only), (2) as a fallback when control
 * reaches an address without a recompiled function and (3) by the lockstep
 * verifier, which runs a function both ways and compares the outcome.
 *
 * Like the recompiled code it calls the native overrides (native.c) when
 * control reaches their addresses, and feeds precision geometry (PGXP) the
 * same register/memory moves of projected vertices.
 */
#include "psx.h"

/* When true, jal/jalr targets that have recompiled code are executed
 * natively; when false everything is interpreted (verification). */
bool g_interp_call_native = true;
/* Optional per-block hook for trace comparison. */
void (*g_interp_block_hook)(uint32_t pc);

static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

typedef struct {
    int reg;
    uint32_t val;
} PendingLoad;

void interp_run(CPU *c, uint32_t pc, uint32_t stop_ra) {
    PendingLoad pend = {0, 0};
    uint32_t next_pc = pc + 4;
    bool in_delay = false;
    uint32_t branch_target = 0;
    bool branch_taken = false;
    uint64_t steps = 0;

    for (;;) {
        if (pc == stop_ra && !in_delay)
            break;
        uint32_t p = pc & 0x1FFFFFFFu;
        if (p >= 0x00200000 && !(p >= 0x1FC00000))
            fatal("interpreter: pc out of RAM: %08X", pc);
        uint32_t w = mem_r32(pc);
        c->pc = pc;
        if (++steps % 64 == 0) {
            c->cycles += (64 * g_cycle_scale) >> 4;
            if (c->cycles >= c->next_event)
                cpu_service_events(c);
        }

        /* apply pending load after this instruction reads operands */
        PendingLoad cur_load = pend;
        pend.reg = 0;

        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31,
                 rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
        uint32_t imm = w & 0xFFFF, simm = sx16(w);
        uint32_t RS = c->r[rs], RT = c->r[rt];
        uint32_t wr_reg = 0, wr_val = 0;
        bool is_branch = false;
        bool take = false;
        uint32_t target = 0;
        bool link = false;
        uint32_t link_reg = 31;

        switch (op) {
        case 0x00:
            switch (fn) {
            case 0x00: wr_reg = rd; wr_val = RT << sa; break;
            case 0x02: wr_reg = rd; wr_val = RT >> sa; break;
            case 0x03: wr_reg = rd; wr_val = (uint32_t)((int32_t)RT >> sa); break;
            case 0x04: wr_reg = rd; wr_val = RT << (RS & 31); break;
            case 0x06: wr_reg = rd; wr_val = RT >> (RS & 31); break;
            case 0x07: wr_reg = rd; wr_val = (uint32_t)((int32_t)RT >> (RS & 31)); break;
            case 0x08: is_branch = true; take = true; target = RS; break;
            case 0x09: is_branch = true; take = true; target = RS; link = true; link_reg = rd; break;
            case 0x0C:
                hle_syscall(c, (w >> 6) & 0xFFFFF);
                break;
            case 0x0D: hle_break(c, pc, (w >> 6) & 0xFFFFF); break;
            case 0x10: wr_reg = rd; wr_val = c->hi; break;
            case 0x11: c->hi = RS; break;
            case 0x12: wr_reg = rd; wr_val = c->lo; break;
            case 0x13: c->lo = RS; break;
            case 0x18: { int64_t m = (int64_t)(int32_t)RS * (int32_t)RT; c->lo = (uint32_t)m; c->hi = (uint32_t)((uint64_t)m >> 32); break; }
            case 0x19: { uint64_t m = (uint64_t)RS * RT; c->lo = (uint32_t)m; c->hi = (uint32_t)(m >> 32); break; }
            case 0x1A: cpu_div(c, RS, RT); break;
            case 0x1B: cpu_divu(c, RS, RT); break;
            case 0x20: case 0x21:
                wr_reg = rd; wr_val = RS + RT;
                if (fn == 0x21 && rd && (rs == 0) != (rt == 0))
                    PGXP_MOV((int)rd, (int)(rs ? rs : rt));
                break;
            case 0x22: case 0x23: wr_reg = rd; wr_val = RS - RT; break;
            case 0x24: wr_reg = rd; wr_val = RS & RT; break;
            case 0x25:
                wr_reg = rd; wr_val = RS | RT;
                if (rd && (rs == 0) != (rt == 0))
                    PGXP_MOV((int)rd, (int)(rs ? rs : rt));
                break;
            case 0x26: wr_reg = rd; wr_val = RS ^ RT; break;
            case 0x27: wr_reg = rd; wr_val = ~(RS | RT); break;
            case 0x2A: wr_reg = rd; wr_val = (int32_t)RS < (int32_t)RT; break;
            case 0x2B: wr_reg = rd; wr_val = RS < RT; break;
            default: hle_invalid(c, pc, w);
            }
            break;
        case 0x01: {
            bool ge = (rt & 1);
            bool cond = ge ? ((int32_t)RS >= 0) : ((int32_t)RS < 0);
            is_branch = true;
            take = cond;
            target = pc + 4 + (simm << 2);
            if ((rt & 0x1E) == 0x10) { link = true; link_reg = 31; }
            break;
        }
        case 0x02: is_branch = true; take = true; target = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); break;
        case 0x03: is_branch = true; take = true; target = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFF) << 2); link = true; break;
        case 0x04: is_branch = true; take = RS == RT; target = pc + 4 + (simm << 2); break;
        case 0x05: is_branch = true; take = RS != RT; target = pc + 4 + (simm << 2); break;
        case 0x06: is_branch = true; take = (int32_t)RS <= 0; target = pc + 4 + (simm << 2); break;
        case 0x07: is_branch = true; take = (int32_t)RS > 0; target = pc + 4 + (simm << 2); break;
        case 0x08: case 0x09:
            wr_reg = rt; wr_val = RS + simm;
            if (imm == 0 && rs && rt)
                PGXP_MOV((int)rt, (int)rs);
            break;
        case 0x0A: wr_reg = rt; wr_val = (int32_t)RS < (int32_t)simm; break;
        case 0x0B: wr_reg = rt; wr_val = RS < simm; break;
        case 0x0C: wr_reg = rt; wr_val = RS & imm; break;
        case 0x0D:
            wr_reg = rt; wr_val = RS | imm;
            if (imm == 0 && rs && rt)
                PGXP_MOV((int)rt, (int)rs);
            break;
        case 0x0E: wr_reg = rt; wr_val = RS ^ imm; break;
        case 0x0F: wr_reg = rt; wr_val = imm << 16; break;
        case 0x10:
            if (rs == 0) { pend.reg = (int)rt; pend.val = cop0_read(c, (int)rd); }
            else if (rs == 4) cop0_write(c, (int)rd, RT);
            else if (rs == 0x10 && fn == 0x10) cop0_rfe(c);
            break;
        case 0x12:
            if (rs & 0x10) gte_command(c, w & 0x1FFFFFF);
            else if (rs == 0) {
                pend.reg = (int)rt; pend.val = gte_read_data(c, (int)rd);
                if (rd >= 12 && rd <= 15 && rt)
                    PGXP_MFC2((int)rt, (int)rd);
            }
            else if (rs == 2) { pend.reg = (int)rt; pend.val = gte_read_ctrl(c, (int)rd); }
            else if (rs == 4) {
                gte_write_data(c, (int)rd, RT);
                if (rd >= 12 && rd <= 15)
                    PGXP_MTC2((int)rd, (int)rt);
            }
            else if (rs == 6) gte_write_ctrl(c, (int)rd, RT);
            break;
        case 0x20: pend.reg = (int)rt; pend.val = (uint32_t)(int32_t)(int8_t)mem_r8(RS + simm); break;
        case 0x21: pend.reg = (int)rt; pend.val = sx16(mem_r16(RS + simm)); break;
        case 0x22: { uint32_t base = (cur_load.reg == (int)rt) ? cur_load.val : RT; pend.reg = (int)rt; pend.val = mem_lwl(base, RS + simm); break; }
        case 0x23:
            pend.reg = (int)rt; pend.val = mem_r32(RS + simm);
            if (rt)
                PGXP_LW((int)rt, RS + simm);
            break;
        case 0x24: pend.reg = (int)rt; pend.val = mem_r8(RS + simm); break;
        case 0x25: pend.reg = (int)rt; pend.val = mem_r16(RS + simm); break;
        case 0x26: { uint32_t base = (cur_load.reg == (int)rt) ? cur_load.val : RT; pend.reg = (int)rt; pend.val = mem_lwr(base, RS + simm); break; }
        case 0x28: mem_w8(RS + simm, RT); break;
        case 0x29: mem_w16(RS + simm, RT); break;
        case 0x2A: mem_swl(RS + simm, RT); break;
        case 0x2B:
            mem_w32(RS + simm, RT);
            PGXP_SW(RS + simm, (int)rt);
            break;
        case 0x2E: mem_swr(RS + simm, RT); break;
        case 0x32:
            gte_write_data(c, (int)rt, mem_r32(RS + simm));
            if (rt >= 12 && rt <= 15)
                PGXP_LWC2((int)rt, RS + simm);
            break;
        case 0x3A:
            mem_w32(RS + simm, gte_read_data(c, (int)rt));
            if (rt >= 12 && rt <= 15)
                PGXP_SWC2(RS + simm, (int)rt);
            break;
        default: hle_invalid(c, pc, w);
        }

        /* retire the previous load (unless this instruction overwrote it) */
        if (cur_load.reg && cur_load.reg != (int)wr_reg)
            c->r[cur_load.reg] = cur_load.val;
        if (wr_reg)
            c->r[wr_reg] = wr_val;
        if (pend.reg == 0)
            pend.val = 0;
        c->r[0] = 0;

        if (in_delay) {
            in_delay = false;
            if (branch_taken) {
                pc = branch_target;
                next_pc = pc + 4;
                if (g_interp_block_hook)
                    g_interp_block_hook(pc);
                continue;
            }
            pc = next_pc;
            next_pc = pc + 4;
            continue;
        }
        if (is_branch) {
            if (link)
                c->r[link_reg] = pc + 8;
            uint32_t tp = target & 0x1FFFFFFFu;
            if (take && (tp == 0xA0 || tp == 0xB0 || tp == 0xC0)) {
                /* BIOS vector: run the delay slot (it sets t1), then the
                 * HLE call; continue at ra (jump) or after the call */
                uint32_t ret = pc + 8;
                interp_run(c, pc + 4, pc + 8);
                if (tp == 0xA0) bios_call_A0(c);
                else if (tp == 0xB0) bios_call_B0(c);
                else bios_call_C0(c);
                pc = link ? ret : c->r[31];
                next_pc = pc + 4;
                continue;
            }
            if (take && g_interp_only) {
                RecompFunc ov = native_lookup(target);
                if (ov) {
                    /* native override: run the delay slot, then the native
                     * function as a call (jal/jalr) or a tail call (j/jr,
                     * returning to the current ra) */
                    uint32_t ret = pc + 8;
                    interp_run(c, pc + 4, pc + 8);
                    if (link)
                        c->r[link_reg] = ret;
                    uint32_t back = link ? ret : c->r[31];
                    ov(c);
                    pc = back;
                    next_pc = pc + 4;
                    if (pend.reg) {
                        c->r[pend.reg] = pend.val;
                        pend.reg = 0;
                    }
                    continue;
                }
            }
            if (take && link && g_interp_call_native && !g_interp_only &&
                module_lookup(target)) {
                /* run delay slot first */
                uint32_t ds = pc + 4;
                uint32_t ret = pc + 8;
                interp_run(c, ds, ds + 4);
                c->r[link_reg] = ret;
                call_indirect(c, target);
                pc = ret;
                next_pc = pc + 4;
                if (pend.reg) {
                    c->r[pend.reg] = pend.val;
                    pend.reg = 0;
                }
                continue;
            }
            in_delay = true;
            branch_taken = take;
            branch_target = target;
            pc += 4;
            next_pc = pc + 4;
            continue;
        }
        pc = next_pc;
        next_pc = pc + 4;
    }
    if (pend.reg)
        c->r[pend.reg] = pend.val;
    c->r[0] = 0;
}

void interp_call(CPU *c, uint32_t addr) {
    interp_run(c, addr, c->r[31]);
}
