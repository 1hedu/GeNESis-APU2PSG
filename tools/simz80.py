#!/usr/bin/env python3
"""
simz80.py - executes the assembled driver on a toy Z80 interpreter and checks
that the loops actually emit the waveforms they claim to.

This is not a Z80 emulator.  It implements only the opcodes z80_psgdac.s80
uses, which is the point: it is small enough to trust, and it catches the
things that are genuinely easy to get wrong here -- the sbc a,a / and / add
trick that turns a phase comparison into a PSG volume byte, the duty ratio
that comes out of a given threshold, and the wave-table indexing.

Usage:  python3 tools/simz80.py
"""

import re
import sys

HDR = "psgdac_z80.h"


def load():
    h = open(HDR).read()
    sym = {m.group(1): int(m.group(2), 16)
           for m in re.finditer(r"#define (P_\w+|L_\w+)\s+0x([0-9A-F]+)", h)}
    body = h.split("psgdac_z80")[1]
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2}),", body))
    return blob, sym


class Z80:
    """Just enough Z80.  Memory is 64K; writes outside RAM are logged."""

    def __init__(self, blob):
        self.m = bytearray(0x10000)
        self.m[:len(blob)] = blob
        self.a = 0
        self.f_c = 0
        self.r = {"b": 0, "c": 0, "d": 0, "e": 0, "h": 0, "l": 0}
        self.r2 = {"b": 0, "c": 0, "d": 0, "e": 0, "h": 0, "l": 0}
        self.iy = 0
        self.sp = 0
        self.pc = 0
        self.writes = []
        self.cycles = 0
        self.zflag = False

    def pair(self, hi, lo, regs=None):
        r = regs or self.r
        return (r[hi] << 8) | r[lo]

    def setpair(self, hi, lo, v, regs=None):
        r = regs or self.r
        r[hi] = (v >> 8) & 0xFF
        r[lo] = v & 0xFF

    def w16(self):
        v = self.m[self.pc] | (self.m[self.pc + 1] << 8)
        self.pc += 2
        return v

    def w8(self):
        v = self.m[self.pc]
        self.pc += 1
        return v

    def store(self, addr, val):
        if addr >= 0x4000:          # PSG / YM / bank window: observable side effect
            self.writes.append((addr, val))
        else:
            self.m[addr] = val

    def step(self):
        op = self.m[self.pc]
        self.pc += 1
        c = 4
        if op == 0xF3:      pass                                  # di
        elif op == 0x31:    self.sp = self.w16(); c = 10
        elif op == 0x21:    self.setpair("h", "l", self.w16()); c = 10
        elif op == 0x11:    self.setpair("d", "e", self.w16()); c = 10
        elif op == 0x01:    self.setpair("b", "c", self.w16()); c = 10
        elif op == 0xD9:    self.r, self.r2 = self.r2, self.r; c = 4
        elif op == 0x19:
            v = self.pair("h", "l") + self.pair("d", "e")
            self.setpair("h", "l", v & 0xFFFF); c = 11
        elif op == 0x7C:    self.a = self.r["h"]
        elif op == 0x78:    self.a = self.r["b"]
        elif op == 0xFE:
            n = self.w8(); self.f_c = 1 if self.a < n else 0; c = 7
        elif op == 0x9F:    self.a = 0xFF if self.f_c else 0x00
        elif op == 0xE6:    self.a &= self.w8(); c = 7
        elif op == 0xF6:    self.a |= self.w8(); c = 7
        elif op == 0xC6:
            v = self.a + self.w8(); self.a = v & 0xFF; self.f_c = v >> 8; c = 7
        elif op == 0x3C:    self.a = (self.a + 1) & 0xFF
        elif op == 0x32:    self.store(self.w16(), self.a); c = 13
        elif op == 0x3A:    self.a = self.m[self.w16()]; c = 13
        elif op == 0x22:
            a = self.w16()
            self.m[a] = self.r["l"]; self.m[a + 1] = self.r["h"]; c = 16
        elif op == 0x2A:
            a = self.w16()
            self.r["l"] = self.m[a]; self.r["h"] = self.m[a + 1]; c = 16
        elif op == 0x7D:    self.a = self.r["l"]
        elif op == 0x16:    self.r["d"] = self.w8(); c = 7
        elif op == 0x5F:    self.r["e"] = self.a
        elif op == 0x47:    self.r["b"] = self.a
        elif op == 0x1A:    self.a = self.m[self.pair("d", "e")]; c = 7
        elif op == 0x0A:    self.a = self.m[self.pair("b", "c")]; c = 7
        elif op == 0x0C:
            self.r["c"] = (self.r["c"] + 1) & 0xFF; self.zflag = self.r["c"] == 0
        elif op == 0x2C:    self.r["l"] = (self.r["l"] + 1) & 0xFF
        elif op == 0x7E:    self.a = self.m[self.pair("h", "l")]; c = 7
        elif op == 0x56:    self.r["d"] = self.m[self.pair("h", "l")]; c = 7
        elif op == 0x5E:    self.r["e"] = self.m[self.pair("h", "l")]; c = 7
        elif op == 0x12:    self.store(self.pair("d", "e"), self.a); c = 7
        elif op == 0xBD:
            self.f_c = 1 if self.a < self.r["l"] else 0
            self.zflag = (self.a == self.r["l"])
        elif op == 0xE5:
            self.sp = (self.sp - 2) & 0xFFFF
            self.m[self.sp] = self.r["l"]; self.m[self.sp + 1] = self.r["h"]; c = 11
        elif op == 0xE1:
            self.r["l"] = self.m[self.sp]; self.r["h"] = self.m[self.sp + 1]
            self.sp = (self.sp + 2) & 0xFFFF; c = 10
        elif op == 0x36:
            self.store(self.pair("h", "l"), self.w8()); c = 10
        elif op == 0xC3:    self.pc = self.w16(); c = 10
        elif op == 0x28 or op == 0x20:
            # The driver uses jr z / jr nz after exactly two things: "inc c",
            # where Z means the PCM cursor wrapped a page, and "cp l", where Z
            # means the command queue has drained. Track which was last.
            e = self.w8()
            z = self.zflag
            take = (z if op == 0x28 else not z)
            c = 7
            if take:
                self.pc = (self.pc + (e - 256 if e > 127 else e)) & 0xFFFF
                c = 12
        elif op == 0xED:
            self.pc += 1; c = 8                                   # im 1
        elif op == 0xFD:
            sub = self.m[self.pc]; self.pc += 1
            if sub == 0x21:   self.iy = self.w16(); c = 14
            elif sub == 0x09:
                self.iy = (self.iy + self.pair("b", "c")) & 0xFFFF; c = 15
            elif sub == 0x22:
                a = self.w16()
                self.m[a] = self.iy & 0xFF
                self.m[a + 1] = self.iy >> 8
                c = 20
            else: raise NotImplementedError("FD %02X" % sub)
        else:
            raise NotImplementedError("opcode %02X at %04X" % (op, self.pc - 1))
        self.cycles += c


