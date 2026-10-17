/*
 * P4OS - Emulador, Neo Geo core: the sound CPU.
 *
 * gngeo's cpu_z80_* interface (memory.h) on superzazu's Z80 (z80/, MIT),
 * in place of mamez80_interf.c and the MAME Z80 core, whose licence allows
 * non-commercial use only. The memory map is gngeo's: a flat 64 KB copy
 * where the four banked windows (0x8000, 0xC000, 0xE000, 0xF000) get the
 * cartridge's M1 ROM bank copied in when the sound program switches it,
 * and 2 KB of RAM at 0xF800. Ports go to z80_port_read/write (memory.c),
 * which talk to the YM2610 and the 68000.
 */
#include <string.h>

#include "gngeo/emu.h"     /* gngeo's (main/ has an emu.h too) */
#include "memory.h"
#include "state.h"
#include "z80/z80.h"
#include "ng_core.h"

static z80 s_z;
static uint8_t *s_mem;          /* 64 KB, allocated by ng_z80_alloc() */
static bool s_irq_line;

/* z80_bank[] is memory.c's */

static uint8_t port_in(z80 *z, uint16_t port)
{
    (void)z;
    return z80_port_read(port);
}

static void port_out(z80 *z, uint16_t port, uint8_t v)
{
    (void)z;
    z80_port_write(port, v);
}

bool ng_z80_alloc(void)
{
    if (!s_mem) s_mem = calloc(1, 0x10000);
    return s_mem != NULL;
}

void ng_z80_free(void)
{
    free(s_mem);
    s_mem = NULL;
}

void cpu_z80_switchbank(Uint8 bank, Uint16 PortNo)
{
    static const uint32_t win_size[4] = { 0x4000, 0x2000, 0x1000, 0x0800 };
    static const uint32_t win_at[4] = { 0x8000, 0xC000, 0xE000, 0xF000 };
    static const uint32_t win_mask[4] = { 0x0f, 0x1f, 0x3f, 0x7f };
    if (bank > 3) return;
    z80_bank[bank] = PortNo;
    uint32_t ofs = win_size[bank] * ((PortNo >> 8) & win_mask[bank]);
    /* As gngeo: a bank past the end of the ROM leaves the window as it
     * was. */
    if (ofs + win_size[bank] <= memory.rom.cpu_z80.size)
        memcpy(s_mem + win_at[bank], memory.rom.cpu_z80.p + ofs, win_size[bank]);
}

void cpu_z80_init(void)
{
    z80_init(&s_z);
    s_z.port_in = port_in;
    s_z.port_out = port_out;
    s_z.mem = s_mem;
    s_z.mem_ro_end = 0xF800;
    s_irq_line = false;

    memset(s_mem, 0, 0x10000);
    uint32_t n = memory.rom.cpu_z80.size < 0xF800 ? memory.rom.cpu_z80.size : 0xF800;
    memcpy(s_mem, memory.rom.cpu_z80.p, n);
    z80_bank[0] = 0x8000;
    z80_bank[1] = 0xc000;
    z80_bank[2] = 0xe000;
    z80_bank[3] = 0xf000;
    /* The Z80 starts at 0 with SP, AF at FFFF (z80_init) and interrupts
     * off, mode 0, as after a reset. */
}

void cpu_z80_run(int nbcycle)
{
    unsigned long end = s_z.cyc + (unsigned long)nbcycle;
    while ((long)(end - s_z.cyc) > 0) {
        /* The YM2610's interrupt is a level: while it is up and the Z80
         * accepts interrupts, it is taken again (z80_gen_int is an edge
         * that the core clears when it takes it). */
        if (s_irq_line && !s_z.int_pending) z80_gen_int(&s_z, 0xFF);
        /* Halted with nothing to wake it: the rest of the slice is NOPs. */
        if (s_z.halted && !s_z.nmi_pending && !(s_z.int_pending && s_z.iff1)) {
            s_z.cyc = end;
            break;
        }
        z80_step(&s_z);
    }
}

void cpu_z80_nmi(void)
{
    z80_gen_nmi(&s_z);
}

