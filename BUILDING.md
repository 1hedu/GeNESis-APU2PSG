# Building

The ROM is checked in as `rom.bin`, already built from the current source. You
only need this file if you want to change the code.

## The normal way — SGDK

Put `GeNESis-APU2PSG.c`, `psgdac.h` and `psgdac_z80.h` in an SGDK project's
`src/` folder and build as usual:

```
make -f $GDK/makefile.gen
```

There are no resources, so `res/` can be empty. `psgdac_z80.h` is generated but
checked in, so the Z80 assembler is not part of the build.

Requires a reasonably current SGDK — the driver calls `Z80_unloadDriver()`,
which replaced the older `Z80_loadDriver(Z80_DRIVER_NULL, ...)`.

## How the checked-in rom.bin was built

Without SGDK's own `m68k-elf` toolchain, using Ubuntu's m68k cross compiler.
**The library is rebuilt with the same compiler as the ROM** — mixing SGDK's
prebuilt `libmd.a` with a different GCC means mismatched LTO bytecode and two
toolchains' ABIs in one binary. With the library rebuilt, SGDK's stock makefile
works unmodified, LTO and all.

```sh
apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu \
                libgmp-dev libmpfr-dev libmpc-dev default-jre
git clone --depth 1 https://github.com/Stephane-D/SGDK.git
gcc -O2 -o /tmp/bintos SGDK/tools/bintos/src/bintos.c     # ships with SGDK
```

The library build needs `sjasm` for seven `.s80` Z80 drivers. Those assemble to
pure data, so the blobs already inside the shipped library are reused verbatim;
`tools/sgdk_fake_sjasm.sh` extracts them.

```sh
cd SGDK
make -f makelib.gen PREFIX=m68k-linux-gnu- \
     ASMZ80=../tools/sgdk_fake_sjasm.sh BINTOS=/tmp/bintos
```

Then build the ROM with the stock makefile — **and link SGDK's shipped
`lib/libgcc.a`, not the cross compiler's own** (`LIBGCC=SGDK/lib/libgcc.a` on
the make command line). Ubuntu's `m68k-linux-gnu` libgcc targets Linux, which
assumes a 68020+, so its division helpers open with `bsr.l` (`0x61FF`) — an
instruction the 68000 does not have.

Ubuntu's cross-GCC also defaults to emitting a build-id note, which on a
bare-metal link lands at address 0 and collides with `.text`; a wrapper adds
`-Wl,--build-id=none` at link time only. An `m68k-elf` toolchain needs neither
the wrapper nor any of the above.

```sh
make -f SGDK/makefile.gen GDK=SGDK PREFIX=/path/to/wrapper/m68k-linux-gnu-
```

Finally set the header checksum, which `sizebnd`'s `-checksum` did not write here:

```python
d = bytearray(open('rom.bin','rb').read())
s = 0
for i in range(0x200, len(d), 2): s = (s + int.from_bytes(d[i:i+2],'big')) & 0xFFFF
d[0x18E:0x190] = s.to_bytes(2,'big')
open('rom.bin','wb').write(d)
```

## Headless testing against a real emulator core

`tools/lrhost.c` is a minimal libretro host: it loads an emulator core (.so),
runs the ROM for N frames with no window, reports per-frame audio energy, and
dumps chosen frames as PPM images so the on-screen overlay can be read from a
capture.

```
gcc -O2 -o lrhost tools/lrhost.c -ldl
./lrhost picodrive_libretro.so rom.bin 600 300 590   # 600 frames, dump 300 & 590
```

## Rebuilding the Z80 driver

Only needed if you edit `z80_psgdac.s80`:

```
python3 tools/asmz80.py z80_psgdac.s80 -o psgdac_z80.h
python3 tools/simz80.py
```


## Reproducing the exact shipped capture: gapu_map.py

`tools/gapu_map.py` gets back the *exact* capture -- byte for byte, not a
fresh recording -- without distributing it. It works the same way an IPS or
BPS ROM patch does, and for the same reason a patch distributed without its
base ROM does not redistribute the base ROM: the distributed file is
meaningless on its own.

The capture's bytes are XORed against a keystream derived from a SHA-256 hash
of the ROM (`SHA256(rom_sha256 || counter)`, concatenated over counter =
0,1,2,... -- a standard hash-based counter-mode construction). The packed
file is indistinguishable from noise on its own; only a keystream from the
exact same ROM bytes recovers the capture.

```
python3 tools/gapu_map.py pack   nes_apu_data.txt game.nes -o nes_apu_data.gapumap
python3 tools/gapu_map.py unpack nes_apu_data.gapumap game.nes -o nes_apu_data.txt
```

This is the primary path for reproducing what this repo has always shipped.
`gen_apu_capture.js` below is for making a *different* capture -- a new game,
a longer playthrough, deeper into a level -- where exact reproduction isn't
the goal and there is no original to reconstruct.

## Regenerating nes_apu_data.txt from your own ROM

`gen_apu_capture.js` records a capture from a NES ROM by running it headless
and logging every APU register write. A button-input schedule (a timed
sequence of controller presses, which holds no game content of its own)
drives a specific stretch of gameplay.

```
cd tools
npm install
node gen_apu_capture.js path/to/game.nes schedules/ducktales.txt 8697 -o ../nes_apu_data.txt
```

Uses [jsnes](https://github.com/jsnes/jsnes), a pure-JavaScript NES core with
every APU register write visible as a plain JS call (`nes.papu.writeReg()`),
so no emulator GUI or memory-map API is involved.

The output is `GeNESis-APU2PSG-Recorder.lua`'s v2 format (`#GAPU2 v2` header,
26 comma-separated fields a line): post-envelope volumes from
`channel.masterVolume`, channel on/off from `channel.getLengthStatus()`, and
DMC triggers latched at the `$4015` write. `tools/gen_apudata.py` and
`tools/gen_dpcm.py` consume it unmodified.

The included `schedules/ducktales.txt` stays near the title screen; swap in a
longer-timed schedule to capture further into the game.