def patch16(cpu, addr, v):
    cpu.m[addr] = v & 0xFF
    cpu.m[addr + 1] = (v >> 8) & 0xFF


def run(blob, sym, variant, patches, waves=None, samples=64):
    cpu = Z80(blob)
    if waves:
        cpu.m[0x0400:0x0500] = bytes(waves)
    for k, v in patches.items():
        if isinstance(v, tuple):
            patch16(cpu, sym[k] + v[0], v[1])
        else:
            cpu.m[sym[k] + 1] = v
    for n in ("P_entry", "P_svc", "P_svcout"):
        patch16(cpu, sym[n] + 1, sym[variant])
    while cpu.pc != sym[variant]:                 # run the init preamble
        cpu.step()
    out, start = [], sym[variant]
    for _ in range(samples):
        cpu.writes.clear()
        cpu.cycles = 0
        cpu.step()
        while cpu.pc != start:
            cpu.step()
        out.append((list(cpu.writes), cpu.cycles))
    return out


def att_to_span(att):
    """The byte the 68000 pokes into the 'and' so mute+span == 0x90|attenuation."""
    return (att - 0x0F) & 0xFF


def main():
    blob, sym = load()
    fail = 0

    # ---- pulse duty ----------------------------------------------------
    for duty_name, thresh, want in (("12.5%", 32, 0.125),
                                    ("25%", 64, 0.25),
                                    ("50%", 128, 0.5),
                                    ("75%", 192, 0.75)):
        # delta of 256 walks the phase high byte one step per sample
        res = run(blob, sym, "L_v3", {
            "P_v3_a_duty": thresh, "P_v3_a_span": att_to_span(2),
            "P_v3_a_out": (1, 0x7F11), "P_v3_b_out": (1, 0x0304),
            "P_v3_c_out": (1, 0x0304), "P_v3_a_delta": (1, 256),
        }, samples=256)
        vals = [w[0][1] for w, _ in res]
        loud = sum(1 for v in vals if v == 0x92)
        mute = sum(1 for v in vals if v == 0x9F)
        ok = loud == want * 256 and loud + mute == 256
        fail += not ok
        print("%s duty %-6s loud=%3d mute=%3d  (want %d loud)"
              % ("OK " if ok else "BAD", duty_name, loud, mute, want * 256))

    # ---- attenuation encoding ------------------------------------------
    bad = []
    for att in range(16):
        res = run(blob, sym, "L_v3", {
            "P_v3_a_duty": 255, "P_v3_a_span": att_to_span(att),
            "P_v3_a_out": (1, 0x7F11), "P_v3_b_out": (1, 0x0304),
            "P_v3_c_out": (1, 0x0304), "P_v3_a_delta": (1, 0),
        }, samples=1)
        got = res[0][0][0][1]
        if got != 0x90 | att:
            bad.append((att, got))
    ok = not bad
    fail += not ok
    print("%s ch0 attenuation 0..15 encode to 0x90|att%s"
          % ("OK " if ok else "BAD", "" if ok else " -- %r" % bad))

    # ---- channel B lands on channel 1 ----------------------------------
    res = run(blob, sym, "L_v3", {
        "P_v3_b_duty": 255, "P_v3_b_span": att_to_span(5),
        "P_v3_b_out": (1, 0x7F11), "P_v3_a_out": (1, 0x0304),
        "P_v3_c_out": (1, 0x0304), "P_v3_b_delta": (1, 0),
    }, samples=1)
    got = res[0][0][0][1]
    ok = got == 0xB5
    fail += not ok
    print("%s ch1 attenuation 5 -> 0x%02X (want 0xB5)" % ("OK " if ok else "BAD", got))

    # ---- wave voice walks the table ------------------------------------
    tri = []
    for i in range(256):
        step = i >> 3                       # 32-step NES triangle staircase
        lvl = step if step < 16 else 31 - step
        tri.append(0xD0 | (15 - lvl))
    res = run(blob, sym, "L_v3", {
        "P_v3_c_delta": (1, 0x0800), "P_v3_c_page": 0x04,
        "P_v3_c_out": (1, 0x7F11), "P_v3_a_out": (1, 0x0304),
        "P_v3_b_out": (1, 0x0304),
    }, waves=tri, samples=32)
    got = [w[0][1] for w, _ in res]
    want = [tri[(i * 0x0800 >> 8) & 0xFF] for i in range(1, 33)]
    ok = got == want
    fail += not ok
    print("%s wave voice walks all 32 staircase steps in order" % ("OK " if ok else "BAD"))

    # ---- a disabled voice still costs the same ------------------------
    on = run(blob, sym, "L_v3", {"P_v3_a_out": (1, 0x7F11),
                                 "P_v3_b_out": (1, 0x7F11),
                                 "P_v3_c_out": (1, 0x7F11)}, samples=4)
    off = run(blob, sym, "L_v3", {"P_v3_a_out": (1, 0x0304),
                                  "P_v3_b_out": (1, 0x0304),
                                  "P_v3_c_out": (1, 0x0304)}, samples=4)
    ok = on[0][1] == off[0][1] == 243
    fail += not ok
    print("%s V3 loop is %d cycles enabled, %d disabled (want 243 both)"
          % ("OK " if ok else "BAD", on[0][1], off[0][1]))

    for name, want in (("L_v2", 154), ("L_v2d", 185)):
        r = run(blob, sym, name, {}, samples=4)
        got = r[1][1]
        ok = got == want
        fail += not ok
        print("%s %s loop is %d cycles (want %d) -> %.0f Hz"
              % ("OK " if ok else "BAD", name, got, want, 3579545.0 / got))

    # ---- PCM ring wraps inside 0x9000..0x9FFF --------------------------
    cpu = Z80(blob)
    for n in ("P_entry", "P_svc", "P_svcout"):
        patch16(cpu, sym[n] + 1, sym["L_v2d"])
    patch16(cpu, sym["P_v2d_d_out"] + 1, 0x4001)
    patch16(cpu, sym["P_v2d_a_out"] + 1, 0x0304)
    patch16(cpu, sym["P_v2d_b_out"] + 1, 0x0304)
    while cpu.pc != sym["L_v2d"]:
        cpu.step()
    seen = set()
    for _ in range(5000 * 40):
        cpu.step()
        if cpu.pc == sym["L_v2d"]:
            seen.add((cpu.r["b"] << 8) | cpu.r["c"])
            if len(seen) > 4200:
                break
    lo, hi = min(seen), max(seen)
    ok = lo >= 0x9000 and hi <= 0x9FFF
    fail += not ok
    print("%s PCM cursor stayed in %04X..%04X (ring is 9000..9FFF)"
          % ("OK " if ok else "BAD", lo, hi))

    # ---- the command queue ---------------------------------------------
    # This is the whole reason the 68000 no longer touches a sound chip, so it
    # gets checked the same way everything else here does: by running it.
    QBASE = 0x0300
    cmds = [(0x7F11, 0x8E), (0x7F11, 0x3F),        # a PSG tone period, latch+data
            (0x4000, 0x28), (0x4001, 0xF5),        # a YM2612 register, addr+data
            (0x4000, 0x2A),                        # ...and the DAC re-latch
            (0x7F11, 0xDF)]                        # silence PSG channel 2

    cpu = Z80(blob)
    for n in ("P_entry", "P_svc", "P_svcout"):
        patch16(cpu, sym[n] + 1, sym["L_v3"])
    patch16(cpu, sym["P_v3_a_out"] + 1, 0x7F11)    # a live DAC voice, to be sure
    patch16(cpu, sym["P_v3_a_delta"] + 1, 0x0800)  # ...its phase must survive
    patch16(cpu, sym["P_v3_b_out"] + 1, 0x0204)
    patch16(cpu, sym["P_v3_c_out"] + 1, 0x0204)
    while cpu.pc != sym["L_v3"]:
        cpu.step()

    # post the frame's writes exactly as PSGDAC_flush does
    q = QBASE & 0xFF
    for a, d in cmds:
        cpu.m[QBASE | q] = a & 0xFF;  q = (q + 1) & 0xFF
        cpu.m[QBASE | q] = a >> 8;    q = (q + 1) & 0xFF
        cpu.m[QBASE | q] = d;         q = (q + 1) & 0xFF
    cpu.m[0x0208] = q                              # qend
    patch16(cpu, sym["P_svc"] + 1, sym["L_svcrun"])

    hl_before = cpu.pair("h", "l")
    seen, per_sample = [], []
    for _ in range(len(cmds) + 4):
        cpu.writes.clear()
        cpu.step()
        while cpu.pc != sym["L_v3"]:
            cpu.step()
        extra = [w for w in cpu.writes if w != (0x7F11, cpu.writes[0][1])]
        per_sample.append(len(cpu.writes) - 1)     # minus the DAC voice's own write
        seen.extend(cpu.writes[1:])

    ok = seen == cmds
    fail += not ok
    print("%s queue replayed %d/%d commands in order%s"
          % ("OK " if ok else "BAD", len(seen), len(cmds),
             "" if ok else " -- got %r" % (seen,)))

    ok = all(n <= 1 for n in per_sample)
    fail += not ok
    print("%s never more than one queued command per sample (max %d)"
          % ("OK " if ok else "BAD", max(per_sample)))

    drained = (cpu.m[sym["P_svc"] + 1] | (cpu.m[sym["P_svc"] + 2] << 8))
    ok = drained == sym["L_v3"]
    fail += not ok
    print("%s service slot switched itself back off when drained (-> %04X)"
          % ("OK " if ok else "BAD", drained))

    # HL carries pulse A's phase across the service routine's push/pop
    advanced = (hl_before + 0x0800 * (len(cmds) + 4)) & 0xFFFF
    ok = cpu.pair("h", "l") == advanced
    fail += not ok
    print("%s pulse A's phase survived the service routine (%04X, want %04X)"
          % ("OK " if ok else "BAD", cpu.pair("h", "l"), advanced))

    print("\n%d check(s) failed" % fail)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
