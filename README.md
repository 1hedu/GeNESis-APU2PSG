<img width="640" height="448" alt="rom-260824-200447" src="https://github.com/user-attachments/assets/e6f8faea-dbc2-4eb5-bdfa-c3f3e3a36be5" />


# GeNESis-APU2PSG: Playback NES audio data on Genesis/Mega Drive

This project is devoted to Krikkz, the inventor of EverDrive.  

I used it to play a MegaDrive port of Super Mario Bros. 1, and it mapped some of the soundtrack to the FM synth. I think it was just the bass, but still very cool to hear it playback with that Sega character. Then I learned, that the Everdrive PRO, actually included a NES core on the FPGA. I was then struck by the idea to do this project--after all it should be possible. 

I chose to start with the PSG only because even without layering FM color, the difference in soundchip and circuitry on the two different hardware platforms, different sample rates, should still produce some distinctly-Sega timbre, in theory. Indeed, with the volume DAC solution its definitely got...character.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/crossover-dark.png">
  <img alt="Pitch map: the triangle sits on FM at any pitch; the volume DAC covers pulses up to its ceiling, above which each voice falls back to the PSG hardware tone generator." src="docs/crossover-light.png">
</picture>

Requirements:
- SGDK
- FCEUX NES Emulator
- Gens r57Shell Genesis/MegaDrive Emulator
- Python 3, only if you want to rebuild the Z80 driver (the assembled blob is checked in)

`rom.bin` is checked in and built from the current source. The capture is baked
into the cart and plays at boot with no script (B switches to the live script
path), so any emulator or an Everdrive can play it standalone. To change the
code see **[BUILDING.md](BUILDING.md)**.

# To record NES audio:
Open up FCEUX, load GeNESis-APU2PSG-Recorder lua script. It should run without error. 
Load a NES Rom file and the script will imediately start logging the audio data

# To playback NES audio on Genesis/MegaDrive:
Both the Sega ROM and the lua script GeNESis-APU2PSG-Player must be loaded. 
As long as the NES data file exists, in same directory as gens executable, it should playback the song using the PSG chip.

`nes_apu_data.txt` is not distributed (it encodes the game's music). Recreate it from your own copy of the game:

```
python3 tools/gapu_map.py unpack nes_apu_data.gapumap game.nes -o nes_apu_data.txt
```

# To use the live synced version:
Have to have both scripts running at the same time, in the same directory. Turn down the NES emulator audio in OS settings.

# How it works

- **Pulses** — park a PSG channel's tone period at 1 (ultrasonic carrier) and rewrite its 4-bit attenuator from a free-running Z80 loop: a volume DAC, giving true 12.5 / 25 / 75% duty. The loop's sample rate sets a pitch ceiling (3107 Hz in V2, 2557 in V2D, 1920 in V3); above it a voice falls back to the hardware tone — pitch exact, duty 50%.
- **One pulse on FM** — an optional mode puts either pulse on YM2612 channel 4 (four carriers at MUL 1/2/3/4, levels from the duty's own harmonic series) and leaves the other on the volume DAC. LEFT/RIGHT swaps which is which. With one voice left, the Z80 loop drops to 73 cycles: a 49 kHz sample rate and a 6129 Hz ceiling, so the DAC pulse keeps its true duty at any pitch in the song.
- **Triangle** — YM2612 channel 5, algorithm 7, four carriers at MUL 1/3/5/7. A PSG-only mode plays it on the volume DAC instead.
- **Noise** — PSG noise clocked from tone channel 2 (rate 3) reaches 14 of the 16 NES periods; short mode reaches 15. Costs nothing with the triangle on FM.
- **DPCM** — YM2612 channel 6 DAC, streamed from 68000 RAM through the Z80 bank window. `tools/gen_dpcm.py` extracts samples from the game's `.nes`.

The Z80 does only the sample-rate work; everything register-rate is written by
the 68000 directly (the PSG lives in the VDP at `0xC00011`, not on the Z80
bus). `technique-map.html` is a one-page summary.

# Controls

| button | what it does |
|---|---|
| B | data source: **CART** (embedded capture, boot default) vs **SCRIPT** (live from Lua) |
| C | play the embedded DPCM test drum |
| START | cycle synthesis mode: **HW** → **DAC** → **DAC+NOISE** → **FM TRI** (default) → **FM TRI+P** (one pulse on FM too) |
| LEFT / RIGHT | in FM TRI+P: swap which pulse is the FM one |
| X / Y / Z | mute pulse 1 / pulse 2 / triangle |
| A | mute noise |
| UP / DOWN | trim the FM triangle's level, or the FM pulse's in FM TRI+P |
| MODE | manual noise audition (6-button pad): LEFT/RIGHT period, Z long/short, UP/DOWN volume |

# Files

| file | what it is |
|---|---|
| `GeNESis-APU2PSG.c` | the ROM |
| `psgdac.h` | 68000 side of the Z80 driver |
| `z80_psgdac.s80` | the Z80 driver source |
| `psgdac_z80.h` | assembled driver blob, checked in |
| `tools/asmz80.py` | small Z80 assembler |
| `tools/simz80.py` | runs the driver on a toy Z80 and checks its output |
| `tools/gen_apudata.py` | packs `nes_apu_data.txt` into `apudata.h` for the cart |
| `nes_apu_data.gapumap` | the capture, XOR-packed against the game ROM; `tools/gapu_map.py` unpacks it |
| `technique-map.html` | one-page summary of which technique covers which voice and pitch |

To rebuild the driver after editing the assembly:

```
python3 tools/asmz80.py z80_psgdac.s80 -o psgdac_z80.h
python3 tools/simz80.py
```

# Data format

The recorder writes `#GAPU2 v2`. Fields 1..18 are the v1 layout; v2 appends
post-envelope volumes, DPCM enable, a frame counter, and DMC trigger events
latched at the `$4015` write. NES volume is linear and PSG attenuation
logarithmic, so `15 − v` is not a level conversion; period fields must be
masked with `0x7FF`.

# NOTES:

- Noise rests when the stream says it rests; A is a manual mute.
- The scripts resolve `nes_apu_data.txt` next to themselves.
- A live synced version <s>exists,</s> is added.
- <s>Gens r57shell may be hard to find. I downloaded it, and tried a couple days later from the same location, and the link was broken.  I'm working on a BizHawk version of the Gens lua.</s> Link is back.
- The Lua scripts find the shared block by scanning RAM for a `GAPU` magic; neither side hardcodes an address.
- Thank you to AlyJames, who helped elucidate the potential of pulse waves on the Genesis, for me, a random DM.

# TODO:
6. Get the attention of Krikkz, so he might add this to his NES core on his Mega Everdrive PRO