void cpu_z80_raise_irq(int l)
{
    (void)l;
    s_irq_line = true;
    z80_gen_int(&s_z, 0xFF);
}

void cpu_z80_lower_irq(void)
{
    s_irq_line = false;
    s_z.int_pending = 0;
}

/* ---- save states: the registers one by one, so the layout does not
 * depend on the compiler's bit fields ---------------------------------- */

typedef struct {
    uint32_t cyc;
    uint16_t pc, sp, ix, iy, mem_ptr;
    uint8_t a, b, c, d, e, h, l, f;
    uint8_t a_, b_, c_, d_, e_, h_, l_, f_;
    uint8_t i, r, iff_delay, im, int_data;
    uint8_t iff1, iff2, halted, int_pending, nmi_pending, irq_line;
} ng_z80_regs_t;

static uint8_t get_f(const z80 *z)
{
    return (z->cf << 0) | (z->nf << 1) | (z->pf << 2) | (z->xf << 3) |
           (z->hf << 4) | (z->yf << 5) | (z->zf << 6) | (z->sf << 7);
}

static void set_f(z80 *z, uint8_t v)
{
    z->cf = v & 1; z->nf = (v >> 1) & 1; z->pf = (v >> 2) & 1; z->xf = (v >> 3) & 1;
    z->hf = (v >> 4) & 1; z->yf = (v >> 5) & 1; z->zf = (v >> 6) & 1; z->sf = (v >> 7) & 1;
}

void ng_z80_state(ng_st_t *st)
{
    ng_z80_regs_t r;
    if (st->writing) {
        memset(&r, 0, sizeof(r));
        r.cyc = (uint32_t)s_z.cyc;
        r.pc = s_z.pc; r.sp = s_z.sp; r.ix = s_z.ix; r.iy = s_z.iy; r.mem_ptr = s_z.mem_ptr;
        r.a = s_z.a; r.b = s_z.b; r.c = s_z.c; r.d = s_z.d; r.e = s_z.e; r.h = s_z.h; r.l = s_z.l;
        r.f = get_f(&s_z);
        r.a_ = s_z.a_; r.b_ = s_z.b_; r.c_ = s_z.c_; r.d_ = s_z.d_; r.e_ = s_z.e_;
        r.h_ = s_z.h_; r.l_ = s_z.l_; r.f_ = s_z.f_;
        r.i = s_z.i; r.r = s_z.r; r.iff_delay = s_z.iff_delay; r.im = s_z.interrupt_mode;
        r.int_data = s_z.int_data;
        r.iff1 = s_z.iff1; r.iff2 = s_z.iff2; r.halted = s_z.halted;
        r.int_pending = s_z.int_pending; r.nmi_pending = s_z.nmi_pending;
        r.irq_line = s_irq_line;
    }
    ng_st_data(st, &r, sizeof(r));
    ng_st_data(st, z80_bank, sizeof(Uint16) * 4);
    ng_st_data(st, s_mem, 0x10000);
    if (!st->writing && !st->failed) {
        s_z.cyc = r.cyc;
        s_z.pc = r.pc; s_z.sp = r.sp; s_z.ix = r.ix; s_z.iy = r.iy; s_z.mem_ptr = r.mem_ptr;
        s_z.a = r.a; s_z.b = r.b; s_z.c = r.c; s_z.d = r.d; s_z.e = r.e; s_z.h = r.h; s_z.l = r.l;
        set_f(&s_z, r.f);
        s_z.a_ = r.a_; s_z.b_ = r.b_; s_z.c_ = r.c_; s_z.d_ = r.d_; s_z.e_ = r.e_;
        s_z.h_ = r.h_; s_z.l_ = r.l_; s_z.f_ = r.f_;
        s_z.i = r.i; s_z.r = r.r; s_z.iff_delay = r.iff_delay; s_z.interrupt_mode = r.im;
        s_z.int_data = r.int_data;
        s_z.iff1 = r.iff1; s_z.iff2 = r.iff2; s_z.halted = r.halted;
        s_z.int_pending = r.int_pending; s_z.nmi_pending = r.nmi_pending;
        s_irq_line = r.irq_line;
    }
}
