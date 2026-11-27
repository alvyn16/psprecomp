/* psprecomp — Allegrex interpreter. See include/psprecomp/interp.h. */

#include "psprecomp/interp.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include "decode.h"
#include "psprecomp/hle.h"
#include "psprecomp/recomp_rt.h"
#include "psprecomp/vfpu.h"

#include <stdio.h>

static uint32_t reg(uint8_t n) { return n ? psp_cpu.r[n] : 0u; }

/* Import stubs the interpreter can hand to the HLE layer. A game module names
 * these addresses in its import table; the emitter uses the same table to
 * generate thunks, so both paths end up in the same firmware handler. */
#define MAX_HLE 64
static struct { uint32_t addr, nid; } g_hle[MAX_HLE];
static int g_nhle;

void psp_interp_hle_add(uint32_t addr, uint32_t nid) {
    for (int i = 0; i < g_nhle; i++)
        if (g_hle[i].addr == addr) { g_hle[i].nid = nid; return; }
    if (g_nhle < MAX_HLE) {
        g_hle[g_nhle].addr = addr;
        g_hle[g_nhle].nid = nid;
        g_nhle++;
    }
}

void psp_interp_hle_clear(void) { g_nhle = 0; }

static int hle_at(uint32_t addr, uint32_t *nid) {
    for (int i = 0; i < g_nhle; i++)
        if (g_hle[i].addr == addr) { *nid = g_hle[i].nid; return 1; }
    return 0;
}
static void setr(uint8_t n, uint32_t v) { psp_set_reg(n, v); }

/* One non-control instruction. Control flow is handled by psp_interp_step so
 * the delay slot can be run in the right order for every transfer form. */
