/*
 * P4OS - Emulador, Sega 8-bit core: a Z80 interpreter (see z80.h).
 *
 * Copyright (C) 2026 P4OS. This program is free software; you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Written for the Master System, Game Gear and ColecoVision: the documented
 * instruction set with the undocumented parts games and test programs rely
 * on (the X and Y flags, IXH/IXL, SLL, the DD CB register copies, the ED
 * mirrors, MEMPTR for BIT n,(HL), the flags of the block instructions).
 * Timing is per instruction in T-states, without wait states (the consoles
 * have none). It passes ZEXDOC and ZEXALL (see the core's README).
 *
 * One instruction is one trip through a switch; the registers live in the
 * Z80 global so that the rest of SMS Plus can look at them (the PC for
 * unmapped reads, the state files).
 */
#include <string.h>

#include "z80.h"

Z80_Regs Z80;
int z80_cycle_count;
unsigned char *cpu_readmap[64];
unsigned char *cpu_writemap[64];

/* T-states left of the z80_execute() in progress, and how many it was
 * asked for: z80_get_elapsed_cycles() works them out mid-instruction (the
 * VDP's counters are read from inside an IN). */
static int icount;
static int requested;
static int in_exec;

#define CF 0x01
#define NF 0x02
#define PF 0x04
#define VF PF
#define XF 0x08
#define HF 0x10
#define YF 0x20
#define ZF 0x40
#define SF 0x80

/* Flags of an 8-bit result: S, Z and the copies of bits 5 and 3; with P
 * the parity; after INC and DEC. Built once by z80_init(). */
static uint8_t SZ[256], SZP[256], SZHV_inc[256], SZHV_dec[256];

#define PC   Z80.pc.w.l
#define SP   Z80.sp.w.l
#define AF   Z80.af.w.l
#define BC   Z80.bc.w.l
#define DE   Z80.de.w.l
#define HL   Z80.hl.w.l
#define IX   Z80.ix.w.l
#define IY   Z80.iy.w.l
#define WZ   Z80.wz.w.l
#define A    Z80.af.b.h
#define F    Z80.af.b.l
#define B    Z80.bc.b.h
#define C    Z80.bc.b.l
#define D    Z80.de.b.h
#define E    Z80.de.b.l
#define H    Z80.hl.b.h
#define L    Z80.hl.b.l

/* ---- memory and ports ---------------------------------------------------- */

static inline uint8_t RM(uint16_t a)
{
    return cpu_readmap[a >> 10][a & 0x3FF];
}

static inline void WM(uint16_t a, uint8_t v)
{
    Z80.mem_write(a, v);
}

static inline uint16_t RM16(uint16_t a)
{
    return (uint16_t)(RM(a) | (RM((uint16_t)(a + 1)) << 8));
}

