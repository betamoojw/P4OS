/* Generator is (c) James Ponder, 1997-2001 http://www.squish.net/generator/ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* P4OS: no ESP-IDF here; the big tables and the IPC pool come from the
 * core's allocator (ng_big_alloc, under its PSRAM budget). */
void *ng_big_alloc(size_t n);
void ng_big_free(void *p);

#include "generator.h"
#include "cpu68k.h"
#include "mem68k.h"
#include "def68k-iibs.h"
#include "def68k-proto.h"
#include "def68k-funcs.h"
#include "reg68k.h"

int diss68k_gettext(t_ipc * ipc, char *text);

/* Handler for illegal/unrecognized 68K instructions — fires vector 4 */
static void cpu68k_illegal_op(t_ipc *ipc) {
    reg68k_internal_vector(V_ILLEGAL, regs.pc);
}

/*** externed variables ***/

uint8 *cpu68k_rom = NULL;
unsigned int cpu68k_romlen = 0;
uint8 *cpu68k_ram = NULL;
/* P4OS: allocated by cpu68k_alloc() (768 KB on the board) instead of
 * .bss, where they would take PSRAM whatever console the app plays. */
t_iib **cpu68k_iibtable;
void (**cpu68k_functable) (t_ipc * ipc);
int cpu68k_totalinstr;
int cpu68k_totalfuncs;

unsigned int cpu68k_clocks;
unsigned int cpu68k_frames;
unsigned int cpu68k_frozen;     /* cpu frozen, do not interrupt, make pending */
t_regs regs;
uint8 movem_bit[256];
t_ipclist **ipclist; /* P4OS: allocated by cpu68k_alloc() */

/* ── IPC Pool Allocator ──────────────────────────────────────────────
 * One contiguous 2 MB block from PSRAM.  Bump pointer for allocation.
 * When full, zero the hash table and reset the bump pointer — instant
 * "flush" with no per-entry free() calls.  Eliminates malloc overhead,
 * heap fragmentation, and scattered PSRAM accesses.
 * ──────────────────────────────────────────────────────────────────── */
/* P4OS: 2 MB (1 MB if the budget is short), allocated with the tables
 * by cpu68k_alloc(); RetroESP32 asked for 3.5 MB. A full pool is only
 * flushed and refilled. */
/* P4OS: 1 MB, not 2: the sprite cache makes better use of the other MB
 * (Metal Slug flushes the pool 3 times at 2 MB, measured 2026-10-17) */
#define IPC_POOL_MAX   (1024 * 1024)
#define IPC_POOL_MIN   (512 * 1024)
static uint8 *ipc_pool      = NULL;   /* base of the pool                */
static uint32 ipc_pool_used = 0;      /* bump pointer (bytes used so far) */
static uint32 ipc_pool_size = 0;      /* actual allocated size            */

/* Allocate from the IPC pool.  Returns NULL if the pool is full. */
static void *ipc_pool_alloc(uint32 size)
{
  /* Align to 4 bytes */
  size = (size + 3) & ~3u;
  if (ipc_pool_used + size > ipc_pool_size)
    return NULL;
  void *p = ipc_pool + ipc_pool_used;
  ipc_pool_used += size;
  return p;
}

//extern uint8 current_cpu_bank;
extern uint32 bankaddress;

/*** global variables ***/

/*** forward references ***/

void cpu68k_reset(void);