static int exec(const a_insn *in) {
    const uint32_t a = in->rs, b = in->rt;
    uint32_t v;
    int64_t q;

    switch (in->op) {
    case A_NOP: case A_SYNC: return 1;

    /* Integer ALU. ADD/SUB ignore overflow here: the recompiled path uses the
     * same unsigned 32-bit arithmetic for the common no-trap case. */
    case A_ADD: case A_ADDU: v = reg(in->rs) + reg(in->rt); setr(in->rd, v); return 1;
    case A_SUB: case A_SUBU: v = reg(in->rs) - reg(in->rt); setr(in->rd, v); return 1;
    case A_AND:  v = reg(in->rs) &  reg(in->rt); setr(in->rd, v); return 1;
    case A_OR:   v = reg(in->rs) |  reg(in->rt); setr(in->rd, v); return 1;
    case A_XOR:  v = reg(in->rs) ^  reg(in->rt); setr(in->rd, v); return 1;
    case A_NOR:  v = ~(reg(in->rs) | reg(in->rt)); setr(in->rd, v); return 1;
    case A_SLT:  v = (int32_t)reg(in->rs) < (int32_t)reg(in->rt); setr(in->rd, v); return 1;
    case A_SLTU: v = reg(in->rs) < reg(in->rt); setr(in->rd, v); return 1;
    case A_MAX:  v = reg(in->rs) > reg(in->rt) ? reg(in->rs) : reg(in->rt); setr(in->rd, v); return 1;
    case A_MIN:  v = reg(in->rs) < reg(in->rt) ? reg(in->rs) : reg(in->rt); setr(in->rd, v); return 1;

    case A_ADDI: case A_ADDIU: v = reg(in->rs) + (uint32_t)in->imm; setr(in->rt, v); return 1;
    case A_SLTI:  v = (int32_t)reg(in->rs) < in->imm; setr(in->rt, v); return 1;
    case A_SLTIU: v = reg(in->rs) < (uint32_t)in->imm; setr(in->rt, v); return 1;
    case A_ANDI:  v = reg(in->rs) &  (uint32_t)in->imm; setr(in->rt, v); return 1;
    case A_ORI:   v = reg(in->rs) |  (uint32_t)in->imm; setr(in->rt, v); return 1;
    case A_XORI:  v = reg(in->rs) ^  (uint32_t)in->imm; setr(in->rt, v); return 1;
    case A_LUI:   v = (uint32_t)in->imm << 16; setr(in->rt, v); return 1;

    case A_SLL:  v = reg(in->rt) << in->sa; setr(in->rd, v); return 1;
    case A_SRL:  v = reg(in->rt) >> in->sa; setr(in->rd, v); return 1;
    case A_SRA:  v = (uint32_t)((int32_t)reg(in->rt) >> in->sa); setr(in->rd, v); return 1;
    case A_SLLV: v = reg(in->rt) << (reg(in->rs) & 31); setr(in->rd, v); return 1;
    case A_SRLV: v = reg(in->rt) >> (reg(in->rs) & 31); setr(in->rd, v); return 1;
    case A_SRAV: v = (uint32_t)((int32_t)reg(in->rt) >> (reg(in->rs) & 31)); setr(in->rd, v); return 1;

    case A_MFHI: setr(in->rd, psp_cpu.hi); return 1;
    case A_MFLO: setr(in->rd, psp_cpu.lo); return 1;
    case A_MTHI: psp_cpu.hi = reg(in->rs); return 1;
    case A_MTLO: psp_cpu.lo = reg(in->rs); return 1;
    case A_MOVZ: if (!reg(in->rt)) setr(in->rd, reg(in->rs)); return 1;
    case A_MOVN: if ( reg(in->rt)) setr(in->rd, reg(in->rs)); return 1;

    case A_MULT:  q = (int64_t)(int32_t)reg(in->rs) * (int32_t)reg(in->rt);
                  psp_cpu.lo = (uint32_t)q; psp_cpu.hi = (uint32_t)(q >> 32); return 1;
    case A_MULTU: q = (uint64_t)reg(in->rs) * reg(in->rt);
                  psp_cpu.lo = (uint32_t)q; psp_cpu.hi = (uint32_t)(q >> 32); return 1;
    case A_DIV:
        if (!reg(in->rt)) return 0;
        psp_cpu.lo = (uint32_t)((int32_t)reg(in->rs) / (int32_t)reg(in->rt));
        psp_cpu.hi = (uint32_t)((int32_t)reg(in->rs) % (int32_t)reg(in->rt));
        return 1;
    case A_DIVU:
        if (!reg(in->rt)) return 0;
        psp_cpu.lo = reg(in->rs) / reg(in->rt);
        psp_cpu.hi = reg(in->rs) % reg(in->rt);
        return 1;

    /* Loads and stores use the runtime memory map, not raw host pointers. */
    case A_LB:  setr(in->rt, (uint32_t)(int32_t)(int8_t)psp_read8 (reg(in->rs) + (uint32_t)in->imm)); return 1;
    case A_LBU: setr(in->rt, psp_read8 (reg(in->rs) + (uint32_t)in->imm)); return 1;
    case A_LH:  setr(in->rt, (uint32_t)(int32_t)(int16_t)psp_read16(reg(in->rs) + (uint32_t)in->imm)); return 1;
    case A_LHU: setr(in->rt, psp_read16(reg(in->rs) + (uint32_t)in->imm)); return 1;
    case A_LW:  setr(in->rt, psp_read32(reg(in->rs) + (uint32_t)in->imm)); return 1;
    case A_SB:  psp_write8 (reg(in->rs) + (uint32_t)in->imm, (uint8_t)reg(in->rt)); return 1;
    case A_SH:  psp_write16(reg(in->rs) + (uint32_t)in->imm, (uint16_t)reg(in->rt)); return 1;
    case A_SW:  psp_write32(reg(in->rs) + (uint32_t)in->imm, reg(in->rt)); return 1;

    case A_BITREV: {
        uint32_t x = reg(in->rs), y = 0;
        for (int i = 0; i < 32; i++) if (x & (1u << i)) y |= 1u << (31 - i);
        setr(in->rd, y); return 1;
    }
    case A_SEB: setr(in->rd, (uint32_t)(int32_t)(int8_t)reg(in->rs)); return 1;
    case A_SEH: setr(in->rd, (uint32_t)(int32_t)(int16_t)reg(in->rs)); return 1;
    case A_WSBH: {
        uint32_t x = reg(in->rs);
        setr(in->rd, ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu)); return 1;
    }
    case A_WSBW: {
        uint32_t x = reg(in->rs);
        setr(in->rd, ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
                     ((x >> 8) & 0x0000FF00u) | ((x >> 24) & 0x000000FFu)); return 1;
    }
    case A_EXT: {
        /* F_RT_RS_POS_SZ: sa is the position, rd holds the msb (size-1). */
        const unsigned pos = in->sa, size = in->rd - in->sa + 1;
        const uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1u);
        setr(in->rt, (reg(in->rs) >> pos) & mask); return 1;
    }
    /* COP1: the emitted C uses these exact helpers and operations, so the
     * oracle can share their semantics rather than duplicate float rules. */
    case A_MFC1: setr(in->rt, psp_f32_to_bits(psp_cpu.f[in->fs])); return 1;
    case A_MTC1: psp_cpu.f[in->fs] = psp_bits_to_f32(reg(in->rt)); return 1;
    case A_CFC1: setr(in->rt, psp_cpu.fcr31); return 1;
    case A_CTC1: psp_cpu.fcr31 = reg(in->rt); return 1;
    case A_LWC1: psp_cpu.f[in->ft] = psp_read_f32(reg(in->rs) + (uint32_t)in->imm); return 1;
    case A_SWC1: psp_write_f32(reg(in->rs) + (uint32_t)in->imm, psp_cpu.f[in->ft]); return 1;
    case A_ADD_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] + psp_cpu.f[in->ft]; return 1;
    case A_SUB_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] - psp_cpu.f[in->ft]; return 1;
    case A_MUL_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] * psp_cpu.f[in->ft]; return 1;
    case A_DIV_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs] / psp_cpu.f[in->ft]; return 1;
    case A_MOV_S: psp_cpu.f[in->fd] = psp_cpu.f[in->fs]; return 1;
    case A_NEG_S: psp_cpu.f[in->fd] = -psp_cpu.f[in->fs]; return 1;
    case A_ABS_S: psp_cpu.f[in->fd] = psp_fabs(psp_cpu.f[in->fs]); return 1;
    case A_SQRT_S: psp_cpu.f[in->fd] = psp_fsqrt(psp_cpu.f[in->fs]); return 1;
    case A_CVT_S_W: case A_ROUND_W_S: case A_TRUNC_W_S:
    case A_CEIL_W_S: case A_FLOOR_W_S:
        psp_cpu.f[in->fd] = psp_bits_to_f32((uint32_t)(int32_t)psp_cpu.f[in->fs]); return 1;
    case A_CVT_W_S: psp_cpu.f[in->fd] = (float)(int32_t)psp_f32_to_bits(psp_cpu.f[in->fs]); return 1;
    case A_C_COND_S:
        psp_fpu_set_cond(psp_fcmp(in->fcond, psp_cpu.f[in->fs], psp_cpu.f[in->ft]));
        return 1;

    /* VFPU: the runtime owns the arithmetic, just as it does for emitted C.
     * The oracle drives it from the decoded fields, including any prefixes
     * left pending by vpfxs/vpfxt/vpfxd. */
    case A_VPFXS: psp_vfpu_set_prefix(0, in->raw & 0xFFFFFFu); return 1;
    case A_VPFXT: psp_vfpu_set_prefix(1, in->raw & 0xFFFFFFu); return 1;
    case A_VPFXD: psp_vfpu_set_prefix(2, in->raw & 0xFFFFFFu); return 1;
    case A_VADD: psp_vadd(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VSUB: psp_vsub(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VMUL: psp_vmul(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VDIV: psp_vdiv(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VMIN: psp_vmin(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VMAX: psp_vmax(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VDOT: psp_vdot(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VSCL: psp_vscl(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VHDP: psp_vhdp(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VCRS: psp_vcrs(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VDET: psp_vdet(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VCMP: psp_vcmp(in->vd & 0xF, in->vs, in->vt, in->vsize); return 1;
    case A_VSGE: psp_vsge(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VSLT: psp_vslt(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VSCMP: psp_vscmp(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VMOV: psp_vunary(PSP_VU_MOV, in->vd, in->vs, in->vsize); return 1;
    case A_VABS: psp_vunary(PSP_VU_ABS, in->vd, in->vs, in->vsize); return 1;
    case A_VNEG: psp_vunary(PSP_VU_NEG, in->vd, in->vs, in->vsize); return 1;
    case A_VZERO: psp_vunary(PSP_VU_ZERO, in->vd, in->vs, in->vsize); return 1;
    case A_VONE: psp_vunary(PSP_VU_ONE, in->vd, in->vs, in->vsize); return 1;
    case A_VRCP: psp_vunary(PSP_VU_RCP, in->vd, in->vs, in->vsize); return 1;
    case A_VRSQ: psp_vunary(PSP_VU_RSQ, in->vd, in->vs, in->vsize); return 1;
    case A_VSQRT: psp_vunary(PSP_VU_SQRT, in->vd, in->vs, in->vsize); return 1;
    case A_VSIN: psp_vunary(PSP_VU_SIN, in->vd, in->vs, in->vsize); return 1;
    case A_VCOS: psp_vunary(PSP_VU_COS, in->vd, in->vs, in->vsize); return 1;
    case A_VEXP2: psp_vunary(PSP_VU_EXP2, in->vd, in->vs, in->vsize); return 1;
    case A_VLOG2: psp_vunary(PSP_VU_LOG2, in->vd, in->vs, in->vsize); return 1;
    case A_VI2F: psp_vunary(PSP_VU_I2F, in->vd, in->vs, in->vsize); return 1;
    case A_VCST: psp_vcst(in->vd, in->vs, in->vsize); return 1;
    case A_VMMUL: psp_vmmul(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VTFM2: psp_vtfm(in->vd, in->vs, in->vt, 2); return 1;
    case A_VTFM3: psp_vtfm(in->vd, in->vs, in->vt, 3); return 1;
    case A_VTFM4: psp_vtfm(in->vd, in->vs, in->vt, 4); return 1;
    case A_VMSCL: psp_vmscl(in->vd, in->vs, in->vt, in->vsize); return 1;
    case A_VMMOV: psp_vmmov(in->vd, in->vs, in->vsize); return 1;
    case A_VMZERO: psp_vmzero(in->vd, in->vsize); return 1;
    case A_VMONE: psp_vmone(in->vd, in->vsize); return 1;

    default: break;                 /* unsupported: counted by the caller */
    }

    (void)a; (void)b;
    return 0;   /* unsupported: caller counts the trap */
}

/* Is this transfer taken, and where does it go? `link` receives the address
 * a call must write to $ra (0 when the instruction does not link). */
static int transfer(const a_insn *in, uint32_t *to, uint32_t *link) {
    *link = 0;
    switch (in->op) {
    case A_J:     *to = in->target; return 1;
    case A_JAL:   *to = in->target; *link = in->addr + 8; return 1;
    case A_JR:    *to = reg(in->rs); return 1;
    case A_JALR:  *to = reg(in->rs); *link = in->rd ? in->addr + 8 : 0; return 1;
    case A_BEQ:   *to = in->target; return reg(in->rs) == reg(in->rt);
    case A_BNE:   *to = in->target; return reg(in->rs) != reg(in->rt);
    case A_BLEZ:  *to = in->target; return (int32_t)reg(in->rs) <= 0;
    case A_BGTZ:  *to = in->target; return (int32_t)reg(in->rs) > 0;
    case A_BLTZ: case A_BLTZL: case A_BLTZAL: case A_BLTZALL:
        *to = in->target;
        if (in->op == A_BLTZAL || in->op == A_BLTZALL) *link = in->addr + 8;
        return (int32_t)reg(in->rs) < 0;
    case A_BGEZ: case A_BGEZL: case A_BGEZAL: case A_BGEZALL:
        *to = in->target;
        if (in->op == A_BGEZAL || in->op == A_BGEZALL) *link = in->addr + 8;
        return (int32_t)reg(in->rs) >= 0;
    case A_BEQL:  *to = in->target; return reg(in->rs) == reg(in->rt);
    case A_BNEL:  *to = in->target; return reg(in->rs) != reg(in->rt);
    case A_BLEZL: *to = in->target; return (int32_t)reg(in->rs) <= 0;
    case A_BGTZL: *to = in->target; return (int32_t)reg(in->rs) > 0;
    default: return -1;   /* not a transfer */
    }
}

void psp_interp_init(psp_interp *ip, uint32_t entry) {
    ip->pc = entry;
    ip->steps = ip->traps = 0;
    ip->stopped = 0;
}

int psp_interp_step(psp_interp *ip) {
    if (ip->stopped) return 0;
    a_insn in;
    if (!a_decode(psp_read32(ip->pc), ip->pc, &in)) {
        ip->traps++; ip->stopped = 1; return 0;
    }
    psp_cpu.pc = ip->pc;
    ip->steps++;

    uint32_t to = 0, link = 0;
    const int take = transfer(&in, &to, &link);
    if (take < 0) {
        if (!exec(&in)) { ip->traps++; ip->stopped = 1; return 0; }
        ip->pc += 4;
        return 1;
    }

    /* A taken branch writes $ra before the slot runs; MIPS does it in the
     * branch itself, not after the delay instruction. */
    if (link) setr(PSP_RA_INDEX, link);

    if (in.has_delay_slot) {
        a_insn slot;
        if (!a_decode(psp_read32(ip->pc + 4), ip->pc + 4, &slot) ||
            transfer(&slot, &(uint32_t){0}, &(uint32_t){0}) >= 0 ||
            !exec(&slot)) {
            /* A control transfer in a delay slot is unpredictable on the
             * hardware and outside the oracle's modelled subset. */
            ip->traps++; ip->stopped = 1; return 0;
        }
    }
    /* A call into a registered import stub runs the same HLE handler the
     * emitted thunk would, then resumes at the link address. */
    uint32_t nid;
    if (in.is_call && hle_at(to, &nid)) {
        psp_hle_call(nid);
        ip->pc = link ? link : ip->pc + 8u;
        return 1;
    }
    /* Likely branches nullify the slot when not taken. A `jr $ra` is a guest
     * return: there is nothing below the entry to fall into, so stop here. */
    /* The slot has already been stepped (or nullified for a likely branch),
     * so fall-through always resumes two words on. */
    ip->pc = take ? to : ip->pc + (in.has_delay_slot ? 8u : 4u);
    if (in.op == A_JR && in.rs == PSP_RA_INDEX) ip->stopped = 1;
    return 1;
}

uint64_t psp_interp_run(psp_interp *ip, uint64_t max_steps, uint32_t stop_pc) {
    uint64_t n = 0;
    while (n < max_steps && !ip->stopped) {
        if (stop_pc && ip->pc == stop_pc) break;
        if (!psp_interp_step(ip)) break;
        n++;
    }
    return n;
}
