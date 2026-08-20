# GeNESis-APU2PSG: Playback NES audio data on Genesis/Mega Drive

This project is devoted to Krikkz, the inventor of EverDrive.  

I used it to play a MegaDrive port of Super Mario Bros. 1, and it mapped some of the soundtrack to the FM synth. I think it was just the bass, but still very cool to hear it playback with that Sega character. Then I learned, that the Everdrive PRO, actually included a NES core on the FPGA. I was then struck by the idea to do this project--after all it should be possible. 

I chose to start with the PSG only because even without layering FM color, the difference in soundchip and circuitry on the two different hardware platforms, should still produce some distinctly-Sega timbre, in theory. I thought about how to use the PSG to emulate the Triangle channel, and pulse waves with Duty other than 50%. Ultimately, to avoid the FM synth to the utmost, probably requires Z80 assembly.

Currently, frequency-accurate playback of each channel is working. Volume modulation per NES envelopes is working on all channels. <s> except Square 3, which is Triangle on NES.</s>  This is enough to get a song to playback very recognizably.   

**It requires Z80 assembly, and now it has some.** The PSG's attenuator is a 4-bit logarithmic DAC; park a tone channel's period at 0 so its output is DC and rewrite that attenuator fast enough, and the channel stops being a square wave and becomes a waveform generator. That gives real 12.5% / 25% / 75% pulses and a real triangle staircase, on the PSG, with no FM. It also has hard limits, and those limits are why this is now a *combination* of techniques rather than one.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/crossover-dark.png">
  <img alt="Pitch map: the volume DAC covers everything below 1920 Hz in V3 and 3107 Hz in V2; above that each voice falls back to the PSG hardware tone generator. The triangle below 109 Hz is reachable only by the volume DAC." src="docs/crossover-light.png">
</picture>

*Which technique covers which pitch, per voice. Red is the volume DAC, blue is the PSG's own tone generator; the hatched zone is where the DAC is not an upgrade but the only option. Full interactive version: **[the crossover map](https://claude.ai/code/artifact/09fba06d-d92a-44e6-b7d8-c447f013e359)**. Details below.*


Requirements:
- SGDK
- FCEUX NES Emulator
- Gens r57Shell Genesis/MegaDrive Emulator
- Python 3, only if you want to rebuild the Z80 driver (the assembled blob is checked in)

You must use SGDK to build the ROM.

# To record NES audio:
Open up FCEUX, load GeNESis-APU2PSG-Recorder lua script. It should run without error. 
Load a NES Rom file and the script will imediately start logging the audio data

# To playback NES audio on Genesis/MegaDrive:
Both the Sega ROM and the lua script GeNESis-APU2PSG-Player must be loaded. 
As long as the NES data file exists, in same directory as gens executable, it should playback the song using the PSG chip.

# To use the live synced version:
Have to have both scripts running at the same time, in the same directory. Turn down the NES emulator audio in OS settings.


# The technique map

Four techniques, none of which covers the whole job. What follows is which one wins where, and why.