int cpu68k_init(void)
{
  t_iib *iib;
  uint16 bitmap;
  int i, j, sbit, dbit, sbits, dbits;

  memset(cpu68k_iibtable, 0, 65536 * sizeof(*cpu68k_iibtable));       /* P4OS */
  memset(cpu68k_functable, 0, 65536 * 2 * sizeof(*cpu68k_functable)); /* P4OS */
  memset(&regs, 0, sizeof(regs));

  cpu68k_frozen = 0;
  cpu68k_totalinstr = 0;

  for (i = 0; i < iibs_num; i++) {
    iib = &iibs[i];

    bitmap = iib->mask;
    sbits = 0;
    dbits = 0;

    for (j = 0; j < 2; j++) {
      switch (j ? iib->stype : iib->dtype) {
      case dt_Dreg:
      case dt_Areg:
      case dt_Aind:
      case dt_Ainc:
      case dt_Adec:
      case dt_Adis:
      case dt_Aidx:
        if (j) {
          bitmap ^= 7 << iib->sbitpos;
          sbits = 3;
        } else {
          bitmap ^= 7 << iib->dbitpos;
          dbits = 3;
        }
        break;
      case dt_AbsW:
      case dt_AbsL:
      case dt_Pdis:
      case dt_Pidx:
        break;
      case dt_ImmB:
      case dt_ImmW:
      case dt_ImmL:
      case dt_ImmS:
        break;
      case dt_Imm3:
        if (j) {
          bitmap ^= 7 << iib->sbitpos;
          sbits = 3;
        } else {
          bitmap ^= 7 << iib->dbitpos;
          dbits = 3;
        }
        break;
      case dt_Imm4:
        if (j) {
          bitmap ^= 15 << iib->sbitpos;
          sbits = 4;
        } else {
          bitmap ^= 15 << iib->dbitpos;
          dbits = 4;
        }
        break;
      case dt_Imm8:
      case dt_Imm8s:
        if (j) {
          bitmap ^= 255 << iib->sbitpos;
          sbits = 8;
        } else {
          bitmap ^= 255 << iib->dbitpos;
          dbits = 8;
        }
        break;
      case dt_ImmV:
        sbits = 12;
        bitmap ^= 0x0FFF;
        break;
      case dt_Ill:
        /* no src/dst parameter */
        break;
      default:
        LOG_CRITICAL(("CPU definition #%d incorrect", i));
        return 1;
      }
    }
    if (bitmap != 0xFFFF) {
      LOG_CRITICAL(("CPU definition #%d incorrect (0x%x)", i, bitmap));
      return 1;
    }
    for (sbit = 0; sbit < (1 << sbits); sbit++) {
      for (dbit = 0; dbit < (1 << dbits); dbit++) {
        bitmap = iib->bits | (sbit << iib->sbitpos) | (dbit << iib->dbitpos);
        if (iib->stype == dt_Imm3 || iib->stype == dt_Imm4
            || iib->stype == dt_Imm8) {
          if (sbit == 0 && iib->flags.imm_notzero) {
            continue;
          }
        }
        if (cpu68k_iibtable[bitmap] != NULL) {
          LOG_CRITICAL(("CPU definition #%d conflicts (0x%x)", i, bitmap));
          return 1;
        }
        cpu68k_iibtable[bitmap] = iib;
        /* set both flag and non-flag versions */
        cpu68k_functable[bitmap * 2] = cpu68k_funcindex[i * 2];
        cpu68k_functable[bitmap * 2 + 1] = cpu68k_funcindex[i * 2 + 1];
        cpu68k_totalinstr++;
      }
    }
  }

  j = 0;

  for (i = 0; i < 65536; i++) {
    if (cpu68k_iibtable[i]) {
      j++;
    }
  }
  if (j != cpu68k_totalinstr) {
    LOG_CRITICAL(("Instruction count not verified (%d/%d)\n",
                  cpu68k_totalinstr, i));
    return 1;
  }

  cpu68k_totalfuncs = iibs_num;

  for (i = 0; i < 256; i++) {
    for (j = 0; j < 8; j++) {
      if (i & (1 << j))
        break;
    }
    movem_bit[i] = j;
  }

  LOG_VERBOSE(("CPU: %d instructions supported by %d routines",
               cpu68k_totalinstr, cpu68k_totalfuncs));
  iib = cpu68k_iibtable[0x2F39];
  return 0;
}

