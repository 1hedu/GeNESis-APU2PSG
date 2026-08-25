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

Without SGDK's own `m68k-elf` toolchain, using Ubuntu's m68k cross compiler. The
important part is that **the library is rebuilt with the same compiler as the
ROM** — mixing SGDK's prebuilt `libmd.a` (crosstool-NG GCC 13.2) with a different
GCC means mismatched LTO bytecode and two toolchains' ABIs in one binary.
Rebuilding it makes both halves match by construction, and then SGDK's stock
makefile works unmodified, LTO and all.

```sh
apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu \
                libgmp-dev libmpfr-dev libmpc-dev default-jre
git clone --depth 1 https://github.com/Stephane-D/SGDK.git
gcc -O2 -o /tmp/bintos SGDK/tools/bintos/src/bintos.c     # ships with SGDK
```

The library build needs `sjasm` for seven `.s80` Z80 drivers. Those assemble to
**pure data** — every one of the resulting objects has a zero-length `.text` —
so the blobs already inside the shipped library can be reused verbatim instead,
carrying no ABI with them. `tools/sgdk_fake_sjasm.sh` extracts them.

```sh
cd SGDK
make -f makelib.gen PREFIX=m68k-linux-gnu- \
     ASMZ80=../tools/sgdk_fake_sjasm.sh BINTOS=/tmp/bintos
```

Then build the ROM with the stock makefile — **and link SGDK's shipped
`lib/libgcc.a`, not the cross compiler's own** (`LIBGCC=SGDK/lib/libgcc.a` on
the make command line). This is the one that will bite silently: Ubuntu's
`m68k-linux-gnu` libgcc targets Linux, which assumes a 68020+, so its division
helpers open with `bsr.l` (`0x61FF`) — an instruction the 68000 does not have.
Every `u32` division in the ROM calls those helpers. BlastEm's core happens to
accept 68020 encodings so it plays fine there, while Gens r57shell dies with a
LINE 1111 exception, PicoDrive with an address error, and real hardware would
crash too — all inside `__divsi3`/`__modsi3` at the first division after boot.

Build the ROM: Ubuntu's cross-GCC defaults to
emitting a build-id note, which on a bare-metal link lands at address 0 and
collides with `.text`; a wrapper adds `-Wl,--build-id=none` at link time only.
An `m68k-elf` toolchain needs neither the wrapper nor any of the above.

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

## Checking you are running the right build

The on-screen readout tells you immediately. The current build shows `MODE` and
`LOOP` on its top two lines and tags each voice `DAC`, `HW` or `FM`:

```
MODE FM TRI    PROTO v2   23244 Hz
LOOP V2  (154 cyc)  DPCM rdy
P1   736 Hz d0 v11 DAC
```

Anything showing `Selected CH1` and `P1: 1234 N, V: 4, D: 0` is the pre-2026
build, which predates all of the volume-DAC work.

## Headless testing against a real emulator core

`tools/lrhost.c` is a minimal libretro host: it loads an emulator core (.so),
runs the ROM for N frames with no window, reports per-frame audio energy, and
dumps chosen frames as PPM images so the on-screen overlay can be read from a
capture. This is how the pulse-2 misread was reproduced and verified fixed
against the same PicoDrive core the bug was reported on.

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


## Regenerating nes_apu_data.txt from your own ROM

`nes_apu_data.txt` is not checked into this repository. The capture format
encodes a specific game's music -- pitches, durations, envelopes -- which is
exactly the part of a ROM copyright protects. What ships instead is the tool
that produces it, and a button-input schedule (a timed sequence of controller
presses, which holds no game content of its own) to drive a specific stretch
of gameplay. Given your own legally-obtained ROM, running the tool never
distributes anything of the game's.

```
cd tools
npm install
node gen_apu_capture.js path/to/game.nes schedules/ducktales.txt 8697 -o ../nes_apu_data.txt
```

Uses [jsnes](https://github.com/jsnes/jsnes), a from-scratch NES core with
every APU register write visible as a plain JS call. That mattered here: an
FCEUX-under-Xvfb attempt hung/crashed repeatedly in this environment (Qt
needs a real GL context), and Nestopia's libretro core -- reliable for the
DPCM extraction work, see above -- only exposes 2 KB of system RAM through the
standard `retro_get_memory_data` API, not the write-only APU I/O registers a
capture needs. jsnes sidesteps both: no GUI to crash, and
`nes.papu.writeReg()` is one hookable choke point for every register write.

The output is field-for-field identical to `GeNESis-APU2PSG-Recorder.lua`'s
v2 format (`#GAPU2 v2` header, 26 comma-separated fields a line, `.lower.py`
tools none the wiser) -- same post-envelope volumes (`channel.masterVolume`,
already the correct decayed level, no re-derivation needed), same
hardware-accurate on/off bit (`channel.getLengthStatus()` for all five
channels including DMC), same $4015-write-instant DMC trigger latch as the
Lua recorder's `memory.registerwrite` hook. `tools/gen_apudata.py` and
`tools/gen_dpcm.py` consume it unmodified.

**Status:** the extraction mechanism is verified -- well-formed output every
frame, real musical variation, and DPCM triggers cross-validated against the
independent static ROM scan (both agree DuckTales has none). The *included*
`schedules/ducktales.txt` is only a first pass, though: over a full 8697-frame
run it only reaches 3 distinct pulse periods, which reads as stuck on a
title/menu loop rather than into real level content. Refining the schedule
to reach further into the game is the next step, not a tool problem -- swap
in a better-timed schedule (or your own) and rerun.
