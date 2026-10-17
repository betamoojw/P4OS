/*
 * P4OS - Emulador, Sega 8-bit core: a Z80 for SMS Plus.
 *
 * Written for P4OS (cores/sms/z80/z80.c, GPL-2.0-or-later like the rest of
 * the core). It replaces the Z80 SMS Plus carries, Juergen Buchmueller's
 * "Portable Z80 emulator" 3.9 from MAME, whose notice allows non-commercial
 * use only and is not compatible with the GPL. The interface is the one
 * SMS Plus's sources expect of that file (z80_execute(), the Z80 register
 * block, cpu_readmap / cpu_writemap, the line states), so they keep calling
 * it as they did; the code behind it is new.
 *
 * Memory: opcodes, operands and data are read straight from cpu_readmap
 * (64 pages of 1 KB); writes go through Z80.mem_write so the cartridge's
 * mapper registers see them. Ports through Z80.port_read / port_write.
 */
#ifndef P4OS_Z80_H
#define P4OS_Z80_H

#include <stdint.h>

/* A register pair as SMS Plus addresses it: .w.l the 16-bit value, .b.l /
 * .b.h its bytes. Both targets (RISC-V and the Mac) are little-endian. */
typedef union {
    struct { uint8_t l, h, h2, h3; } b;
    struct { uint16_t l, h; } w;
    uint32_t d;
} PAIR;

/* Line states and input lines (the subset SMS Plus uses). */
enum {
    CLEAR_LINE = 0,
    ASSERT_LINE = 1,
};
#define INPUT_LINE_IRQ0 0
#define INPUT_LINE_NMI  32

typedef struct {
    PAIR pc, sp, af, bc, de, hl, ix, iy, wz;
    PAIR af2, bc2, de2, hl2;

    uint8_t r;              /* refresh register, bits 0-6 count            */
    uint8_t r2;             /* bit 7 of R as last written                  */
    uint8_t iff1;
    uint8_t iff2;

    uint8_t halt;
    uint8_t im;
    uint8_t i;
    uint8_t nmi_state;      /* the NMI line                                */

    uint8_t nmi_pending;    /* an NMI edge waits to be taken               */
    uint8_t irq_state;      /* the INT line                                */
    uint8_t after_ei;       /* the instruction after EI: no interrupt yet  */
    uint8_t _pad;

    int (*irq_callback)(int irqline);       /* the byte on the data bus (IM 0/2) */
    uint8_t (*mem_read)(uint16_t address);  /* unused by the CPU, kept for SMS Plus */
    void (*mem_write)(uint16_t address, uint8_t data);
    void (*port_write)(uint16_t port, uint8_t data);
    uint8_t (*port_read)(uint16_t port);
} Z80_Regs;

extern Z80_Regs Z80;
extern int z80_cycle_count;
extern unsigned char *cpu_readmap[64];
extern unsigned char *cpu_writemap[64];

void z80_init(int index, int clock, const void *config, int (*irq_callback)(int));
void z80_reset(void);
void z80_exit(void);
/* Runs at least 'cycles' T-states (the last instruction may overshoot);
 * returns how many ran. */
int  z80_execute(int cycles);
void z80_set_irq_line(int irqline, int state);
void z80_reset_cycle_count(void);
/* T-states since the last z80_reset_cycle_count(), counting the ones of
 * the z80_execute() in progress. */
int  z80_get_elapsed_cycles(void);

#endif