#if 0 /* P4OS: needs the disassembler (diss68k.c, not vendored) */
void cpu68k_printipc(t_ipc * ipc)
{
    static char dasmtxt[256];
  printf("IPC @ 0x%p\n", ipc);
  diss68k_gettext(ipc,dasmtxt);
  printf("%s\n",dasmtxt);
  printf("  opcode: %04X, uses %X set %X\n", ipc->opcode, ipc->used,
         ipc->set);
  printf("  src = %08X\n", (unsigned)ipc->src);
  printf("  dst = %08X\n", (unsigned)ipc->dst);
}

#endif

/* fill in ipc */

void cpu68k_ipc(uint32 addr68k, uint8 *addr, t_iib * iib, t_ipc * ipc)
{
  t_type type;
  uint32 *p;

  ipc->opcode = LOCENDIAN16(*(uint16 *)addr);
  ipc->wordlen = 1;
  if (!iib) {
    /* illegal instruction, no further details (wordlen must be set to 1) */
    return;
  }

  ipc->used = iib->flags.used;
  ipc->set = iib->flags.set;

  if ((iib->mnemonic == i_Bcc) || (iib->mnemonic == i_BSR)) {
    /* special case - we can calculate the offset now */
    /* low 8 bits of current instruction are addr+1 */
    ipc->src = (sint32)(*(sint8 *)(addr + 1));
    if (ipc->src == 0) {
      ipc->src = (sint32)(sint16)LOCENDIAN16(*(uint16 *)(addr + 2));
      ipc->wordlen++;
    }
    ipc->src += addr68k + 2;    /* add PC of next instruction */
    return;
  }
  if (iib->mnemonic == i_DBcc || iib->mnemonic == i_DBRA) {
    /* special case - we can calculate the offset now */
    ipc->src = (sint32)(sint16)LOCENDIAN16(*(uint16 *)(addr + 2));
    ipc->src += addr68k + 2;    /* add PC of next instruction */
    ipc->wordlen++;
    return;
  }

  addr += 2;
  addr68k += 2;

  for (type = 0; type < 2; type++) {
    if (type == tp_src)
      p = &(ipc->src);
    else
      p = &(ipc->dst);

    switch (type == tp_src ? iib->stype : iib->dtype) {
    case dt_Adis:
      *p = (sint32)(sint16)LOCENDIAN16(*(uint16 *)addr);
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_Aidx:
      *p = (sint32)(sint8)addr[1];
      *p = (*p & 0xFFFFFF) | (*addr) << 24;
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_AbsW:
      *p = (sint32)(sint16)LOCENDIAN16(*(uint16 *)addr);
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_AbsL:
      *p = (uint32)((LOCENDIAN16(*(uint16 *)addr) << 16) +
                    LOCENDIAN16(*(uint16 *)(addr + 2)));
      ipc->wordlen += 2;
      addr += 4;
      addr68k += 4;
      break;
    case dt_Pdis:
      *p = (sint32)(sint16)LOCENDIAN16(*(uint16 *)addr);
      *p += addr68k;            /* add PC of extension word (this word) */
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_Pidx:
      *p = ((sint32)(sint8)addr[1]) + addr68k;
      *p = (*p & 0xFFFFFF) | (*addr) << 24;
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_ImmB:
      /* low 8 bits of next 16 bit word is addr+1 */
      *p = (uint32)(*(uint8 *)(addr + 1));
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_ImmW:
      *p = (uint32)LOCENDIAN16(*(uint16 *)addr);
      ipc->wordlen++;
      addr += 2;
      addr68k += 2;
      break;
    case dt_ImmL:
      *p = (uint32)((LOCENDIAN16(*(uint16 *)addr) << 16) +
                    LOCENDIAN16(*(uint16 *)(addr + 2)));
      ipc->wordlen += 2;
      addr += 4;
      addr68k += 4;
      break;
    case dt_Imm3:
      if (type == tp_src)
        *p = (ipc->opcode >> iib->sbitpos) & 7;
      else
        *p = (ipc->opcode >> iib->dbitpos) & 7;
      break;
    case dt_Imm4:
      if (type == tp_src)
        *p = (ipc->opcode >> iib->sbitpos) & 15;
      else
        *p = (ipc->opcode >> iib->dbitpos) & 15;
      break;
    case dt_Imm8:
      if (type == tp_src)
        *p = (ipc->opcode >> iib->sbitpos) & 255;
      else
        *p = (ipc->opcode >> iib->dbitpos) & 255;
      break;
    case dt_Imm8s:
      if (type == tp_src)
        *p = (sint32)(sint8)((ipc->opcode >> iib->sbitpos) & 255);
      else
        *p = (sint32)(sint8)((ipc->opcode >> iib->dbitpos) & 255);
      break;
    default:
      break;
    }
  }
}