static inline void WM16(uint16_t a, uint16_t v)
{
    WM(a, (uint8_t)v);
    WM((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static inline uint8_t ARG(void)
{
    uint8_t v = RM(PC);
    PC++;
    return v;
}

static inline uint16_t ARG16(void)
{
    uint16_t v = RM16(PC);
    PC += 2;
    return v;
}

/* High byte first, as the chip does it. */
static inline void PUSH(uint16_t v)
{
    SP--;
    WM(SP, (uint8_t)(v >> 8));
    SP--;
    WM(SP, (uint8_t)v);
}

static inline uint16_t POP(void)
{
    uint16_t v = RM16(SP);
    SP += 2;
    return v;
}

#define IN(port)       Z80.port_read(port)
#define OUT(port, v)   Z80.port_write(port, v)

/* ---- arithmetic ---------------------------------------------------------- */

static inline void op_add(uint8_t v)
{
    unsigned r = A + v;
    F = SZ[r & 0xFF] | ((r >> 8) & CF) | ((A ^ r ^ v) & HF)
      | (((v ^ A ^ 0x80) & (v ^ r) & 0x80) >> 5);
    A = (uint8_t)r;
}

static inline void op_adc(uint8_t v)
{
    unsigned r = A + v + (F & CF);
    F = SZ[r & 0xFF] | ((r >> 8) & CF) | ((A ^ r ^ v) & HF)
      | (((v ^ A ^ 0x80) & (v ^ r) & 0x80) >> 5);
    A = (uint8_t)r;
}

static inline void op_sub(uint8_t v)
{
    unsigned r = (unsigned)A - v;
    F = SZ[r & 0xFF] | ((r >> 8) & CF) | NF | ((A ^ r ^ v) & HF)
      | (((v ^ A) & (A ^ r) & 0x80) >> 5);
    A = (uint8_t)r;
}

static inline void op_sbc(uint8_t v)
{
    unsigned r = (unsigned)A - v - (F & CF);
    F = SZ[r & 0xFF] | ((r >> 8) & CF) | NF | ((A ^ r ^ v) & HF)
      | (((v ^ A) & (A ^ r) & 0x80) >> 5);
    A = (uint8_t)r;
}

/* CP takes bits 5 and 3 from the operand, not from the result. */
static inline void op_cp(uint8_t v)
{
    unsigned r = (unsigned)A - v;
    F = (SZ[r & 0xFF] & (SF | ZF)) | (v & (YF | XF)) | ((r >> 8) & CF) | NF
      | ((A ^ r ^ v) & HF) | (((v ^ A) & (A ^ r) & 0x80) >> 5);
}

static inline void op_and(uint8_t v) { A &= v; F = SZP[A] | HF; }
static inline void op_or(uint8_t v)  { A |= v; F = SZP[A]; }
static inline void op_xor(uint8_t v) { A ^= v; F = SZP[A]; }

static inline uint8_t op_inc(uint8_t v)
{
    v++;
    F = (F & CF) | SZHV_inc[v];
    return v;
}

static inline uint8_t op_dec(uint8_t v)
{
    v--;
    F = (F & CF) | SZHV_dec[v];
    return v;
}

static inline uint16_t op_add16(uint16_t a, uint16_t b)
{
    uint32_t r = (uint32_t)a + b;
    WZ = (uint16_t)(a + 1);
    F = (F & (SF | ZF | VF)) | (((a ^ r ^ b) >> 8) & HF) | ((r >> 16) & CF)
      | ((r >> 8) & (YF | XF));
    return (uint16_t)r;
}

static inline void op_adc16(uint16_t b)
{
    uint32_t a = HL;
    uint32_t r = a + b + (F & CF);
    WZ = (uint16_t)(a + 1);
    F = (((a ^ r ^ b) >> 8) & HF) | ((r >> 16) & CF) | ((r >> 8) & (SF | YF | XF))
      | ((r & 0xFFFF) ? 0 : ZF) | (((b ^ a ^ 0x8000) & (b ^ r) & 0x8000) >> 13);
    HL = (uint16_t)r;
}

static inline void op_sbc16(uint16_t b)
{
    uint32_t a = HL;
    uint32_t r = a - b - (F & CF);
    WZ = (uint16_t)(a + 1);
    F = (((a ^ r ^ b) >> 8) & HF) | NF | ((r >> 16) & CF) | ((r >> 8) & (SF | YF | XF))
      | ((r & 0xFFFF) ? 0 : ZF) | (((b ^ a) & (a ^ r) & 0x8000) >> 13);
    HL = (uint16_t)r;
}

static void op_daa(void)
{
    uint8_t a = A, d = 0, c = F & CF;
    if ((F & HF) || (a & 0x0F) > 9) d = 0x06;
    if (c || a > 0x99) {
        d |= 0x60;
        c = CF;
    }
    uint8_t h;
    if (F & NF) {
        h = ((F & HF) && (a & 0x0F) < 6) ? HF : 0;
        A = (uint8_t)(a - d);
    } else {
        h = ((a & 0x0F) > 9) ? HF : 0;
        A = (uint8_t)(a + d);
    }
    F = SZP[A] | c | (F & NF) | h;
}

/* The CB rotations and shifts, by bits 3-5 of the opcode. */
static inline uint8_t op_rot(int kind, uint8_t v)
{
    uint8_t c;
    switch (kind) {
    case 0: c = v >> 7; v = (uint8_t)((v << 1) | c); break;               /* RLC */
    case 1: c = v & 1;  v = (uint8_t)((v >> 1) | (c << 7)); break;        /* RRC */
    case 2: c = v >> 7; v = (uint8_t)((v << 1) | (F & CF)); break;        /* RL  */
    case 3: c = v & 1;  v = (uint8_t)((v >> 1) | ((F & CF) << 7)); break; /* RR  */
    case 4: c = v >> 7; v = (uint8_t)(v << 1); break;                     /* SLA */
    case 5: c = v & 1;  v = (uint8_t)((v >> 1) | (v & 0x80)); break;      /* SRA */
    case 6: c = v >> 7; v = (uint8_t)((v << 1) | 1); break;               /* SLL */
    default: c = v & 1; v = (uint8_t)(v >> 1); break;                     /* SRL */
    }
    F = SZP[v] | c;
    return v;
}

/* BIT n: Z and P when the bit is clear, S when bit 7 is set; bits 5 and 3
 * from 'xy' (the register, or MEMPTR's high byte for a memory operand). */
static inline void op_bit(int n, uint8_t v, uint8_t xy)
{
    uint8_t m = v & (uint8_t)(1u << n);
    F = (F & CF) | HF | (xy & (YF | XF)) | (m ? (m & SF) : (ZF | PF));
}

/* ---- the 8-bit registers by their index in an opcode --------------------- */

static uint8_t *const R8[8] = {
    &Z80.bc.b.h, &Z80.bc.b.l, &Z80.de.b.h, &Z80.de.b.l,
    &Z80.hl.b.h, &Z80.hl.b.l, NULL, &Z80.af.b.h,
};

/* ---- the CB page ---------------------------------------------------------- */

static void exec_cb(void)
{
    uint8_t op = ARG();
    Z80.r++;
    int r = op & 7, n = (op >> 3) & 7;

    if (r == 6) {
        uint8_t v = RM(HL);
        switch (op >> 6) {
        case 0: WM(HL, op_rot(n, v)); icount -= 15; break;
        case 1: op_bit(n, v, Z80.wz.b.h); icount -= 12; break;
        case 2: WM(HL, v & (uint8_t)~(1u << n)); icount -= 15; break;
        default: WM(HL, v | (uint8_t)(1u << n)); icount -= 15; break;
        }
        return;
    }

    uint8_t *p = R8[r];
    switch (op >> 6) {
    case 0: *p = op_rot(n, *p); break;
    case 1: op_bit(n, *p, *p); break;
    case 2: *p &= (uint8_t)~(1u << n); break;
    default: *p |= (uint8_t)(1u << n); break;
    }
    icount -= 8;
}

/* DD CB d op / FD CB d op: the operation on (IX+d); the undocumented forms
 * with a register copy the result into it as well. */
static void exec_xycb(uint16_t ea)
{
    uint8_t op = ARG();
    int r = op & 7, n = (op >> 3) & 7;
    uint8_t v = RM(ea);

    if ((op >> 6) == 1) {
        op_bit(n, v, (uint8_t)(ea >> 8));
        icount -= 20;
        return;
    }
    switch (op >> 6) {
    case 0: v = op_rot(n, v); break;
    case 2: v &= (uint8_t)~(1u << n); break;
    default: v |= (uint8_t)(1u << n); break;
    }
    WM(ea, v);
    if (r != 6) *R8[r] = v;
    icount -= 23;
}

/* ---- the ED page ---------------------------------------------------------- */

static inline void blk_ld(int dir)
{
    uint8_t v = RM(HL);
    WM(DE, v);
    HL += dir;
    DE += dir;
    BC--;
    uint8_t n = (uint8_t)(v + A);
    F = (F & (SF | ZF | CF)) | (n & XF) | ((n << 4) & YF) | (BC ? VF : 0);
}

static inline void blk_cp(int dir)
{
    uint8_t v = RM(HL);
    uint8_t r = (uint8_t)(A - v);
    HL += dir;
    BC--;
    WZ += dir;
    F = (F & CF) | (SZ[r] & (SF | ZF)) | ((A ^ v ^ r) & HF) | NF;
    if (F & HF) r--;
    F |= (r & XF) | ((r << 4) & YF) | (BC ? VF : 0);
}

/* INI/IND/OUTI/OUTD: the flags as measured on the chip ('k' is the byte
 * moved plus C+-1 for the inputs, plus L after the move for the outputs). */
static inline void blk_io_flags(uint8_t v, unsigned k)
{
    F = SZ[B];
    if (v & 0x80) F |= NF;
    if (k > 0xFF) F |= HF | CF;
    F |= SZP[(uint8_t)((k & 7) ^ B)] & PF;
}

static inline void blk_in(int dir)
{
    uint8_t v = IN(BC);
    WZ = (uint16_t)(BC + dir);
    B--;
    WM(HL, v);
    HL += dir;
    blk_io_flags(v, (unsigned)v + (uint8_t)(C + dir));
}

static inline void blk_out(int dir)
{
    uint8_t v = RM(HL);
    B--;
    WZ = (uint16_t)(BC + dir);
    OUT(BC, v);
    HL += dir;
    blk_io_flags(v, (unsigned)v + L);
}

static void exec_ed(void)
{
    uint8_t op = ARG();
    Z80.r++;
    uint16_t nn;
    uint8_t v;

    switch (op) {
    /* IN r,(C) - 70 sets the flags only */
    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x70: case 0x78:
        v = IN(BC);
        WZ = (uint16_t)(BC + 1);
        F = (F & CF) | SZP[v];
        if (op != 0x70) *R8[(op >> 3) & 7] = v;
        icount -= 12;
        break;
    /* OUT (C),r - 71 writes 0 */
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x71: case 0x79:
        OUT(BC, op == 0x71 ? 0 : *R8[(op >> 3) & 7]);
        WZ = (uint16_t)(BC + 1);
        icount -= 12;
        break;
    case 0x42: op_sbc16(BC); icount -= 15; break;
    case 0x52: op_sbc16(DE); icount -= 15; break;
    case 0x62: op_sbc16(HL); icount -= 15; break;
    case 0x72: op_sbc16(SP); icount -= 15; break;
    case 0x4A: op_adc16(BC); icount -= 15; break;
    case 0x5A: op_adc16(DE); icount -= 15; break;
    case 0x6A: op_adc16(HL); icount -= 15; break;
    case 0x7A: op_adc16(SP); icount -= 15; break;
    case 0x43: nn = ARG16(); WM16(nn, BC); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x53: nn = ARG16(); WM16(nn, DE); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x63: nn = ARG16(); WM16(nn, HL); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x73: nn = ARG16(); WM16(nn, SP); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x4B: nn = ARG16(); BC = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x5B: nn = ARG16(); DE = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x6B: nn = ARG16(); HL = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x7B: nn = ARG16(); SP = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 20; break;
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C:
        v = A;
        A = 0;
        op_sub(v);
        icount -= 8;
        break;
    /* RETN and RETI (and their mirrors) */
    case 0x45: case 0x4D: case 0x55: case 0x5D:
    case 0x65: case 0x6D: case 0x75: case 0x7D:
        Z80.iff1 = Z80.iff2;
        PC = POP();
        WZ = PC;
        icount -= 14;
        break;
    case 0x46: case 0x4E: case 0x66: case 0x6E: Z80.im = 0; icount -= 8; break;
    case 0x56: case 0x76: Z80.im = 1; icount -= 8; break;
    case 0x5E: case 0x7E: Z80.im = 2; icount -= 8; break;
    case 0x47: Z80.i = A; icount -= 9; break;
    case 0x4F: Z80.r = A; Z80.r2 = A; icount -= 9; break;
    case 0x57:
        A = Z80.i;
        F = (F & CF) | SZ[A] | (Z80.iff2 ? VF : 0);
        icount -= 9;
        break;
    case 0x5F:
        A = (uint8_t)((Z80.r & 0x7F) | (Z80.r2 & 0x80));
        F = (F & CF) | SZ[A] | (Z80.iff2 ? VF : 0);
        icount -= 9;
        break;
    case 0x67: /* RRD */
        v = RM(HL);
        WZ = (uint16_t)(HL + 1);
        WM(HL, (uint8_t)((v >> 4) | (A << 4)));
        A = (uint8_t)((A & 0xF0) | (v & 0x0F));
        F = (F & CF) | SZP[A];
        icount -= 18;
        break;
    case 0x6F: /* RLD */
        v = RM(HL);
        WZ = (uint16_t)(HL + 1);
        WM(HL, (uint8_t)((v << 4) | (A & 0x0F)));
        A = (uint8_t)((A & 0xF0) | (v >> 4));
        F = (F & CF) | SZP[A];
        icount -= 18;
        break;

    case 0xA0: blk_ld(1);  icount -= 16; break;
    case 0xA8: blk_ld(-1); icount -= 16; break;
    case 0xA1: blk_cp(1);  icount -= 16; break;
    case 0xA9: blk_cp(-1); icount -= 16; break;
    case 0xA2: blk_in(1);  icount -= 16; break;
    case 0xAA: blk_in(-1); icount -= 16; break;
    case 0xA3: blk_out(1);  icount -= 16; break;
    case 0xAB: blk_out(-1); icount -= 16; break;

    /* The repeating forms go round by stepping PC back over themselves:
     * one pass per instruction, as the chip does, so interrupts and the
     * line timing see each one. */
    case 0xB0: case 0xB8:
        blk_ld(op == 0xB0 ? 1 : -1);
        if (BC) {
            PC -= 2;
            WZ = (uint16_t)(PC + 1);
            icount -= 21;
        } else {
            icount -= 16;
        }
        break;
    case 0xB1: case 0xB9:
        blk_cp(op == 0xB1 ? 1 : -1);
        if (BC && !(F & ZF)) {
            PC -= 2;
            WZ = (uint16_t)(PC + 1);
            icount -= 21;
        } else {
            icount -= 16;
        }
        break;
    case 0xB2: case 0xBA:
        blk_in(op == 0xB2 ? 1 : -1);
        if (B) {
            PC -= 2;
            icount -= 21;
        } else {
            icount -= 16;
        }
        break;
    case 0xB3: case 0xBB:
        blk_out(op == 0xB3 ? 1 : -1);
        if (B) {
            PC -= 2;
            icount -= 21;
        } else {
            icount -= 16;
        }
        break;

    default: /* the rest of the page does nothing */
        icount -= 8;
        break;
    }
}

/* ---- the unprefixed page -------------------------------------------------- */

static void exec_op(uint8_t op);

/* DD and FD: the instructions that use HL, H or L, with IX or IY (or their
 * halves) instead; the others run as without the prefix, 4 T-states
 * later. */
static void exec_xy(PAIR *xy)
{
    for (;;) {
        uint8_t op = ARG();
        Z80.r++;
        uint16_t ea, nn;

#define XYR   xy->w.l
#define XH    xy->b.h
#define XL    xy->b.l
#define EA()  (ea = (uint16_t)(XYR + (int8_t)ARG()), WZ = ea)

        switch (op) {
        case 0x09: XYR = op_add16(XYR, BC); icount -= 15; return;
        case 0x19: XYR = op_add16(XYR, DE); icount -= 15; return;
        case 0x29: XYR = op_add16(XYR, XYR); icount -= 15; return;
        case 0x39: XYR = op_add16(XYR, SP); icount -= 15; return;
        case 0x21: XYR = ARG16(); icount -= 14; return;
        case 0x22: nn = ARG16(); WM16(nn, XYR); WZ = (uint16_t)(nn + 1); icount -= 20; return;
        case 0x2A: nn = ARG16(); XYR = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 20; return;
        case 0x23: XYR++; icount -= 10; return;
        case 0x2B: XYR--; icount -= 10; return;
        case 0x24: XH = op_inc(XH); icount -= 8; return;
        case 0x25: XH = op_dec(XH); icount -= 8; return;
        case 0x26: XH = ARG(); icount -= 11; return;
        case 0x2C: XL = op_inc(XL); icount -= 8; return;
        case 0x2D: XL = op_dec(XL); icount -= 8; return;
        case 0x2E: XL = ARG(); icount -= 11; return;
        case 0x34: EA(); WM(ea, op_inc(RM(ea))); icount -= 23; return;
        case 0x35: EA(); WM(ea, op_dec(RM(ea))); icount -= 23; return;
        case 0x36: EA(); WM(ea, ARG()); icount -= 19; return;

        case 0x44: B = XH; icount -= 8; return;
        case 0x45: B = XL; icount -= 8; return;
        case 0x46: EA(); B = RM(ea); icount -= 19; return;
        case 0x4C: C = XH; icount -= 8; return;
        case 0x4D: C = XL; icount -= 8; return;
        case 0x4E: EA(); C = RM(ea); icount -= 19; return;
        case 0x54: D = XH; icount -= 8; return;
        case 0x55: D = XL; icount -= 8; return;
        case 0x56: EA(); D = RM(ea); icount -= 19; return;
        case 0x5C: E = XH; icount -= 8; return;
        case 0x5D: E = XL; icount -= 8; return;
        case 0x5E: EA(); E = RM(ea); icount -= 19; return;
        case 0x60: XH = B; icount -= 8; return;
        case 0x61: XH = C; icount -= 8; return;
        case 0x62: XH = D; icount -= 8; return;
        case 0x63: XH = E; icount -= 8; return;
        case 0x64: icount -= 8; return;
        case 0x65: XH = XL; icount -= 8; return;
        case 0x66: EA(); H = RM(ea); icount -= 19; return;
        case 0x67: XH = A; icount -= 8; return;
        case 0x68: XL = B; icount -= 8; return;
        case 0x69: XL = C; icount -= 8; return;
        case 0x6A: XL = D; icount -= 8; return;
        case 0x6B: XL = E; icount -= 8; return;
        case 0x6C: XL = XH; icount -= 8; return;
        case 0x6D: icount -= 8; return;
        case 0x6E: EA(); L = RM(ea); icount -= 19; return;
        case 0x6F: XL = A; icount -= 8; return;
        case 0x70: EA(); WM(ea, B); icount -= 19; return;
        case 0x71: EA(); WM(ea, C); icount -= 19; return;
        case 0x72: EA(); WM(ea, D); icount -= 19; return;
        case 0x73: EA(); WM(ea, E); icount -= 19; return;
        case 0x74: EA(); WM(ea, H); icount -= 19; return;
        case 0x75: EA(); WM(ea, L); icount -= 19; return;
        case 0x77: EA(); WM(ea, A); icount -= 19; return;
        case 0x7C: A = XH; icount -= 8; return;
        case 0x7D: A = XL; icount -= 8; return;
        case 0x7E: EA(); A = RM(ea); icount -= 19; return;

#define XY_ALU(base, fn)                                              \
        case base + 4: fn(XH); icount -= 8; return;                   \
        case base + 5: fn(XL); icount -= 8; return;                   \
        case base + 6: EA(); fn(RM(ea)); icount -= 19; return;
        XY_ALU(0x80, op_add)
        XY_ALU(0x88, op_adc)
        XY_ALU(0x90, op_sub)
        XY_ALU(0x98, op_sbc)
        XY_ALU(0xA0, op_and)
        XY_ALU(0xA8, op_xor)
        XY_ALU(0xB0, op_or)
        XY_ALU(0xB8, op_cp)
#undef XY_ALU

        case 0xCB: EA(); exec_xycb(ea); return;
        case 0xE1: XYR = POP(); icount -= 14; return;
        case 0xE5: PUSH(XYR); icount -= 15; return;
        case 0xE3:
            nn = RM16(SP);
            WM16(SP, XYR);
            XYR = nn;
            WZ = nn;
            icount -= 23;
            return;
        case 0xE9: PC = XYR; icount -= 8; return;
        case 0xF9: SP = XYR; icount -= 10; return;

        case 0xDD: xy = &Z80.ix; icount -= 4; continue;
        case 0xFD: xy = &Z80.iy; icount -= 4; continue;
        case 0xED: icount -= 4; exec_ed(); return;
        default:
            icount -= 4;
            exec_op(op);
            return;
        }
#undef XYR
#undef XH
#undef XL
#undef EA
    }
}

static inline void jr(void)
{
    int8_t d = (int8_t)ARG();
    PC = (uint16_t)(PC + d);
    WZ = PC;
}

static void exec_op(uint8_t op)
{
    uint16_t nn;
    uint8_t v;

    switch (op) {
    case 0x00: icount -= 4; break;
    case 0x01: BC = ARG16(); icount -= 10; break;
    case 0x11: DE = ARG16(); icount -= 10; break;
    case 0x21: HL = ARG16(); icount -= 10; break;
    case 0x31: SP = ARG16(); icount -= 10; break;
    case 0x02: WM(BC, A); Z80.wz.b.l = (uint8_t)(BC + 1); Z80.wz.b.h = A; icount -= 7; break;
    case 0x12: WM(DE, A); Z80.wz.b.l = (uint8_t)(DE + 1); Z80.wz.b.h = A; icount -= 7; break;
    case 0x0A: A = RM(BC); WZ = (uint16_t)(BC + 1); icount -= 7; break;
    case 0x1A: A = RM(DE); WZ = (uint16_t)(DE + 1); icount -= 7; break;
    case 0x22: nn = ARG16(); WM16(nn, HL); WZ = (uint16_t)(nn + 1); icount -= 16; break;
    case 0x2A: nn = ARG16(); HL = RM16(nn); WZ = (uint16_t)(nn + 1); icount -= 16; break;
    case 0x32:
        nn = ARG16();
        WM(nn, A);
        Z80.wz.b.l = (uint8_t)(nn + 1);
        Z80.wz.b.h = A;
        icount -= 13;
        break;
    case 0x3A: nn = ARG16(); A = RM(nn); WZ = (uint16_t)(nn + 1); icount -= 13; break;

    case 0x03: BC++; icount -= 6; break;
    case 0x13: DE++; icount -= 6; break;
    case 0x23: HL++; icount -= 6; break;
    case 0x33: SP++; icount -= 6; break;
    case 0x0B: BC--; icount -= 6; break;
    case 0x1B: DE--; icount -= 6; break;
    case 0x2B: HL--; icount -= 6; break;
    case 0x3B: SP--; icount -= 6; break;
    case 0x09: HL = op_add16(HL, BC); icount -= 11; break;
    case 0x19: HL = op_add16(HL, DE); icount -= 11; break;
    case 0x29: HL = op_add16(HL, HL); icount -= 11; break;
    case 0x39: HL = op_add16(HL, SP); icount -= 11; break;

    case 0x04: B = op_inc(B); icount -= 4; break;
    case 0x0C: C = op_inc(C); icount -= 4; break;
    case 0x14: D = op_inc(D); icount -= 4; break;
    case 0x1C: E = op_inc(E); icount -= 4; break;
    case 0x24: H = op_inc(H); icount -= 4; break;
    case 0x2C: L = op_inc(L); icount -= 4; break;
    case 0x3C: A = op_inc(A); icount -= 4; break;
    case 0x34: WM(HL, op_inc(RM(HL))); icount -= 11; break;
    case 0x05: B = op_dec(B); icount -= 4; break;
    case 0x0D: C = op_dec(C); icount -= 4; break;
    case 0x15: D = op_dec(D); icount -= 4; break;
    case 0x1D: E = op_dec(E); icount -= 4; break;
    case 0x25: H = op_dec(H); icount -= 4; break;
    case 0x2D: L = op_dec(L); icount -= 4; break;
    case 0x3D: A = op_dec(A); icount -= 4; break;
    case 0x35: WM(HL, op_dec(RM(HL))); icount -= 11; break;

    case 0x06: B = ARG(); icount -= 7; break;
    case 0x0E: C = ARG(); icount -= 7; break;
    case 0x16: D = ARG(); icount -= 7; break;
    case 0x1E: E = ARG(); icount -= 7; break;
    case 0x26: H = ARG(); icount -= 7; break;
    case 0x2E: L = ARG(); icount -= 7; break;
    case 0x3E: A = ARG(); icount -= 7; break;
    case 0x36: v = ARG(); WM(HL, v); icount -= 10; break;

    case 0x07:
        A = (uint8_t)((A << 1) | (A >> 7));
        F = (F & (SF | ZF | PF)) | (A & (YF | XF | CF));
        icount -= 4;
        break;
    case 0x0F:
        F = (F & (SF | ZF | PF)) | (A & CF);
        A = (uint8_t)((A >> 1) | (A << 7));
        F |= A & (YF | XF);
        icount -= 4;
        break;
    case 0x17:
        v = (uint8_t)((A << 1) | (F & CF));
        F = (F & (SF | ZF | PF)) | (A >> 7) | (v & (YF | XF));
        A = v;
        icount -= 4;
        break;
    case 0x1F:
        v = (uint8_t)((A >> 1) | (F << 7));
        F = (F & (SF | ZF | PF)) | (A & CF) | (v & (YF | XF));
        A = v;
        icount -= 4;
        break;

    case 0x08: {
        uint16_t t = AF;
        AF = Z80.af2.w.l;
        Z80.af2.w.l = t;
        icount -= 4;
        break;
    }
    case 0xD9: {
        uint16_t t;
        t = BC; BC = Z80.bc2.w.l; Z80.bc2.w.l = t;
        t = DE; DE = Z80.de2.w.l; Z80.de2.w.l = t;
        t = HL; HL = Z80.hl2.w.l; Z80.hl2.w.l = t;
        icount -= 4;
        break;
    }
    case 0xEB: nn = DE; DE = HL; HL = nn; icount -= 4; break;
    case 0xE3: nn = RM16(SP); WM16(SP, HL); HL = nn; WZ = nn; icount -= 19; break;

    case 0x10:
        B--;
        if (B) {
            jr();
            icount -= 13;
        } else {
            PC++;
            icount -= 8;
        }
        break;
    case 0x18: jr(); icount -= 12; break;
    case 0x20: if (!(F & ZF)) { jr(); icount -= 12; } else { PC++; icount -= 7; } break;
    case 0x28: if (F & ZF)    { jr(); icount -= 12; } else { PC++; icount -= 7; } break;
    case 0x30: if (!(F & CF)) { jr(); icount -= 12; } else { PC++; icount -= 7; } break;
    case 0x38: if (F & CF)    { jr(); icount -= 12; } else { PC++; icount -= 7; } break;

    case 0x27: op_daa(); icount -= 4; break;
    case 0x2F:
        A ^= 0xFF;
        F = (F & (SF | ZF | PF | CF)) | HF | NF | (A & (YF | XF));
        icount -= 4;
        break;
    case 0x37:
        F = (F & (SF | ZF | PF)) | CF | (A & (YF | XF));
        icount -= 4;
        break;
    case 0x3F:
        F = ((F & (SF | ZF | PF | CF)) | ((F & CF) << 4) | (A & (YF | XF))) ^ CF;
        icount -= 4;
        break;

    /* LD r,r' and HALT */
#define LDR(base, dst)                                         \
    case base + 0: dst = B; icount -= 4; break;                \
    case base + 1: dst = C; icount -= 4; break;                \
    case base + 2: dst = D; icount -= 4; break;                \
    case base + 3: dst = E; icount -= 4; break;                \
    case base + 4: dst = H; icount -= 4; break;                \
    case base + 5: dst = L; icount -= 4; break;                \
    case base + 6: dst = RM(HL); icount -= 7; break;           \
    case base + 7: dst = A; icount -= 4; break;
    LDR(0x40, B)
    LDR(0x48, C)
    LDR(0x50, D)
    LDR(0x58, E)
    LDR(0x60, H)
    LDR(0x68, L)
    LDR(0x78, A)
#undef LDR
    case 0x70: WM(HL, B); icount -= 7; break;
    case 0x71: WM(HL, C); icount -= 7; break;
    case 0x72: WM(HL, D); icount -= 7; break;
    case 0x73: WM(HL, E); icount -= 7; break;
    case 0x74: WM(HL, H); icount -= 7; break;
    case 0x75: WM(HL, L); icount -= 7; break;
    case 0x77: WM(HL, A); icount -= 7; break;
    case 0x76: Z80.halt = 1; icount -= 4; break;

#define ALU(base, fn)                                          \
    case base + 0: fn(B); icount -= 4; break;                  \
    case base + 1: fn(C); icount -= 4; break;                  \
    case base + 2: fn(D); icount -= 4; break;                  \
    case base + 3: fn(E); icount -= 4; break;                  \
    case base + 4: fn(H); icount -= 4; break;                  \
    case base + 5: fn(L); icount -= 4; break;                  \
    case base + 6: fn(RM(HL)); icount -= 7; break;             \
    case base + 7: fn(A); icount -= 4; break;
    ALU(0x80, op_add)
    ALU(0x88, op_adc)
    ALU(0x90, op_sub)
    ALU(0x98, op_sbc)
    ALU(0xA0, op_and)
    ALU(0xA8, op_xor)
    ALU(0xB0, op_or)
    ALU(0xB8, op_cp)
#undef ALU
    case 0xC6: op_add(ARG()); icount -= 7; break;
    case 0xCE: op_adc(ARG()); icount -= 7; break;
    case 0xD6: op_sub(ARG()); icount -= 7; break;
    case 0xDE: op_sbc(ARG()); icount -= 7; break;
    case 0xE6: op_and(ARG()); icount -= 7; break;
    case 0xEE: op_xor(ARG()); icount -= 7; break;
    case 0xF6: op_or(ARG()); icount -= 7; break;
    case 0xFE: op_cp(ARG()); icount -= 7; break;

    /* conditions by bits 3-5: NZ Z NC C PO PE P M */
#define COND(n) ((n) == 0 ? !(F & ZF) : (n) == 1 ? (F & ZF) : (n) == 2 ? !(F & CF) : \
                 (n) == 3 ? (F & CF) : (n) == 4 ? !(F & PF) : (n) == 5 ? (F & PF) :  \
                 (n) == 6 ? !(F & SF) : (F & SF))
#define RETCC(o, n) case o: if (COND(n)) { PC = POP(); WZ = PC; icount -= 11; } else icount -= 5; break;
#define JPCC(o, n)  case o: nn = ARG16(); WZ = nn; if (COND(n)) PC = nn; icount -= 10; break;
#define CALLCC(o, n) case o: nn = ARG16(); WZ = nn; \
        if (COND(n)) { PUSH(PC); PC = nn; icount -= 17; } else icount -= 10; break;
    RETCC(0xC0, 0) RETCC(0xC8, 1) RETCC(0xD0, 2) RETCC(0xD8, 3)
    RETCC(0xE0, 4) RETCC(0xE8, 5) RETCC(0xF0, 6) RETCC(0xF8, 7)
    JPCC(0xC2, 0) JPCC(0xCA, 1) JPCC(0xD2, 2) JPCC(0xDA, 3)
    JPCC(0xE2, 4) JPCC(0xEA, 5) JPCC(0xF2, 6) JPCC(0xFA, 7)
    CALLCC(0xC4, 0) CALLCC(0xCC, 1) CALLCC(0xD4, 2) CALLCC(0xDC, 3)
    CALLCC(0xE4, 4) CALLCC(0xEC, 5) CALLCC(0xF4, 6) CALLCC(0xFC, 7)
#undef RETCC
#undef JPCC
#undef CALLCC
#undef COND
    case 0xC9: PC = POP(); WZ = PC; icount -= 10; break;
    case 0xC3: PC = ARG16(); WZ = PC; icount -= 10; break;
    case 0xCD: nn = ARG16(); WZ = nn; PUSH(PC); PC = nn; icount -= 17; break;
    case 0xE9: PC = HL; icount -= 4; break;
    case 0xF9: SP = HL; icount -= 6; break;

    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        PUSH(PC);
        PC = op & 0x38;
        WZ = PC;
        icount -= 11;
        break;

    case 0xC1: BC = POP(); icount -= 10; break;
    case 0xD1: DE = POP(); icount -= 10; break;
    case 0xE1: HL = POP(); icount -= 10; break;
    case 0xF1: AF = POP(); icount -= 10; break;
    case 0xC5: PUSH(BC); icount -= 11; break;
    case 0xD5: PUSH(DE); icount -= 11; break;
    case 0xE5: PUSH(HL); icount -= 11; break;
    case 0xF5: PUSH(AF); icount -= 11; break;

    case 0xD3: {
        uint8_t n = ARG();
        OUT((uint16_t)(n | (A << 8)), A);
        Z80.wz.b.l = (uint8_t)(n + 1);
        Z80.wz.b.h = A;
        icount -= 11;
        break;
    }
    case 0xDB: {
        uint16_t port = (uint16_t)(ARG() | (A << 8));
        A = IN(port);
        WZ = (uint16_t)(port + 1);
        icount -= 11;
        break;
    }

    case 0xF3: Z80.iff1 = Z80.iff2 = 0; icount -= 4; break;
    case 0xFB: Z80.iff1 = Z80.iff2 = 1; Z80.after_ei = 1; icount -= 4; break;

    case 0xCB: exec_cb(); break;
    case 0xED: exec_ed(); break;
    case 0xDD: exec_xy(&Z80.ix); break;
    case 0xFD: exec_xy(&Z80.iy); break;
    }
}

/* ---- interrupts and the run loop ------------------------------------------ */

static void take_irq(void)
{
    if (Z80.halt) {
        Z80.halt = 0;
    }
    Z80.iff1 = Z80.iff2 = 0;
    Z80.r++;
    int vec = Z80.irq_callback ? Z80.irq_callback(0) : 0xFF;

    if (Z80.im == 2) {
        uint16_t at = (uint16_t)((vec & 0xFF) | (Z80.i << 8));
        PUSH(PC);
        PC = RM16(at);
        icount -= 19;
    } else {
        /* IM 1, and IM 0 with RST 38h on the bus: what these consoles put
         * there (their data bus floats high). */
        PUSH(PC);
        PC = (Z80.im == 0) ? (uint16_t)(vec & 0x38) : 0x0038;
        icount -= 13;
    }
    WZ = PC;
}

static void take_nmi(void)
{
    Z80.halt = 0;
    Z80.iff1 = 0;
    Z80.r++;
    PUSH(PC);
    PC = 0x0066;
    WZ = PC;
    icount -= 11;
    Z80.nmi_pending = 0;
}

int z80_execute(int cycles)
{
    icount = cycles;
    requested = cycles;
    in_exec = 1;

    /* An NMI comes only from outside (the pause button, the ColecoVision's
     * VDP), so between two calls: it is checked here. */
    if (Z80.nmi_pending) take_nmi();

    while (icount > 0) {
        if (Z80.irq_state != CLEAR_LINE && Z80.iff1 && !Z80.after_ei) take_irq();
        Z80.after_ei = 0;

        if (Z80.halt) {
            /* HALT repeats NOPs (R counts them) until an interrupt, and
             * nothing inside this call can raise one: the rest of the
             * slice goes at once. */
            if (icount > 0) {
                int n = (icount + 3) / 4;
                Z80.r += (uint8_t)n;
                icount -= 4 * n;
            }
            break;
        }

        uint8_t op = RM(PC);
        PC++;
        Z80.r++;
        exec_op(op);
    }

    in_exec = 0;
    int ran = cycles - icount;
    z80_cycle_count += ran;
    return ran;
}

void z80_set_irq_line(int irqline, int state)
{
    if (irqline == INPUT_LINE_NMI) {
        if (Z80.nmi_state == CLEAR_LINE && state != CLEAR_LINE) Z80.nmi_pending = 1;
        Z80.nmi_state = (uint8_t)state;
    } else {
        Z80.irq_state = (uint8_t)state;
    }
}

void z80_reset_cycle_count(void)
{
    z80_cycle_count = 0;
}

int z80_get_elapsed_cycles(void)
{
    if (in_exec) return z80_cycle_count + (requested - icount);
    return z80_cycle_count;
}

static int default_irq(int line)
{
    (void)line;
    return 0xFF;
}

void z80_init(int index, int clock, const void *config, int (*irq_callback)(int))
{
    (void)index;
    (void)clock;
    (void)config;

    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p += (i >> b) & 1;
        SZ[i] = (uint8_t)((i ? (i & SF) : ZF) | (i & (YF | XF)));
        SZP[i] = (uint8_t)(SZ[i] | ((p & 1) ? 0 : PF));
        SZHV_inc[i] = (uint8_t)(SZ[i] | (i == 0x80 ? VF : 0) | ((i & 0x0F) == 0 ? HF : 0));
        SZHV_dec[i] = (uint8_t)(SZ[i] | NF | (i == 0x7F ? VF : 0) | ((i & 0x0F) == 0x0F ? HF : 0));
    }

    void (*mw)(uint16_t, uint8_t) = Z80.mem_write;
    uint8_t (*mr)(uint16_t) = Z80.mem_read;
    void (*pw)(uint16_t, uint8_t) = Z80.port_write;
    uint8_t (*pr)(uint16_t) = Z80.port_read;
    memset(&Z80, 0, sizeof(Z80));
    Z80.mem_write = mw;
    Z80.mem_read = mr;
    Z80.port_write = pw;
    Z80.port_read = pr;
    Z80.irq_callback = irq_callback ? irq_callback : default_irq;
    /* The registers the chip leaves undefined at power on, as most
     * emulators set them. */
    AF = 0xFFFF;
    SP = 0xFFFF;
    IX = 0xFFFF;
    IY = 0xFFFF;
    icount = 0;
    requested = 0;
    in_exec = 0;
}

void z80_reset(void)
{
    PC = 0x0000;
    Z80.i = 0;
    Z80.r = 0;
    Z80.r2 = 0;
    Z80.im = 0;
    Z80.iff1 = Z80.iff2 = 0;
    Z80.halt = 0;
    Z80.after_ei = 0;
    Z80.nmi_pending = 0;
    WZ = PC;
}

void z80_exit(void)
{
}
