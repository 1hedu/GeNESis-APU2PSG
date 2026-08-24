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

Without SGDK's own toolchain, using Ubuntu's m68k cross compiler. Recorded here
because three things had to be worked around, and anyone reproducing it will hit
the same three:

```sh
apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu
git clone --depth 1 https://github.com/Stephane-D/SGDK.git

# 1. SGDK's prebuilt libmd.a carries LTO bytecode from a different GCC build,
#    which this compiler refuses to read. The archive also has full machine code
#    in it (-ffat-lto-objects), so stripping the bytecode leaves it linkable.
mkdir delto && cd delto && m68k-linux-gnu-ar x SGDK/lib/libmd.a
for f in *.o; do
  SEC=$(m68k-linux-gnu-objdump -h "$f" | awk '{print $2}' \
        | grep -E '^\.gnu\.(lto_|debuglto_)' | sed 's/^/-R /' | tr '\n' ' ')
  [ -n "$SEC" ] && m68k-linux-gnu-objcopy $SEC "$f"
done
m68k-linux-gnu-ar rcs ../libmd.a *.o && cd ..

# 2. Compile with LTO off for the same reason.
make -f SGDK/makefile.gen GDK=SGDK PREFIX=m68k-linux-gnu- EXTRA_FLAGS="-fno-lto"

# 3. The link rule hardcodes its flags, so do that step by hand: Ubuntu's GCC
#    emits a build-id note that lands at address 0 and collides with .text.
m68k-linux-gnu-gcc -m68000 -n -T SGDK/md.ld -nostdlib \
  out/release/sega.o out/release/src/main.o libmd.a -lgcc \
  -o out/release/rom.out -Wl,--gc-sections -Wl,--build-id=none -fno-lto
m68k-linux-gnu-objcopy -O binary out/release/rom.out rom.bin
java -jar SGDK/bin/sizebnd.jar rom.bin -sizealign 131072
```

Then fix the header checksum (sizebnd's `-checksum` did not take here):

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