The figures here and in **[the crossover map](https://claude.ai/code/artifact/09fba06d-d92a-44e6-b7d8-c447f013e359)** are generated, not drawn. Rebuild them from the repo root with `python3 tools/build_map.py technique-map.html`; every coordinate and percentage comes from the assembler's cycle counts and `nes_apu_data.txt`, so the pictures cannot drift from the code.

### 1. Hardware tone generator
The PSG's own square wave. Pitch-exact, costs nothing, works at any pitch the chip can reach. Two limits: it is 50% duty and only 50% duty, and its period register is 10 bits, so it bottoms out at **109 Hz**. The NES triangle goes down to 27 Hz. Below 109 Hz the hardware generator does not go flat, it simply cannot go there at all.

### 2. Volume DAC  *(the main solution)*
Park the tone period at 0 so the channel outputs DC, then rewrite its attenuation register from a free-running Z80 loop. The attenuator is logarithmic, which turns out to be a gift: scaling a waveform by a volume is *addition* in the log domain, so a whole pulse voice is eight instructions with no wavetable at all —

```
        ld de,DELTA         ; phase += delta
        add hl,de
        ld a,h
        cp  DUTY            ; carry <=> inside the high part of the pulse
        sbc a,a             ; -> 0xFF or 0x00
        and SPAN            ; = (attenuation - 15) & 0xFF
        add a,MUTE          ; log-domain volume scaling is just an add
        ld (7F11h),a
```

63 Z80 cycles a voice. The triangle needs a real 32-step table, so it costs 89.

**Sample rate is loop length.** That is the whole cost model, and it is why this "struggles with more pitch or channels" — they are the same problem seen from two sides. Each voice you add lengthens the loop, which lowers the sample rate, which lowers the pitch ceiling for *every* voice. So the driver ships three loop variants and the ROM picks one per frame:

| variant | voices | cycles | sample rate | 12.5% duty ceiling |
|---|---|---|---|---|
| **V3**  | pulse + pulse + triangle | 233 | 15363 Hz | 1920 Hz |
| **V2**  | pulse + pulse            | 144 | 24858 Hz | 3107 Hz |
| **V2D** | pulse + pulse + PCM      | 175 | 20455 Hz | 2557 Hz |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/loop-budget-dark.png">
  <img alt="Z80 cycles per sample for each loop variant: two pulse voices at 63 cycles each, a wave voice at 89, a PCM voice at 31, plus 18 cycles of loop overhead." src="docs/loop-budget-light.png">
</picture>

A 12.5% pulse needs eight slots per period to exist at all, so the ceiling is the sample rate over eight. Above it the voice is handed back to the hardware tone generator: the pitch stays exact and the duty degrades to 50%, which is a far better trade than an aliased 12.5% pulse.

Measured against the capture checked into this repo (8697 frames):
- **85% of pulse-1 frames use a duty the PSG hardware cannot make** (61.5% at 12.5%, 23.4% at 75%). This is the technique earning its keep.
- **97% of pulse frames sit at or below V3's ceiling**, 100% below V2's. So the crossover fires on about 3% of frames — audible on the high lead runs, which is exactly where it was reported to struggle.

### 3. Tone-clocked noise
The PSG's noise generator has three fixed rates. The NES has sixteen periods in two modes, so 32 sounds. Setting the PSG's noise rate to 3 clocks the shift register from **tone channel 2** instead, and channel 2's period is 10 bits — so the NES's rate table becomes reachable to within about 1%:

| NES period index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch2 period (white) | – | 1 | 2 | 4 | 8 | 12 | 16 | 20 | 25 | 32 | 47 | 63 | 95 | 127 | 254 | 508 |
| period error | – | 0% | 0% | 0% | 0% | 0% | 0% | 0% | −1.0% | +0.8% | −1.1% | −0.8% | −0.3% | 0% | −0.1% | −0.1% |

Fifteen of sixteen, exactly. Only index 0 is out of reach — it wants a 447 kHz shift rate, and the PSG tops out at half that.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/noise-coverage-dark.png">
  <img alt="Noise coverage strip: only periods 6, 7, 9 and 11 are close enough to a fixed PSG rate; the other twelve need channel 2 as the shift clock, and those are the ones the capture actually uses." src="docs/noise-coverage-light.png">
</picture>

*All 16 NES noise periods. Blue is reachable with one of the PSG's three fixed rates, gold needs channel 2 as the shift clock, grey is out of range. Bars are each period's share of noise-active frames in the checked-in capture.*

Short (periodic) mode needs a different mapping. The NES's short sequence is 93 steps to the PSG's 15, so matching the shift *rate* would put the pitch six octaves out; the table matches perceived pitch instead. Indices 14 and 15 clamp, at 9.5 Hz and 4.7 Hz, which is below hearing anyway.

**The price is channel 2, which is where the triangle lives.** That is the trade the old TODO asked about, and it is not close: in the checked-in capture, **99.2% of noise-active frames use a period the three fixed rates cannot reach**. Drums are almost never on one of the three. So this is a genuine choice, not a free win, and it is on a button (START) so you can hear both.

### 4. YM2612 channel 6 DAC — for DPCM
DPCM does not belong on the volume DAC. It would consume the entire Z80 loop, take a PSG channel, and still only get four logarithmic bits. The YM2612's channel-6 DAC is 8-bit linear and, once register 2Ah is latched, costs one store per sample. That is its home.

The samples stream out of 68000 RAM through the Z80's bank window, *not* over the Z80 bus — because every byte the 68000 hands to the Z80 requires stopping the Z80, and stopping the Z80 stops the audio. That is also why parameter updates are diffed: only bytes that actually changed get written, so the per-frame stall is a handful of stores instead of a hole in the sound.

Status: the driver path, the ring protocol and the recorder-side `$4011` capture are all in. It is the least exercised part of this and is off by default — set `RECORD_DPCM = true` in the recorder.

### What this means for the FM ideas in the old TODO
Two of them are now unnecessary rather than unfinished. The volume DAC produces the duties directly, so there is nothing left for "50% square + FM to color it into 12.5%" to fix, and nothing left for a DC-offset trick on an FM operator to reach. FM's remaining job is the one the PSG genuinely cannot do: the DAC channel for DPCM, and extra polyphony above the volume DAC's pitch ceiling if you ever want the high leads to keep their duty.


# Controls

| button | what it does |
|---|---|
| START | cycle synthesis mode: **HW** (everything on hardware tone generators) → **DAC** (volume-DAC pulses and triangle) → **DAC+NOISE** (also tone-clocked noise, which costs the triangle) |
| X / Y / Z | mute pulse 1 / pulse 2 / triangle |
| A | mute noise (starts muted) |
| MODE | manual noise audition: step all 16 periods by hand |
| B / C, LEFT / RIGHT | in manual noise: previous / next NES noise period |
| Z, UP / DOWN | in manual noise: long/short mode, volume |

MODE needs a 6-button pad. The on-screen readout shows which loop variant is running, its sample rate, and whether each voice is currently **DAC** or **HW**, so the crossover is visible while it happens.


# Files

| file | what it is |
|---|---|
| `GeNESis-APU2PSG.c` | the ROM. Reads the shared RAM block, decides technique per voice per frame, drives the PSG |
| `psgdac.h` | 68000 side of the volume-DAC driver: upload, patch, diff |
| `z80_psgdac.s80` | the Z80 driver, and the source of truth for it |
| `psgdac_z80.h` | assembled driver blob + patch offsets. Generated, checked in, no build step needed |
| `tools/asmz80.py` | a tiny dependency-free Z80 assembler, so the blob can be regenerated with stock Python |
| `tools/simz80.py` | runs the assembled driver on a toy Z80 and checks the waveforms it emits |
| `tools/build_map.py` | generates `technique-map.html` from the cycle counts and the capture. The PNGs in `docs/` are screenshots of its figures |
| `technique-map.html` | the generated map, standalone. Open it in a browser, or read the hosted copy linked above |

To rebuild the driver after editing the assembly:

```
python3 tools/asmz80.py z80_psgdac.s80 -o psgdac_z80.h
python3 tools/simz80.py
```

`simz80.py` verifies the duty ratios, the attenuation encoding, the wave-table walk, that a disabled voice costs the same cycles as an enabled one, and the loop lengths the sample rates are derived from. If you change the loop, the sample rates in `psgdac.h` and `DPCM_RATE` in the recorder change with it.


# Data format

The recorder now writes `#GAPU2 v2` as a header line and appends four fields to each line. Fields 1..18 are unchanged, so old players still read new logs, and new players still read old logs. What v2 adds is what v1 could not say:

- **post-envelope volumes.** v1 logged the raw `$4000` nibble, which is the audible volume only when the constant-volume flag is set. Every envelope tail was wrong.
- **the noise period index and mode.** v1 carried a three-way rate where the NES has sixteen in two modes.
- **pulse 2's duty**, which v1 recorded and then never transmitted.

Two conversion bugs went with it. NES volume is linear 0..15 and PSG attenuation is logarithmic at 2 dB a step; the old `15 - v` treated one as the other, so NES volume 8 came out 14 dB down instead of 2.5 dB down. And `memory.readword(0x4002)` returns the period *plus* the length-counter bits from `$4003`; 8666 of the 8697 lines in the checked-in capture carry that pollution, and it was never masked off.


# NOTES:

- <s>Press 'z' on keyboard to toggle noise channel. i cant get it to rest silently.</s> Noise now rests when the stream says it rests; A is a manual mute on top of that.
- Alter the filepath in the scripts, to point to same dir, OR put nes_apu_data.txt, in same directory as Gens.exe.
- <s>During playback, the noise channel only, must be enabled by pressing A on the controller.</s> Still true, and still for the same reason: with no Lua feeding it, an enabled noise channel just blares.
- A live synced version <s>exists,</s> is added.
- <s>Gens r57shell may be hard to find. I downloaded it, and tried a couple days later from the same location, and the link was broken.  I'm working on a BizHawk version of the Gens lua.</s> Link is back.
- The shared block lives at a hardcoded `0xFF0000`, which is inside SGDK's own RAM area. It has always worked here, but it is luck, not design — if a future SGDK build puts something live at those addresses, that is the first place to look.
- Thank you to AlyJames, who helped elucidate the potential of pulse waves on the Genesis, for me, a random DM.

# TODO:
1. <s>Fix Triangle. Not playing correct note lengths.</s>
2. <s>Complete mapping of 32 Noise sounds possible on NES.</s> Done — 15 of 16 periods in both modes, via tone-clocked noise. <s>Evaluate sacrificing square3 for full frequency range.</s> Evaluated: it costs the triangle, and 99.2% of noise frames in the test capture need it. On START, so you can judge by ear.

3. Test timbre tricks and Genesis/MegaDrive FM synth integration.
      
      Some possibilities:
  
      - <s>DC Offset trick + Volume Modulation (VM) to produce pulse waves of various duty, and triangle, on PSG.</s> Done. This is the main solution.
      - 2 detuned PSG square waves can give us a pulse wave similar to what NES produces. <s>Can we get 3 to sound like 2?</s> No. Still worth trying as the *high-pitch* fallback, where the volume DAC runs out of sample rate and currently degrades to 50% — two phase-offset hardware squares keep some duty character at any pitch, at the cost of a second channel.
      - FM synth DAC mode channel to play NES DPCM channel. Driver path and ring protocol are in; needs real testing.
      - <s>DC Offset trick on 1 FM synth channel using separated operators (Special FM Mode).</s> Not needed now — the PSG makes the duties directly.
      - <s>FM synth layered over 50% square, to color the waveform appropriately per whichever duty the NES is playing.</s> Superseded below the pitch ceiling. Still the best idea *above* it.

4. Test on real hardware. Everything here is verified in simulation (the Z80 loop is executed instruction by instruction and its output checked) and against the checked-in capture, but none of it has been through a real Mega Drive yet. The bus-stall behaviour and the exact V2D sample rate — the PCM read goes through the bank window and picks up wait states the cycle count does not know about — are the two things most likely to differ.

5.  Get the attention of Krikkz, so he might add this to his NES core on his Mega Everdrive PRO
