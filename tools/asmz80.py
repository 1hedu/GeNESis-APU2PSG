#!/usr/bin/env python3
"""
asmz80.py - a deliberately tiny, dependency-free Z80 assembler.

It supports exactly the instruction subset used by z80_psgdac.s80, and nothing
else.  The point is that the driver blob committed to this repository can be
regenerated and audited by anyone with a stock Python 3, without installing
sjasm/pasmo or wiring a new rule into the SGDK makefile.

Beyond emitting bytes it also emits:
  * a C header with the blob and a #define for every label, and
  * a cycle count for every  ;@cycles <from> <to>  request in the source,
    because the driver's sample rate *is* its loop length.

Usage:  python3 tools/asmz80.py z80_psgdac.s80 -o psgdac_z80.h
"""

import re
import sys

# mnemonic pattern -> (opcode bytes, operand kind, cycles)
#   operand kind: None | 'n' (8-bit) | 'nn' (16-bit little endian) | 'e' (rel)
TABLE = [
    (r"^di$",                 (0xF3,),          None, 4),
    (r"^ei$",                 (0xFB,),          None, 4),
    (r"^im\s+1$",             (0xED, 0x56),     None, 8),
    (r"^nop$",                (0x00,),          None, 4),
    (r"^halt$",               (0x76,),          None, 4),
    (r"^exx$",                (0xD9,),          None, 4),
    (r"^sbc\s+a,a$",          (0x9F,),          None, 4),

    (r"^ld\s+sp,(.+)$",       (0x31,),          'nn', 10),
    (r"^ld\s+bc,(.+)$",       (0x01,),          'nn', 10),
    (r"^ld\s+de,(.+)$",       (0x11,),          'nn', 10),
    (r"^ld\s+hl,\((.+)\)$",   (0x2A,),          'nn', 16),
    (r"^ld\s+hl,(.+)$",       (0x21,),          'nn', 10),
    (r"^ld\s+ix,(.+)$",       (0xDD, 0x21),     'nn', 14),
    (r"^ld\s+iy,(.+)$",       (0xFD, 0x21),     'nn', 14),
    (r"^ld\s+\((.+)\),hl$",   (0x22,),          'nn', 16),
    (r"^ld\s+\((.+)\),iy$",   (0xFD, 0x22),     'nn', 20),
    (r"^ld\s+\((.+)\),ix$",   (0xDD, 0x22),     'nn', 20),
    (r"^ld\s+a,\(hl\)$",      (0x7E,),          None, 7),
    (r"^ld\s+d,\(hl\)$",      (0x56,),          None, 7),
    (r"^ld\s+e,\(hl\)$",      (0x5E,),          None, 7),
    (r"^ld\s+\(de\),a$",      (0x12,),          None, 7),
    (r"^ld\s+\(bc\),a$",      (0x02,),          None, 7),
    (r"^ld\s+a,\(bc\)$",      (0x0A,),          None, 7),
    (r"^ld\s+a,\(de\)$",      (0x1A,),          None, 7),
    (r"^ld\s+a,\((.+)\)$",    (0x3A,),          'nn', 13),
    (r"^ld\s+\((.+)\),a$",    (0x32,),          'nn', 13),

    (r"^ld\s+\(hl\),(.+)$",   (0x36,),          'n',  10),

    (r"^ld\s+a,b$",           (0x78,),          None, 4),
    (r"^ld\s+a,h$",           (0x7C,),          None, 4),
    (r"^ld\s+a,l$",           (0x7D,),          None, 4),
    (r"^ld\s+b,a$",           (0x47,),          None, 4),
    (r"^ld\s+e,a$",           (0x5F,),          None, 4),
    (r"^ld\s+l,a$",           (0x6F,),          None, 4),
    (r"^ld\s+h,a$",           (0x67,),          None, 4),
    (r"^ld\s+a,(.+)$",        (0x3E,),          'n',  7),
    (r"^ld\s+b,(.+)$",        (0x06,),          'n',  7),
    (r"^ld\s+c,(.+)$",        (0x0E,),          'n',  7),
    (r"^ld\s+d,(.+)$",        (0x16,),          'n',  7),
    (r"^ld\s+e,(.+)$",        (0x1E,),          'n',  7),
    (r"^ld\s+h,(.+)$",        (0x26,),          'n',  7),
    (r"^ld\s+l,(.+)$",        (0x2E,),          'n',  7),

    (r"^add\s+hl,bc$",        (0x09,),          None, 11),
    (r"^add\s+hl,de$",        (0x19,),          None, 11),
    (r"^add\s+iy,bc$",        (0xFD, 0x09),     None, 15),
    (r"^add\s+ix,bc$",        (0xDD, 0x09),     None, 15),
    (r"^add\s+a,(.+)$",       (0xC6,),          'n',  7),
    (r"^and\s+(.+)$",         (0xE6,),          'n',  7),
    (r"^or\s+(.+)$",          (0xF6,),          'n',  7),
    (r"^xor\s+(.+)$",         (0xEE,),          'n',  7),
    (r"^cp\s+l$",             (0xBD,),          None, 4),
    (r"^cp\s+h$",             (0xBC,),          None, 4),
    (r"^cp\s+(.+)$",          (0xFE,),          'n',  7),
    (r"^inc\s+a$",            (0x3C,),          None, 4),
    (r"^inc\s+b$",            (0x04,),          None, 4),
    (r"^inc\s+c$",            (0x0C,),          None, 4),
    (r"^inc\s+l$",            (0x2C,),          None, 4),
    (r"^inc\s+bc$",           (0x03,),          None, 6),
    (r"^push\s+hl$",          (0xE5,),          None, 11),
    (r"^pop\s+hl$",           (0xE1,),          None, 10),

    (r"^jp\s+(.+)$",          (0xC3,),          'nn', 10),
    # conditional jr is costed at its NOT-taken price (7); every conditional
    # branch in the driver is arranged so that not-taken is the common path.
    # A taken branch costs 5 more.
    (r"^jr\s+nz,(.+)$",       (0x20,),          'e',  7),
    (r"^jr\s+z,(.+)$",        (0x28,),          'e',  7),
    (r"^jr\s+(.+)$",          (0x18,),          'e',  12),
]
TABLE = [(re.compile(p, re.I), b, k, c) for (p, b, k, c) in TABLE]


