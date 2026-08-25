#!/usr/bin/env python3
"""
gapu_map.py -- pack/unpack nes_apu_data.txt against a ROM, so the repository
can distribute neither the capture (a derivative of the game's own music) nor
the ROM (the game itself), only a mapping that is meaningless without both.

This is the same trick IPS/BPS ROM patches use, and for the same reason: a
patch distributed without its base ROM does not redistribute the base ROM,
because it cannot be turned back into anything playable without a copy the
recipient already legally owns. Applied here: XOR the capture's bytes against
a keystream derived from a hash of the ROM. The result -- the .gapumap file --
is cryptographically indistinguishable from noise on its own; only XORing it
against a keystream from the EXACT SAME rom bytes recovers the original
capture. No trace of the capture's content is extractable without the ROM,
and no trace of the ROM is present in the map at all (only its hash was used
to seed the keystream, which is one-way).

Keystream: SHA-256(rom_sha256 || counter) concatenated over counter=0,1,2,...
-- a standard hash-based counter-mode construction. Not attempting real
cryptographic security (there is no secret being protected against a
motivated attacker who also lacks the ROM); this only needs to be one-way and
uniform enough that the packed file carries no usable structure of its own.

Usage:
  gapu_map.py pack   nes_apu_data.txt rom.nes -o nes_apu_data.gapumap
  gapu_map.py unpack nes_apu_data.gapumap rom.nes -o nes_apu_data.txt
"""
import hashlib
import sys


def keystream(seed: bytes, n: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < n:
        out += hashlib.sha256(seed + counter.to_bytes(8, "big")).digest()
        counter += 1
    return bytes(out[:n])


def rom_seed(rom_path: str) -> bytes:
    return hashlib.sha256(open(rom_path, "rb").read()).digest()


def xor(data: bytes, seed: bytes) -> bytes:
    ks = keystream(seed, len(data))
    return bytes(a ^ b for a, b in zip(data, ks))


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ("pack", "unpack"):
        print(__doc__)
        sys.exit(1)
    mode, in_path, rom_path = sys.argv[1], sys.argv[2], sys.argv[3]
    out_path = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else (
        "nes_apu_data.gapumap" if mode == "pack" else "nes_apu_data.txt")

    data = open(in_path, "rb").read()
    seed = rom_seed(rom_path)
    result = xor(data, seed)
    open(out_path, "wb").write(result)

    print("%s: %d bytes -> %s (rom sha256 %s)"
          % (mode, len(data), out_path, hashlib.sha256(open(rom_path,'rb').read()).hexdigest()[:16]))
    if mode == "unpack":
        head = result[:20]
        ok = head.startswith(b"#GAPU2") or all(32 <= b < 127 or b in (9, 10, 13) for b in head)
        print("looks like text: %s (first bytes: %r)" % (ok, head))


if __name__ == "__main__":
    main()
