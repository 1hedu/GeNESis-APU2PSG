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

Then build the ROM with the stock makefile. Ubuntu's cross-GCC defaults to
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

## Rebuilding the Z80 driver

Only needed if you edit `z80_psgdac.s80`:

```
python3 tools/asmz80.py z80_psgdac.s80 -o psgdac_z80.h
python3 tools/simz80.py
```