/* forward declaration */
void cpu68k_clearcache(void);

t_ipclist *cpu68k_makeipclist(uint32 pc)
{
  int size = 64;
  unsigned int alloc_size = sizeof(t_ipclist) + 64 * sizeof(t_ipc) + 8;
  t_ipclist *list;
  t_ipc *ipc;
  t_iib *iib;
  int instrs = 0;
  uint16 required;
  int i;

  /* P4OS: the pool is allocated by cpu68k_alloc() at load. */
  list = ipc_pool_alloc(alloc_size);
  if (list == NULL) {
      printf("IPC pool full (%uK) — flushing\n", ipc_pool_used / 1024);
      cpu68k_clearcache();
      list = ipc_pool_alloc(alloc_size);
      if (list == NULL) /* P4OS: cannot happen with a pool of 1 MB */
          return NULL;
  }
  ipc = (t_ipc *) (list + 1);

  pc &= 0xffffff;
  list->pc = pc;
  list->clocks = 0;
  list->norepeat = 0;

  if ((pc&0xF00000)==0x200000)
      list->bank = bankaddress;
  else
       list->bank = 0;


  do {
    instrs++;
    if (instrs > size) {
	if (size > 10000) {
	    /* P4OS: a runaway block (10000 instructions without a branch:
	     * executing garbage). gngeo exited; here the block is cut. */
	    printf("Something has gone seriously wrong @ %08X\n", (unsigned)pc);
	    instrs--;
	    break;
	}
	size *= 2;  /* double to minimize repeated grow+waste */
	{
	    unsigned int new_alloc = sizeof(t_ipclist) + size * sizeof(t_ipc) + 8;
	    /* Pool allocator: can't realloc in place.  Allocate new, copy. */
	    /* The old space is abandoned (reclaimed on next flush). */
	    t_ipclist *newlist = ipc_pool_alloc(new_alloc);
	    if (newlist == NULL) {
		uint32 save_pc = list->pc;
		printf("IPC pool full during grow — flushing\n");
		cpu68k_clearcache();
		return cpu68k_makeipclist(save_pc);
	    }
	    memcpy(newlist, list, sizeof(t_ipclist) + (instrs - 1) * sizeof(t_ipc));
	    list = newlist;
	}
	ipc = ((t_ipc *) (list + 1)) + instrs - 1;
    }
    if (!(iib = cpu68k_iibtable[fetchword(pc)])) {
	/* Illegal instruction — emit exception handler and end block */
	ipc->opcode = fetchword(pc);
	ipc->wordlen = 1;
	ipc->used = 0;
	ipc->set = 0;
	ipc->src = 0;
	ipc->dst = 0;
	ipc->function = cpu68k_illegal_op;
	list->clocks += 34;
	pc += 2;
	ipc++;
	break;
    }
    cpu68k_ipc(pc, mem68k_memptr[pc >> 12] (pc), iib, ipc);
    list->clocks += iib->clocks;
    pc += (iib->wordlen) << 1;
    ipc++;
  }
  while (!iib->flags.endblk);
  *(int *)ipc = 0;

  if (instrs == 2 && iib) {
    ipc--;
    if (iib->mnemonic == i_Bcc && ipc->src == list->pc) {
      /* we have a 2-instruction block ending in a branch to start */
      ipc = (t_ipc *) (list + 1);
      iib = cpu68k_iibtable[ipc->opcode];
      if (iib->mnemonic == i_TST || iib->mnemonic == i_CMP) {
        /* it's a tst/cmp and then a Bcc */
        if (!(iib->stype == dt_Ainc || iib->stype == dt_Adec)) {
          /* no change could happen during the block */
          list->norepeat = 1;
        }
      }
    }
  }

  ipc = ((t_ipc *) (list + 1)) + instrs - 1;
  required = 0x1F;              /* all 5 flags need to be correct at end */
  for (i = 0; i < instrs; i++) {
    ipc->set &= required;
    required &= ~ipc->set;
    required |= ipc->used;
    {
      void *fn = ipc->set
        ? (void *)cpu68k_functable[(ipc->opcode << 1) + 1]
        : (void *)cpu68k_functable[ipc->opcode << 1];
      if (fn) ipc->function = fn;
      /* else: keep existing handler (e.g. illegal instruction) */
    }
    ipc--;
  }
  /* fprintf("Cached %08X to %08X\n", list->pc, pc-((iib->wordlen)<<1)); */
  return list;
}

