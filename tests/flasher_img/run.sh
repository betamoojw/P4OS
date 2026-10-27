#!/bin/bash
# The programmer's firmware files (components/aos_flasher/aos_flasher_img.c)
# on the Mac: one firmware - three pieces, two of them a short gap apart -
# written as .bin, Intel HEX, ELF and UF2 by Python, read back by the C code,
# and the regions compared with what they must be. Then the files it has to
# refuse: a bad checksum, overlapping pieces, a UF2 for two chips.
#
#   tests/flasher_img/run.sh
set -e
cd "$(dirname "$0")"
P4=../..
out=$(mktemp -d)
cc -O1 -Wall -Werror -I$P4/components/aos_flasher/include -o "$out/t" main.c $P4/components/aos_flasher/aos_flasher_img.c
cd "$out"
python3 - <<'PY'
import hashlib, random, struct, subprocess, sys
random.seed(7)
A, B, C = 0x08000000, 0x08000C00, 0x08010000
pieces = [(A, random.randbytes(3000)), (B, random.randbytes(1000)), (C, random.randbytes(777))]
FAM = 0xE48BFF56

def expected():
    # A and B are 72 bytes apart: one region, the gap at 0xFF; C on its own
    ab = pieces[0][1] + b"\xff" * (B - A - 3000) + pieces[1][1]
    return [(A, ab), (C, pieces[2][1])]

def hexfile(path):
    lines, upper = [], None
    def rec(t, addr, data):
        b = bytes([len(data), addr >> 8 & 255, addr & 255, t]) + data
        return ":" + (b + bytes([-sum(b) & 255])).hex().upper()
    for base, data in pieces:
        for i in range(0, len(data), 16):
            a = base + i
            if a >> 16 != upper:
                upper = a >> 16
                lines.append(rec(4, 0, struct.pack(">H", upper)))
            lines.append(rec(0, a & 0xFFFF, data[i:i + 16]))
    lines.append(rec(5, 0, struct.pack(">I", 0x080001C1)))
    lines.append(":00000001FF")
    open(path, "w").write("\r\n".join(lines) + "\r\n")

def elffile(path):
    phs, blob, off = [], b"", 52 + 32 * 4
    for base, data in pieces:
        phs.append(struct.pack("<8I", 1, off + len(blob), 0x20000000 + base, base, len(data), len(data), 5, 4))
        blob += data
    phs.append(struct.pack("<8I", 1, 0, 0x20001000, 0x20001000, 0, 4096, 6, 4))     # .bss: nothing to write
    eh = b"\x7fELF" + bytes([1, 1, 1]) + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0x080001C1, 52, 0, 0, 52, 32, 4, 0, 0, 0)
    open(path, "wb").write(eh + b"".join(phs) + blob)

def uf2file(path, fam2=None):
    blocks = []
    for base, data in pieces:
        for i in range(0, len(data), 256):
            blocks.append((base + i, data[i:i + 256]))
    blocks.insert(3, (0x20000000, b"\xaa" * 256))           # not main flash: skipped
    out = b""
    for n, (a, d) in enumerate(blocks):
        flags = 0x2000 | (1 if a == 0x20000000 else 0)
        fam = fam2 if fam2 and n == 5 else FAM
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, flags, a, len(d), n, len(blocks), fam) + d.ljust(476, b"\0") + struct.pack("<I", 0x0AB16F30)
    open(path, "wb").write(out)

def run(*args):
    return subprocess.run(["./t", *args], capture_output=True, text=True).stdout

def check(name, out, regs, fam=0, entry=None):
    lines = out.strip().split("\n")
    want = ["%08x %d %s" % (a, len(d), hashlib.sha256(d).hexdigest()) for a, d in regs]
    got_regs, tail = lines[:-1], lines[-1].split()
    ok = got_regs == want and int(tail[3], 16) == fam and (entry is None or int(tail[5], 16) == entry)
    print("%-10s %s" % (name, "ok" if ok else "FAIL\n" + out))
    return ok

hexfile("fw.hex"); elffile("fw.elf"); uf2file("fw.uf2")
open("fw.bin", "wb").write(pieces[2][1])
good = [check("hex", run("fw.hex"), expected(), entry=0x080001C1),
        check("elf", run("fw.elf"), expected(), entry=0x080001C1),
        check("uf2", run("fw.uf2"), expected(), fam=FAM),
        check("bin", run("fw.bin", "0x08010000"), [(C, pieces[2][1])])]

# what it must refuse
t = open("fw.hex", newline="").read().split("\r\n"); t[3] = t[3][:-2] + ("00" if t[3][-2:] != "00" else "01"); open("bad.hex", "w", newline="").write("\r\n".join(t))
uf2file("two.uf2", fam2=0x68ED2B88)
pieces.append((A + 100, b"x" * 10)); hexfile("over.hex")
for name, f, why in [("checksum", "bad.hex", "checksum"), ("overlap", "over.hex", "same address"), ("2 chips", "two.uf2", "two different chips")]:
    o = run(f)
    ok = o.startswith("ERROR") and why in o
    good.append(ok)
    print("%-10s %s" % (name, "ok (refused)" if ok else "FAIL " + o))
sys.exit(0 if all(good) else 1)
PY
echo "all good"
