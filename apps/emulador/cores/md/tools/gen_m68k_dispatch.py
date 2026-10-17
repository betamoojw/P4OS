#!/usr/bin/env python3
"""
P4OS - Emulador, Mega Drive core: the 68000's dispatch table, packed.

Musashi (as Gwenesis carries it) decodes each instruction through two
tables of 65536 entries: a function pointer per opcode (256 KB, and in a
shared object 65536 relocations the loader has to apply) and a byte of
cycles per opcode (64 KB). This script folds both into one table of 16-bit
words, the handler's number in the low 11 bits and the cycles / 2 in the
top 5, plus a short array of the ~1700 distinct handlers:

    m68k_dispatch.h   static const uint16_t m68ki_dispatch[0x10000]
    m68k_handlers.h   static void (*const m68ki_handlers[N])(void)

128 KB of read-only data with no relocations instead of 320 KB, and one
load per instruction instead of two.

The opcode-to-handler mapping is Musashi's own table builder
(m68ki_build_opcode_table in m68kops.h) run over m68k_opcode_handler_table,
and the script checks it against the static tables Gwenesis shipped
(m68ki_instruction_jump_table.h / m68ki_cycles.h, which stop at 0xEFBF and
leave the rest of the 64K NULL: an opcode from 0xEFC0 up, line F
included, called a NULL pointer). If the originals are not found next to
m68kops.h the check is skipped.

    python3 gen_m68k_dispatch.py <dir of m68kops.h> <output dir>
"""
import os
import re
import sys

MUL = 7


def parse_handler_table(text):
    start = text.index("m68k_opcode_handler_table[] =")
    body = text[start:text.index("{0, 0, 0, 0}", start)]
    rows = re.findall(r"\{(m68k_op_\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*\}", body)
    return [(n, int(m, 16), int(x, 16), int(c)) for n, m, x, c in rows]


def build(table):
    """m68ki_build_opcode_table(), step by step."""
    jt = ["m68k_op_illegal"] * 0x10000
    # The builder's default is 4 unmultiplied; the static table Gwenesis
    # ran with has 0 there. Either way the illegal-instruction exception
    # takes its own cycles off and the value never counts.
    cy = [0] * 0x10000
    i = 0
    n = len(table)
    while i < n and table[i][1] != 0xFF00:
        name, mask, match, c = table[i]
        for op in range(0x10000):
            if (op & mask) == match:
                jt[op] = name
                cy[op] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xFF00:
        name, mask, match, c = table[i]
        for k in range(0x100):
            jt[match | k] = name
            cy[match | k] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xF1F8:
        name, mask, match, c = table[i]
        for a in range(8):
            for b in range(8):
                op = match | (a << 9) | b
                jt[op] = name
                cy[op] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xFFF0:
        name, mask, match, c = table[i]
        for k in range(0x10):
            jt[match | k] = name
            cy[match | k] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xF1FF:
        name, mask, match, c = table[i]
        for k in range(8):
            jt[match | (k << 9)] = name
            cy[match | (k << 9)] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xFFF8:
        name, mask, match, c = table[i]
        for k in range(8):
            jt[match | k] = name
            cy[match | k] = c * MUL
        i += 1
    while i < n and table[i][1] == 0xFFFF:
        name, mask, match, c = table[i]
        jt[match] = name
        cy[match] = c * MUL
        i += 1
    if i != n:
        sys.exit("handler table: row %d (%s) has a mask the builder does not know" % (i, table[i][0]))
    return jt, cy


def check_against_static(src_dir, jt, cy):
    jpath = os.path.join(src_dir, "m68ki_instruction_jump_table.h")
    cpath = os.path.join(src_dir, "m68ki_cycles.h")
    if not (os.path.exists(jpath) and os.path.exists(cpath)):
        print("static tables not found: check skipped")
        return
    t = open(jpath).read()
    names = [s.strip() for s in t[t.index("{") + 1:t.rindex("}")].replace("\n", " ").split(",") if s.strip()]
    t = open(cpath).read()
    vals = [s.strip() for s in t[t.index("{") + 1:t.rindex("}")].replace("\n", " ").split(",") if s.strip()]
    cyc = [int(v.split("*")[0]) * int(v.split("*")[1]) for v in vals]
    bad = 0
    for op in range(len(names)):
        if names[op] != jt[op]:
            bad += 1
        elif cyc[op] != cy[op]:
            bad += 1
    print("static tables: %d entries, %d differ from the builder" % (len(names), bad))
    if bad:
        sys.exit("mismatch")


def main():
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    table = parse_handler_table(open(os.path.join(src_dir, "m68kops.h")).read())
    jt, cy = build(table)
    check_against_static(src_dir, jt, cy)

    handlers = []
    index = {}
    for name in jt:
        if name not in index:
            index[name] = len(handlers)
            handlers.append(name)
    if len(handlers) >= 2048:
        sys.exit("too many handlers for 11 bits")
    words = []
    for op in range(0x10000):
        c = cy[op]
        if c % (2 * MUL) or c // (2 * MUL) >= 32:
            sys.exit("cycles of opcode %04x (%d) do not fit" % (op, c))
        words.append(index[jt[op]] | ((c // (2 * MUL)) << 11))

    head = ("/* Generated by cores/md/tools/gen_m68k_dispatch.py from m68kops.h's\n"
            " * m68k_opcode_handler_table: do not edit. */\n")
    with open(os.path.join(out_dir, "m68k_dispatch.h"), "w") as f:
        f.write(head)
        f.write("/* Per opcode: handler number (bits 0-10) | cycles / 2 (bits 11-15). */\n")
        f.write("#define M68KI_N_HANDLERS %d\n" % len(handlers))
        f.write("static const unsigned short m68ki_dispatch[0x10000] = {\n")
        for k in range(0, 0x10000, 16):
            f.write("  " + ",".join("0x%04x" % w for w in words[k:k + 16]) + ",\n")
        f.write("};\n")
    with open(os.path.join(out_dir, "m68k_handlers.h"), "w") as f:
        f.write(head)
        f.write("static void (*const m68ki_handlers[M68KI_N_HANDLERS])(void) = {\n")
        for k in range(0, len(handlers), 4):
            f.write("  " + ", ".join(handlers[k:k + 4]) + ",\n")
        f.write("};\n")
    print("%d handlers, tables written to %s" % (len(handlers), out_dir))


if __name__ == "__main__":
    main()