void cpu68k_clearcache(void)
{
  /* Pool allocator: just zero the hash table and reset the bump pointer.
   * No per-entry free() needed — the entire pool is reused. */
  memset(ipclist, 0, LEN_IPCLISTTABLE * sizeof(*ipclist)); /* P4OS */
  ipc_pool_used = 0;
  cpu68k_flushes++; /* P4OS */
}

/* P4OS: the tables and the pool, allocated when a game is loaded and
 * freed when it is unloaded. */
uint32_t cpu68k_flushes;

int cpu68k_alloc(void)
{
  cpu68k_iibtable = ng_big_alloc(65536 * sizeof(*cpu68k_iibtable));
  cpu68k_functable = ng_big_alloc(65536 * 2 * sizeof(*cpu68k_functable));
  ipclist = ng_big_alloc(LEN_IPCLISTTABLE * sizeof(*ipclist));
  ipc_pool_size = IPC_POOL_MAX;
  ipc_pool = ng_big_alloc(ipc_pool_size);
  if (!ipc_pool) {
    ipc_pool_size = IPC_POOL_MIN;
    ipc_pool = ng_big_alloc(ipc_pool_size);
  }
  ipc_pool_used = 0;
  cpu68k_flushes = 0;
  if (!cpu68k_iibtable || !cpu68k_functable || !ipclist || !ipc_pool) {
    cpu68k_free();
    return 0;
  }
  return 1;
}

void cpu68k_free(void)
{
  ng_big_free(cpu68k_iibtable);
  ng_big_free(cpu68k_functable);
  ng_big_free(ipclist);
  ng_big_free(ipc_pool);
  cpu68k_iibtable = NULL;
  cpu68k_functable = NULL;
  ipclist = NULL;
  ipc_pool = NULL;
  ipc_pool_size = ipc_pool_used = 0;
}

uint32_t ipc_pool_used_bytes(void) { return ipc_pool_used; }
uint32_t ipc_pool_total_bytes(void) { return ipc_pool_size; }

void cpu68k_reset(void)
{
  int i;
#if 0
  if (!cpu68k_ram) {
    /* +4 due to bug in DIRECTRAM hdr/mem68k.h code over-run of buffer */
    if ((cpu68k_ram = malloc(0x10000 + 4)) == NULL)
      ui_err("Out of memory");
  }
  memset(cpu68k_ram, 0, 0x10000);
#endif

  regs.pc = fetchlong(4);
  regs.regs[15] = fetchlong(0);
  regs.sr.sr_int = 0;
  regs.sr.sr_struct.s = 1;      /* Supervisor mode */
  regs.stop = 0;
  cpu68k_clocks = 0;
  cpu68k_frames = 0;            /* Number of frames */

  for (i = 0; i < LEN_IPCLISTTABLE; i++) {
    ipclist[i] = NULL;
  }
  ipc_pool_used = 0;
}

void cpu68k_endfield(void)
{
  cpu68k_clocks = 0;
}