class AsmError(Exception):
    pass


def parse_num(tok, syms, strict):
    """Evaluate an expression over labels, decimal, 0x/h hex and +/- ."""
    tok = tok.strip()
    total, sign, i = 0, 1, 0
    for part in re.split(r"([+\-])", tok):
        part = part.strip()
        if part == "+":
            sign = 1
            continue
        if part == "-":
            sign = -1
            continue
        if not part:
            continue
        if re.fullmatch(r"[0-9][0-9a-fA-F]*[hH]", part):
            v = int(part[:-1], 16)
        elif re.fullmatch(r"0[xX][0-9a-fA-F]+", part):
            v = int(part, 16)
        elif re.fullmatch(r"[0-9]+", part):
            v = int(part)
        elif part in syms:
            v = syms[part]
        elif strict:
            raise AsmError("unknown symbol %r" % part)
        else:
            v = 0
        total += sign * v
        sign = 1
        i += 1
    return total


def assemble(text):
    lines = []
    for raw in text.splitlines():
        line = raw.split(";")[0].rstrip()
        if not line.strip():
            continue
        label = None
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$", line)
        if m:
            label, line = m.group(1), m.group(2)
        elif not line[0].isspace():
            # "name equ value" form
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+(equ\s+.*)$", line, re.I)
            if not m:
                raise AsmError("cannot parse %r" % raw)
            label, line = m.group(1), m.group(2)
        lines.append((label, line.strip(), raw))

    syms, out, cycles = {}, bytearray(), {}
    for strict in (False, True):
        syms_pass = dict(syms)
        pc, out, cycles = 0, bytearray(), {}
        for label, ins, raw in lines:
            low = ins.lower()
            if low.startswith("equ"):
                syms_pass[label] = parse_num(ins[3:], syms_pass, strict)
                continue
            if label:
                syms_pass[label] = pc
            if not ins:
                continue
            if low.startswith("org"):
                new = parse_num(ins[3:], syms_pass, strict)
                if new < pc:
                    raise AsmError("org moves backwards at %r" % raw)
                out.extend(b"\x00" * (new - pc))
                pc = new
                continue
            if low.startswith("ds"):
                n = parse_num(ins[2:], syms_pass, strict)
                out.extend(b"\x00" * n)
                pc += n
                continue
            if low.startswith("db"):
                for tok in ins[2:].split(","):
                    out.append(parse_num(tok, syms_pass, strict) & 0xFF)
                    pc += 1
                continue
            for pat, opbytes, kind, cyc in TABLE:
                m = pat.match(ins)
                if not m:
                    continue
                cycles[pc] = cyc
                out.extend(opbytes)
                pc += len(opbytes)
                if kind:
                    v = parse_num(m.group(1), syms_pass, strict)
                    if kind == "n":
                        out.append(v & 0xFF)
                        pc += 1
                    elif kind == "nn":
                        out.append(v & 0xFF)
                        out.append((v >> 8) & 0xFF)
                        pc += 2
                    else:  # 'e'
                        rel = v - (pc + 1)
                        if strict and not (-128 <= rel <= 127):
                            raise AsmError("jr out of range at %r" % raw)
                        out.append(rel & 0xFF)
                        pc += 1
                break
            else:
                raise AsmError("unsupported instruction %r" % raw)
        syms = syms_pass
    return bytes(out), syms, cycles


def cycle_span(cycles, lo, hi):
    return sum(c for pc, c in cycles.items() if lo <= pc < hi)


def main():
    src = sys.argv[1]
    out = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else None
    text = open(src).read()
    blob, syms, cycles = assemble(text)

    spans = []
    for m in re.finditer(r";@cycles\s+(\S+)\s+(\S+)", text):
        a, b = m.group(1), m.group(2)
        spans.append((a, b, cycle_span(cycles, syms[a], syms[b])))

    exported = sorted((k, v) for k, v in syms.items() if k.startswith("P_") or k.startswith("L_"))

    lines = []
    lines.append("/* Generated by tools/asmz80.py from %s - do not edit by hand. */" % src)
    lines.append("#ifndef _PSGDAC_Z80_H_")
    lines.append("#define _PSGDAC_Z80_H_")
    lines.append("")
    lines.append("#define PSGDAC_Z80_SIZE %d" % len(blob))
    lines.append("")
    for k, v in exported:
        lines.append("#define %-18s 0x%04X" % (k, v))
    lines.append("")
    for a, b, c in spans:
        lines.append("#define CYCLES_%-11s %d" % (a.replace("L_", ""), c))
    lines.append("")
    lines.append("static const u8 psgdac_z80[PSGDAC_Z80_SIZE] = {")
    for i in range(0, len(blob), 12):
        lines.append("    " + " ".join("0x%02X," % b for b in blob[i:i + 12]))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    body = "\n".join(lines) + "\n"

    if out:
        open(out, "w").write(body)
    print("%s: %d bytes" % (src, len(blob)))
    for a, b, c in spans:
        print("  %-10s %3d cycles/sample -> %.1f Hz" % (a, c, 3579545.0 / c))


if __name__ == "__main__":
    main()
