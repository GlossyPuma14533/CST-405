#!/usr/bin/env python3
"""
mipscount.py — run a .s file produced by OUR compiler and count every MIPS
instruction it executes.

SPIM runs the program but cannot say how much work it did.  For the Topic 4
benchmark (optimized vs unoptimized) we want a number that does not depend on
the speed of the machine, so this script executes the same assembly and
counts:

    instructions   every instruction executed (the headline number)
    loads/stores   lw / sw executed (memory traffic, i.e. spills and reloads)
    branches       beq / bne / j / jal / jr executed

It understands only the instruction subset codegen.c emits (li la lw sw move
add addi sub mul div sll slt sgt sle sge seq sne beq bne blt bgt ble bge j jal
jr syscall)
and the directives .data .text .globl .word .space .asciiz .align.  Anything
else is reported as an error rather than silently ignored.  Its printed
output is compared against SPIM's by `make bench`, so a disagreement between
the two would show up immediately.

usage:  python3 bench/mipscount.py program.s [--quiet]
"""
import re
import sys

MASK = 0xFFFFFFFF
REG_NAMES = (["$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3"]
             + ["$t%d" % i for i in range(8)] + ["$s%d" % i for i in range(8)]
             + ["$t8", "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"])
REG = {n: i for i, n in enumerate(REG_NAMES)}
DATA_BASE, STACK_TOP = 0x10010000, 0x7FFFEFFC


def s32(v):
    v &= MASK
    return v - (1 << 32) if v & 0x80000000 else v


def parse(path):
    """Return (instructions, label->instruction index, data labels, memory)."""
    text, labels, data, mem = [], {}, {}, {}
    addr, section = DATA_BASE, ".text"
    for raw in open(path):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^([A-Za-z_.$][\w.$]*):\s*(.*)$", line)
        if m:
            name, line = m.group(1), m.group(2).strip()
            if section == ".data":
                data[name] = addr
            else:
                labels[name] = len(text)
            if not line:
                continue
        if line in (".data", ".text"):
            section = line
            continue
        if line.startswith(".globl"):
            continue
        if line.startswith(".align"):
            n = 1 << int(line.split()[1])
            addr = (addr + n - 1) // n * n
            continue
        if line.startswith(".word"):
            for w in line[5:].split(","):
                mem[addr] = int(w.strip() or 0) & MASK
                addr += 4
            continue
        if line.startswith(".space"):
            addr += int(line.split()[1])
            continue
        if line.startswith(".asciiz"):
            s = bytes(line[line.index('"') + 1:line.rindex('"')], "utf-8").decode("unicode_escape")
            for ch in s + "\0":
                mem[("b", addr)] = ord(ch)
                addr += 1
            continue
        if section != ".text":
            raise SystemExit("mipscount: unsupported data line: " + line)
        op, _, rest = line.partition(" ")
        args = [a.strip() for a in rest.split(",")] if rest.strip() else []
        text.append((op, args, raw.rstrip()))
    return text, labels, data, mem


def run(path, quiet=False):
    text, labels, data, mem = parse(path)
    r = [0] * 32
    r[REG["$sp"]] = STACK_TOP
    hi = lo = 0
    pc = labels["main"]
    out = []
    count = {"instructions": 0, "loads": 0, "stores": 0, "branches": 0}

    def reg(n):
        return r[REG[n]]

    def setr(n, v):
        if n != "$zero":
            r[REG[n]] = v & MASK

    def address(operand):
        m = re.match(r"^(-?\d*)\((\$\w+)\)$", operand)
        if m:
            return (int(m.group(1) or 0) + reg(m.group(2))) & MASK
        return data[operand]

    def val(x):
        return reg(x) if x.startswith("$") else int(x, 0)

    while True:
        if pc >= len(text):
            raise SystemExit("mipscount: fell off the end of the program")
        op, a, src = text[pc]
        pc += 1
        count["instructions"] += 1
        if op == "li":
            setr(a[0], int(a[1], 0))
        elif op == "la":
            setr(a[0], address(a[1]))
        elif op == "move":
            setr(a[0], reg(a[1]))
        elif op == "lw":
            count["loads"] += 1
            setr(a[0], mem.get(address(a[1]), 0))
        elif op == "sw":
            count["stores"] += 1
            mem[address(a[1])] = reg(a[0])
        elif op in ("add", "addu", "addi", "addiu"):
            setr(a[0], reg(a[1]) + val(a[2]))
        elif op in ("sub", "subu"):
            setr(a[0], reg(a[1]) - val(a[2]))
        elif op == "mul":
            setr(a[0], s32(reg(a[1])) * s32(val(a[2])))
        elif op == "div" and len(a) == 3:
            d = s32(val(a[2]))
            if d == 0:
                raise SystemExit("mipscount: division by zero")
            q = abs(s32(reg(a[1]))) // abs(d)
            setr(a[0], q if (s32(reg(a[1])) < 0) == (d < 0) else -q)
        elif op == "div":                                   # div rs, rt -> lo/hi
            x, d = s32(reg(a[0])), s32(reg(a[1]))
            q = abs(x) // abs(d)
            lo = q if (x < 0) == (d < 0) else -q
            hi = x - lo * d
        elif op == "mflo":
            setr(a[0], lo)
        elif op == "mfhi":
            setr(a[0], hi)
        elif op == "neg":
            setr(a[0], -s32(reg(a[1])))
        elif op == "sll":
            setr(a[0], reg(a[1]) << int(a[2]))
        elif op in ("slt", "sgt", "sle", "sge", "seq", "sne"):
            x, y = s32(reg(a[1])), s32(val(a[2]))
            res = {"slt": x < y, "sgt": x > y, "sle": x <= y,
                   "sge": x >= y, "seq": x == y, "sne": x != y}[op]
            setr(a[0], int(res))
        elif op in ("beq", "bne", "blt", "bgt", "ble", "bge"):
            count["branches"] += 1
            x, y = s32(reg(a[0])), s32(val(a[1]))
            taken = {"beq": x == y, "bne": x != y, "blt": x < y,
                     "bgt": x > y, "ble": x <= y, "bge": x >= y}[op]
            if taken:
                pc = labels[a[2]]
        elif op == "j":
            count["branches"] += 1
            pc = labels[a[0]]
        elif op == "jal":
            count["branches"] += 1
            setr("$ra", pc)
            pc = labels[a[0]]
        elif op == "jr":
            count["branches"] += 1
            pc = reg(a[0])
        elif op == "syscall":
            code = reg("$v0")
            if code == 1:
                out.append(str(s32(reg("$a0"))))
            elif code == 4:
                p = reg("$a0")
                while mem.get(("b", p), 0):
                    out.append(chr(mem[("b", p)]))
                    p += 1
            elif code == 10:
                break
            else:
                raise SystemExit("mipscount: unsupported syscall %d" % code)
        else:
            raise SystemExit("mipscount: unsupported instruction: " + src.strip())
    program_output = "".join(out)
    if not quiet:
        sys.stdout.write(program_output)
    return count, program_output


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    c, _ = run(sys.argv[1], quiet="--quiet" in sys.argv)
    print("instructions=%d loads=%d stores=%d branches=%d"
          % (c["instructions"], c["loads"], c["stores"], c["branches"]))
