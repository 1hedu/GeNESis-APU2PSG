// =============================================================================
// GeNESis-APU2PSG -- play NES APU data on the Genesis/Mega Drive PSG
//
// The PSG has three tone generators and a noise generator.  The NES has two
// pulses with four duties, a triangle, noise with 32 distinct sounds, and DPCM.
// Nothing about that maps cleanly, so this ROM mixes four techniques and
// chooses between them every frame:
//
//   1. HARDWARE TONE   -- the PSG's own square generator.  Pitch-exact from
//                         109 Hz up, free, but 50% duty and nothing else.
//   2. VOLUME DAC      -- park a channel's period at 1 (a 55.9 kHz carrier the
//                         output stage averages away), then
//                         rewrite its 4-bit logarithmic attenuator thousands of
//                         times a second.  Gives real 12.5/25/75% duty and a
//                         real triangle staircase, and reaches below the PSG's
//                         109 Hz floor.  Costs Z80 loop time, and its pitch
//                         ceiling is the loop's sample rate over the number of
//                         steps a waveform needs.
//   3. TONE-CLOCKED NOISE -- feed the noise generator from tone channel 2
//                         instead of its three fixed rates.  Reaches 15 of the
//                         16 NES noise periods instead of 3, at the price of
//                         channel 2, which is where the triangle lives.
//   4. YM2612 DAC      -- DPCM does not fit on the volume DAC: it would eat the
//                         whole Z80 loop and still only get four logarithmic
//                         bits.  Channel 6's 8-bit linear DAC is its home.
//
// Techniques 2 and 4 share one Z80 loop, and the loop's length is its sample
// rate, so they compete.  The allocator in synthUpdate() is where that gets
// resolved.  See the technique map in README.md.
// =============================================================================

#include <genesis.h>
#include <z80_ctrl.h>
#include <psg.h>

#include "psgdac.h"
#include "apudata.h"
#include "pcmsample.h"

// -----------------------------------------------------------------------------
// Sound chip access.
//
// The PSG lives in the VDP at 0xC00011 -- NOT on the Z80 bus -- so the 68000
// writes it directly, any time, with no bus request.  Two rules keep the two
// masters honest:
//
//   - A tone period is a latch byte plus a data byte, and the Z80 DAC loop's
//     attenuation bytes re-point the PSG's register latch.  So tone-period
//     pairs are wrapped in a short bus request: the Z80 is stopped for ~2 us
//     and nothing can interleave.  Attenuation and noise-control writes are
//     single self-contained bytes and go out bare.
//   - The YM2612 IS on the Z80 bus, so FM writes hold the bus; and any part-I
//     access breaks the address latch the V2D PCM store relies on, so 0x2A is
//     re-latched before the bus is released.
// -----------------------------------------------------------------------------
#define PSG_PORT_68K ((vu8*)0xC00011)

static inline void psgWrite(u8 b) { *PSG_PORT_68K = b; }

// Tone periods only; must NOT be called while the bus is already held.
static void psgTone(u8 ch, u16 period)
{
    Z80_requestBus(TRUE);
    *PSG_PORT_68K = 0x80 | (ch << 5) | (period & 0x0F);
    *PSG_PORT_68K = (period >> 4) & 0x3F;
    Z80_releaseBus();
}

static inline void psgAtten(u8 ch, u8 att) { psgWrite(0x90 | (ch << 5) | (att & 0x0F)); }

// ---- direct access, valid only while the bus is held -----------------------
static inline void psgDirect(u8 b) { *PSG_PORT_68K = b; }

static void ymDirect(u8 part, u8 reg, u8 val)
{
    vu8* addr = (vu8*)(0xA04000 + (part * 2));
    vu8* data = (vu8*)(0xA04001 + (part * 2));
    while (*addr & 0x80) ;
    *addr = reg;
    while (*addr & 0x80) ;
    *data = val;
}

// -----------------------------------------------------------------------------
// NES -> PSG conversion tables.  Regenerate with tools/gen_tables.py.
// -----------------------------------------------------------------------------

// NES volume is linear 0..15.  PSG attenuation is logarithmic, 2 dB a step.  The
// old 15-v mapping treated one as the other, which is why envelope tails used to
// dive off a cliff: NES volume 8 is 2.5 dB down, not 14 dB down.
static const u8 nesVolToAtten[16] = {15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0};

// Duty as a threshold on the phase accumulator's high byte.
static const u8 dutyThreshold[4] = {32, 64, 128, 192};   // 12.5, 25, 50, 75 %

// A DAC-mode voice is exactly 6 dB quieter than the same channel making a tone.
//
// The Sega PSG takes a period of 0 literally rather than substituting 0x400 the
// way a discrete SN76489 does, so the channel toggles every internal clock: a
// 112 kHz square that the output filter averages to half the attenuator's level.
// That halving is what the volume DAC modulates, so its fundamental is (2/pi)(V/2)
// where a hardware square at the same attenuation gives (2/pi)V -- a factor of
// two on the nose. Without this, a pulse crossing the pitch ceiling would jump
// 6 dB louder at the handover. Attenuation is 2 dB a step, so the correction is 3.
#define HW_ATTEN_OFFSET 3

static inline u8 hwAtten(u8 att)
{
    u16 a = att + HW_ATTEN_OFFSET;
    return (a > 15) ? 15 : (u8)a;
}

// Channel-2 tone period that clocks the noise generator at the NES rate.
//
// The shift register advances once per tone-3 output cycle, so its rate is
// clock/(32*P) -- the same as the tone frequency, not twice it. MAME settles it:
// the noise counter reloads with (tone-3 period << 1) at the internal clock/16,
// one shift per expiry, which is also how the fixed settings come out as
// clock/512, /1024 and /2048. Getting this wrong puts every drum an octave out.
//
// Two more things the reference implementation settles, both of which this code
// depends on:
//   - Writing tone channel 2's period updates the noise rate immediately while
//     mode 3 is selected, without touching the noise control register. So drum
//     pitch can move without retriggering the sequence.
//   - Writing the noise control register reseeds the shift register on every
//     write, not only when the mode bit changes -- the Sega part is not the
//     NCR variant. Hence the change-guard in noiseOut().
//
// White (long) mode matches the shift rate. Periodic (short) mode matches
// perceived pitch instead, because matching the clock would put it octaves out:
// the NES's short sequence is 93 steps, and the PSG's periodic mode is a pure
// rotate of the shift register, so it pulses once per register width.
//
// That width is 16 on the Sega PSG, not the discrete SN76489's 15 -- the Sega
// part widened it (MAME: feedback mask 0x8000 versus 0x4000, taps 0x01/0x08
// versus 0x01/0x02). Using 15 here puts every periodic drum about a semitone
// flat. Indices 0 and 15 are unreachable and pin at the ends.
static const u16 noisePeriodWhite[16]    = {1, 1, 1, 2, 4, 6, 8, 10, 13, 16, 24, 32, 48, 63, 127, 254};
static const u16 noisePeriodPeriodic[16] = {1, 3, 6, 12, 23, 35, 46, 58, 73, 92, 138, 185, 277, 369, 739, 1023};

// Nearest of the PSG's three fixed noise rates, and whether that nearest is more
// than 25% out. The fixed rates land on indices 9, 11 and 13 -- to within 0.8%,
// which is why they exist at all. Everything else needs channel 2. Indices 0 and
// 1 want a period below 1 and are out of the chip's reach in white mode; in
// periodic mode the 93-to-15 pitch factor lifts them back into range.
static const u8 noiseFixedRate[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2};
static const u8 noiseNeedsCh2[16]  = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1};

// -----------------------------------------------------------------------------
// Shared memory protocol with the Lua side.
//
// This block used to sit at a hardcoded 0xFF0000, which is inside the linker's
// RAM area. It worked by luck and then stopped: SGDK's task_pc and task_regs
// landed at 0xFF001A and 0xFF001E, so zeroing the block every frame wiped the
// saved program counter the vblank handler returns through. An illegal
// instruction at PC 0x70 -- a jump into the vector table -- is what that looks
// like from the outside.
//
// So the block is a real C object now, and the linker guarantees nothing else
// shares it. Lua finds it by scanning RAM for the magic and checking that the
// 'self' field points back at where it was found; nothing on either side hard-
// codes an address any more.
// -----------------------------------------------------------------------------
#define APU_BLOCK_MAGIC 0x47415055UL            // 'GAPU'
#define APU_MAGIC       0x47                    // 'G' -- v2 fields valid this frame

typedef struct {
    u32 magic;          // +0   'GAPU', written once at boot
    u32 self;           // +4   this struct's own address, so a scan can verify
    u32 ring;           // +8   the PCM ring's address, or 0
    u8  legacy[16];     // +12  v1 block, PSG-native
    u8  ext[32];        // +28  v2 block, NES-native
} ApuBlock;

static volatile ApuBlock apuBlock;

#define APU_LEGACY  (apuBlock.legacy)
#define APU_EXT     (apuBlock.ext)

// v2 field offsets inside APU_EXT
enum {
    X_MAGIC = 0,
    X_P1_LO, X_P1_HI, X_P1_VOL, X_P1_DUTY, X_P1_ON,
    X_P2_LO, X_P2_HI, X_P2_VOL, X_P2_DUTY, X_P2_ON,
    X_TRI_LO, X_TRI_HI, X_TRI_ON,
    X_NOI_PER, X_NOI_MODE, X_NOI_VOL, X_NOI_ON,
    X_DPCM_ON, X_FRAME,
    X_COUNT
};

// -----------------------------------------------------------------------------
// DPCM test playback.  The Z80's V2D loop free-runs through the 4 KB ring at
// its sample rate and publishes which 256-byte page it is reading; the 68000's
// only job is to keep filling the pages behind it.  C plays an embedded drum
// sample through that path, which exercises the entire DPCM chain -- variant
// switch, ring streaming, the YM2612 DAC -- with no script involved.
// -----------------------------------------------------------------------------
static u32 pcmPos      = 0;         // next byte of the sample to stream
static u8  pcmPlaying  = 0;
static u8  pcmLastPage = 0;
static u16 pcmSilence  = 0;         // pages of silence streamed after the end

// PCM ring the Z80 streams from through its bank window.  4 KB, page aligned so
// the driver's wrap is a mask instead of a compare.  Lua writes it directly, so
// sample data never has to cross the Z80 bus.
static u8 dpcmRing[4096] __attribute__((aligned(4096)));

// -----------------------------------------------------------------------------
// Synthesis mode: what the user is listening to.  START cycles it.
// -----------------------------------------------------------------------------
#define MODE_HW      0   // everything on hardware tone generators
#define MODE_DAC     1   // volume-DAC pulses and triangle, fixed-rate noise
#define MODE_DAC_N   2   // ...plus tone-clocked noise, which costs the triangle
#define MODE_FM      3   // triangle to FM: nothing is contested any more
#define MODE_COUNT   4

static const char* const modeName[MODE_COUNT] = {"HW", "DAC", "DAC+NOISE", "FM TRI"};
static const char* const variantName[PSGDAC_NVARIANT] = {"V3", "V2", "V2D"};

static u8 synthMode = MODE_FM;
static u8 dpcmAvailable = 0;

// -----------------------------------------------------------------------------
// Where the APU stream comes from. CART plays the capture baked into the ROM --
// no emulator scripting involved, so it runs anywhere, which also makes it the
// control experiment when a crash might be script-related. SCRIPT is the live
// shared-memory path. B toggles; CART is the boot default so a bare cart makes
// sound immediately.
// -----------------------------------------------------------------------------
#define SRC_CART    0
#define SRC_SCRIPT  1
static u8  dataSource = SRC_CART;
static u16 cartFrame  = 0;

// The capture is 60 frames a second of NES time. On a PAL machine vblank comes
// 50 times a second, so advancing one capture frame per vblank plays 17% slow.
// Advance in 8.8 fixed point instead: 1.0 per vblank on NTSC, 1.2 on PAL, and
// drop frames when the accumulator carries. Set at boot from SYS_isPAL().
static u16 cartStep = 256;
static u16 cartAcc  = 0;
static u16 uiTick   = 0;

// -----------------------------------------------------------------------------
// Manual test-tone controls, kept from the original build.
// -----------------------------------------------------------------------------
void joyEvent(u16 joy, u16 changed, u16 state) {}

// X / Y / Z / A mute a voice.  Noise starts muted, as it always has: with no Lua
// feeding it, an enabled noise channel just blares.
static u8 chanEnable[4] = {1, 1, 1, 0};
static u16 prevButtons = 0;
static bool manualNoiseControl = FALSE;

// Decoded NES state for this frame.
static u16 p1Period, p2Period, triPeriod;
static u8  p1Vol, p2Vol, p1Duty, p2Duty, p1On, p2On, triOn;
static u8  noisePeriodIdx, noiseMode, noiseVol, noiseOn;
static u8  dpcmOn, nesFrame;
static u8  haveExt = 0;

// What each PSG channel is currently doing, so we only touch registers on change.
static u16 lastTone[3]  = {0xFFFF, 0xFFFF, 0xFFFF};
static u8  lastAtten[4] = {0xFF, 0xFF, 0xFF, 0xFF};
static u8  lastNoise    = 0xFF;
static u8  variantHold  = 0;

// Reported for the debug overlay.
#define TRI_OFF 0
#define TRI_HW  1
#define TRI_DAC 2
#define TRI_FM  3
static const char* const triName[4] = {"-  ", "HW ", "DAC", "FM "};

static u8 voiceIsDac[2] = {0, 0};
static u8 triSource = TRI_OFF;
static u8 noiseStoleCh2 = 0;

static void toneOut(u8 ch, u16 period)
{
    if (lastTone[ch] != period) { psgTone(ch, period); lastTone[ch] = period; }
}

static void attenOut(u8 ch, u8 att)
{
    if (lastAtten[ch] != att) { psgAtten(ch, att); lastAtten[ch] = att; }
}

// Writing the noise control register resets the shift register to its seed, so
// this guard is correctness, not economy: rewriting the same value every frame
// would restart the noise pattern 60 times a second and turn the drums into a
// 60 Hz buzz. Retriggering only on a real change is also what percussion wants
// -- each hit then starts from the same point in the sequence.
static void noiseOut(u8 white, u8 rate)
{
    u8 b = 0xE0 | (white ? 0x04 : 0) | (rate & 3);
    if (lastNoise != b) { psgWrite(b); lastNoise = b; }
}

// -----------------------------------------------------------------------------
// YM2612 channel 6 DAC, for DPCM.
// -----------------------------------------------------------------------------


// Part I's address port stays latched on 0x2A so the Z80 can feed the DAC with
// one store per sample.  Anything the 68000 writes through part I breaks that
// latch, so put it back.  The FM triangle lives on channel 5 -- part II -- for
// exactly this reason: its per-frame frequency writes never touch part I.
// Callers hold the bus.  Any part-I access moved the address latch; put it
// back on the DAC register so the Z80's PCM store keeps landing where it must.
static void ymRelatchDac(void)
{
    if (dpcmAvailable) *(vu8*)0xA04000 = 0x2A;
}

static void ymDacInit(void)
{
    ymDirect(0, 0x2B, 0x80);    // channel 6 becomes the DAC
    ymDirect(1, 0xB6, 0xC0);    // both speakers
    ymDirect(0, 0x28, 0x06);    // key off, the DAC does not need an envelope
    ymDirect(0, 0x2A, 0x80);    // sit at mid scale
    *(vu8*)0xA04000 = 0x2A;     // and leave part I latched on the DAC register
}

// -----------------------------------------------------------------------------
// The FM triangle.
//
// A triangle is odd harmonics at 1/n-squared, and algorithm 7 is four carriers
// in parallel -- so four operators at MUL 1, 3, 5, 7 with the right total levels
// *are* a triangle, additively, with no modulation involved.  Against the NES's
// own 32-step staircase that lands about 4 dB closer than the PSG volume DAC
// manages; against a clean triangle it is 16 dB closer.  The volume DAC's
// problem is structural: 2 dB attenuation steps cannot resolve a 16-level linear
// staircase near full scale, so five pairs of levels collapse and the peaks
// flatten.  FM's total level is 0.75 dB a step and its pitch is exact to a third
// of a cent anywhere from 27 Hz up.
//
// What this really buys is elsewhere, twice over.  The wave voice leaves the Z80
// loop, which shortens it from 233 cycles to 144 and lifts the pulse ceiling
// from 1920 Hz to 3107 Hz.  And PSG channel 2 goes free, so tone-clocked noise
// stops costing the triangle and every NES noise period is available at once.
//
// Caveat worth knowing: FM operators cannot be inverted, and a triangle's
// harmonics alternate in sign.  The magnitude spectrum matches; the scope trace
// will not look like a triangle.
// -----------------------------------------------------------------------------
#define FM_TRI_PART   1         // channels 4-6 live on part II
#define FM_TRI_IDX    1         // ...index 1 of that part is channel 5
#define FM_TRI_KEY    0x05      // channel code for register 0x28
// Added to every operator TL. Calibrated against the NES mixer, not guessed:
// with the linear-approx weights (0.00752/step pulse, 0.00851/step triangle)
// the capture's triangle-to-pulse1 RMS ratio should be 0.78; at level 8 the
// level was recalibrated twice: once against a queue-build bug that inflated
// the pulses, then against the fixed pulses. Current value measured against
// the capture: tri/pulse1 RMS 0.78 target, 0.75 dB a step.
// UP/DOWN trim this live outside manual-noise mode.
#define FM_TRI_LEVEL  23

// fnum at block 0 for a NES triangle period t is FM_TRI_K / (t + 1).
#define FM_TRI_K      2202010UL

static const u8 fmTriMul[4] = {1, 3, 5, 7};
static const u8 fmTriTL[4]  = {0, 26, 38, 47};   // 0.75 dB steps, fitted to the NES staircase

static u16 fmTriPeriod = 0xFFFF;
static u8  fmTriKeyed  = 0;
static u8  fmTriLevel  = FM_TRI_LEVEL;

static void ymTriangleLevel(void)
{
    u8 op;
    Z80_requestBus(TRUE);
    for (op = 0; op < 4; op++)
    {
        u16 tl = fmTriTL[op] + fmTriLevel;
        ymDirect(FM_TRI_PART, 0x40 + (op * 4) + FM_TRI_IDX, tl > 127 ? 127 : tl);
    }
    Z80_releaseBus();               // part II only: the 2A latch was never touched
}

static void ymTriangleInit(void)
{
    u8 op;
    for (op = 0; op < 4; op++)
    {
        u8 r = (op * 4) + FM_TRI_IDX;
        ymDirect(FM_TRI_PART, 0x30 + r, fmTriMul[op]);                 // DT 0, MUL
        ymDirect(FM_TRI_PART, 0x40 + r, fmTriTL[op] + fmTriLevel);     // total level
        ymDirect(FM_TRI_PART, 0x50 + r, 0x1F);                         // instant attack
        ymDirect(FM_TRI_PART, 0x60 + r, 0x00);                         // no decay
        ymDirect(FM_TRI_PART, 0x70 + r, 0x00);                         // no second decay
        ymDirect(FM_TRI_PART, 0x80 + r, 0x0F);                         // full sustain, fast release
        ymDirect(FM_TRI_PART, 0x90 + r, 0x00);                         // SSG-EG off
    }
    ymDirect(FM_TRI_PART, 0xB0 + FM_TRI_IDX, 0x07);  // no feedback, algorithm 7
    ymDirect(FM_TRI_PART, 0xB4 + FM_TRI_IDX, 0xC0);  // both speakers
    ymDirect(0, 0x28, FM_TRI_KEY);                   // key off
    *(vu8*)0xA04000 = 0x2A;                          // restore the DAC latch
    fmTriPeriod = 0xFFFF;
    fmTriKeyed = 0;
}

static void ymTriangleFreq(u16 nesPeriod)
{
    u32 fnum;
    u8 block = 0;

    if (nesPeriod == fmTriPeriod) return;
    fmTriPeriod = nesPeriod;

    fnum = FM_TRI_K / ((u32)nesPeriod + 1);
    while (fnum > 2047 && block < 7) { fnum >>= 1; block++; }
    if (fnum > 2047) fnum = 2047;

    // High byte first: the chip latches it and commits both on the low write.
    Z80_requestBus(TRUE);
    ymDirect(FM_TRI_PART, 0xA4 + FM_TRI_IDX, ((block & 7) << 3) | ((fnum >> 8) & 7));
    ymDirect(FM_TRI_PART, 0xA0 + FM_TRI_IDX, fnum & 0xFF);
    Z80_releaseBus();               // part II only
}

static void ymTriangleKey(u8 on)
{
    if (on == fmTriKeyed) return;
    fmTriKeyed = on;
    Z80_requestBus(TRUE);
    ymDirect(0, 0x28, on ? (0xF0 | FM_TRI_KEY) : FM_TRI_KEY);
    ymRelatchDac();                 // part I was touched: restore the PCM latch
    Z80_releaseBus();
}

// -----------------------------------------------------------------------------
// The NES triangle is a 32-step, 4-bit staircase, which is very nearly what a
// 4-bit logarithmic DAC wants to be handed.  What it cannot do is resolution at
// the top: 2 dB a step means the peaks quantise to about 0.2 of full scale
// where the NES steps by 1/15.  It still beats a square, and unlike the PSG's
// tone generator it goes below 109 Hz, which is where most NES bass lines live.
// -----------------------------------------------------------------------------
static void buildTriangleWave(void)
{
    u8 tab[32];
    u8 i;
    for (i = 0; i < 32; i++)
    {
        u8 level = (i < 16) ? (15 - i) : (i - 16);
        tab[i] = nesVolToAtten[level];
    }
    PSGDAC_loadWave(tab);
}

// -----------------------------------------------------------------------------
// Read whichever protocol version the Lua side is speaking this frame.
// -----------------------------------------------------------------------------
static void readApuBlock(void)
{
    haveExt = (APU_EXT[X_MAGIC] == APU_MAGIC);
    if (manualNoiseControl) noiseOn = 1;

    if (haveExt)
    {
        p1Period = APU_EXT[X_P1_LO] | (APU_EXT[X_P1_HI] << 8);
        p1Vol    = APU_EXT[X_P1_VOL] & 0x0F;
        p1Duty   = APU_EXT[X_P1_DUTY] & 3;
        p1On     = APU_EXT[X_P1_ON];
        p2Period = APU_EXT[X_P2_LO] | (APU_EXT[X_P2_HI] << 8);
        p2Vol    = APU_EXT[X_P2_VOL] & 0x0F;
        p2Duty   = APU_EXT[X_P2_DUTY] & 3;
        p2On     = APU_EXT[X_P2_ON];
        triPeriod = APU_EXT[X_TRI_LO] | (APU_EXT[X_TRI_HI] << 8);
        triOn     = APU_EXT[X_TRI_ON];
        if (!manualNoiseControl)
        {
            noisePeriodIdx = APU_EXT[X_NOI_PER] & 0x0F;
            noiseMode      = APU_EXT[X_NOI_MODE];
            noiseVol       = APU_EXT[X_NOI_VOL] & 0x0F;
            noiseOn        = APU_EXT[X_NOI_ON];
        }
        dpcmOn         = APU_EXT[X_DPCM_ON] | pcmPlaying;
        nesFrame       = APU_EXT[X_FRAME];
    }
    else
    {
        // v1 block: already converted to PSG periods and attenuations by the
        // old Lua.  Reconstruct just enough to drive the hardware path.
        u16 psg1 = APU_LEGACY[0] | (APU_LEGACY[1] << 8);
        u16 psg2 = APU_LEGACY[3] | (APU_LEGACY[4] << 8);
        u16 psgT = APU_LEGACY[10] | (APU_LEGACY[11] << 8);
        p1Period = psg1 ? psg1 - 1 : 0;
        p2Period = psg2 ? psg2 - 1 : 0;
        triPeriod = psgT ? (psgT / 2) : 0;
        p1On = psg1 > 0;  p2On = psg2 > 0;
        triOn = APU_LEGACY[13];
        p1Vol = 15 - (APU_LEGACY[2] & 0x0F);    // undo the old inversion
        p2Vol = 15 - (APU_LEGACY[5] & 0x0F);
        p1Duty = APU_LEGACY[8] & 3;
        p2Duty = 2;
        noiseMode = ((APU_LEGACY[6] >> 2) & 1) ? 0 : 1;
        noisePeriodIdx = 6;                     // v1 only carried a coarse rate
        noiseVol = 15 - (APU_LEGACY[7] & 0x0F);
        noiseOn = APU_LEGACY[7] != 15;
        dpcmOn = 0;
        nesFrame = APU_LEGACY[9];
    }
}

// Decode one frame of the embedded capture into the same variables the script
// path fills, so everything downstream is identical.
static u16 pcmFill(u8 *dst, u16 count)
{
    u16 i;
    for (i = 0; i < count; i++)
    {
        if (pcmPos < PCM_TEST_LEN) dst[i] = pcmTest[pcmPos++];
        else                       dst[i] = 0x80;
    }
    return count;
}

static void pcmStart(void)
{
    u16 i;
    if (!dpcmAvailable) return;
    pcmPos = 0;
    pcmSilence = 0;
    // Prime the whole ring so the reader has clean data wherever it is parked.
    for (i = 0; i < 16; i++) pcmFill(dpcmRing + i * 256, 256);
    pcmLastPage = psgdacDpcmPage;
    pcmPlaying = 1;
}

// Refill every page the reader has moved past since last frame.  Runs outside
// the bus window -- the ring lives in 68000 RAM, so no stall is involved.
static void pcmService(void)
{
    u8 page, cur;
    if (!pcmPlaying) return;

    cur = psgdacDpcmPage & 0x0F;
    for (page = pcmLastPage & 0x0F; page != cur; page = (page + 1) & 0x0F)
    {
        pcmFill(dpcmRing + (u16)page * 256, 256);
        if (pcmPos >= PCM_TEST_LEN) pcmSilence++;
    }
    pcmLastPage = psgdacDpcmPage;

    // One full ring of silence after the sample: the reader has definitely
    // consumed the tail, so stop claiming the V2D loop.
    if (pcmSilence >= 16) pcmPlaying = 0;
}

static void readCartFrame(void)
{
    const u8 *p = apuData + (u32)cartFrame * APU_DATA_STRIDE;

    u16 w;

    // Every 16-bit field lies at an even offset by design: the compiler folds
    // these byte pairs into native word loads, and the 68000 traps a word load
    // at an odd address. The first record layout had pulse 2's word at offset
    // 3 -- an address error on hardware, silent garbage on lenient emulators.
    w = (p[0] << 8) | p[1];
    p1Period = w & 0x7FF; p1Duty = (w >> 11) & 3; p1On = (w >> 13) & 1;

    w = (p[2] << 8) | p[3];
    p2Period = w & 0x7FF; p2Duty = (w >> 11) & 3; p2On = (w >> 13) & 1;

    w = (p[4] << 8) | p[5];
    triPeriod = w & 0x7FF; triOn = (w >> 11) & 1;

    p1Vol = p[6] & 0x0F;
    p2Vol = p[7] & 0x0F;

    if (!manualNoiseControl)
    {
        noisePeriodIdx = p[8] & 0x0F;
        noiseMode      = (p[8] >> 4) & 1;
        noiseOn        = (p[8] >> 5) & 1;
        noiseVol       = p[9] & 0x0F;
    }
    else noiseOn = 1;

    dpcmOn   = pcmPlaying;             // the capture has no PCM; C plays the test sample
    haveExt  = 1;                      // fields are NES-native, like v2
    nesFrame = cartFrame & 0xFF;

    cartAcc += cartStep;
    while (cartAcc >= 256)
    {
        cartAcc -= 256;
        cartFrame++;
        if (cartFrame >= APU_DATA_FRAMES) cartFrame = 0;
    }
}

static void clearApuBlock(void)
{
    u8 i;
    for (i = 0; i < 14; i++) APU_LEGACY[i] = 0;
    for (i = 0; i < X_COUNT; i++) APU_EXT[i] = 0;
}

// -----------------------------------------------------------------------------
// The allocator.  Everything above this point is mechanism; this is the policy.
// -----------------------------------------------------------------------------
static u16 pulseHz(u16 period) { return (u16)(111861UL / (period + 1)); }

static void synthUpdate(void)
{
    u8 variant, wantWave, headroom, fmTriangle, i;
    u8 stealCh2 = 0;
    u16 ceiling;
    u32 kPulse, kTri;

    u16 hz1 = pulseHz(p1Period);
    u16 hz2 = pulseHz(p2Period);

    // ---- decide who gets the noise generator's clock ------------------------
    // Tone-clocked noise buys 15 of 16 NES periods instead of 3, and costs
    // channel 2 -- which is the triangle.  Only spend that when the nearest
    // fixed rate is genuinely wrong.
    // Special noise mode: rate selector 3 clocks the shift register from tone
    // channel 2 instead of one of the three fixed dividers.
    //
    // In MODE_FM there is nothing to steal -- the triangle has left the PSG --
    // so use channel 2 for *every* period, not just the ones a fixed rate
    // misses. Two reasons beyond accuracy:
    //   - even the "close enough" fixed rates are up to 20% out (period 7);
    //     channel 2 is exact to about 1% everywhere.
    //   - writing the noise control register resets the shift register. A song
    //     alternating between a fixed rate and channel 2 rewrites that register
    //     on every switch, restarting the noise pattern each time. Staying on
    //     rate 3 keeps the control byte constant, so the LFSR runs undisturbed
    //     and only real note changes retrigger it.
    if (noiseOn && noiseVol && chanEnable[3])
    {
        if (synthMode == MODE_FM)           stealCh2 = 1;
        else if (synthMode == MODE_DAC_N &&
                 noiseNeedsCh2[noisePeriodIdx]) stealCh2 = 1;
    }

    // ---- pick a loop variant ------------------------------------------------
    // Sample rate is loop length, so this choice is a pitch ceiling.  A pulse
    // that needs duty above V3's ceiling is worth more than a DAC triangle,
    // because the hardware tone generator can still carry the triangle -- badly
    // above 109 Hz, not at all below it.
    fmTriangle = (synthMode == MODE_FM) && triOn && chanEnable[2] && triPeriod;
    wantWave = triOn && chanEnable[2] && !stealCh2 && !fmTriangle &&
               (synthMode != MODE_HW);
    headroom = (p1On && p1Duty != 2 && hz1 > psgdacCeiling[PSGDAC_V3] && hz1 <= psgdacCeiling[PSGDAC_V2]) ||
               (p2On && p2Duty != 2 && hz2 > psgdacCeiling[PSGDAC_V3] && hz2 <= psgdacCeiling[PSGDAC_V2]);

    if (dpcmOn && dpcmAvailable && synthMode != MODE_HW) variant = PSGDAC_V2D;
    else if (wantWave && !headroom)                      variant = PSGDAC_V3;
    else                                                 variant = PSGDAC_V2;

    // Do not let the variant flap frame to frame; each switch retunes every
    // voice, and an unstable triangle is worse than a slightly wrong ceiling.
    if (variant != psgdacVariant)
    {
        if (variantHold) { variant = psgdacVariant; variantHold--; }
        else             { variantHold = 4; }
    }

    ceiling = psgdacCeiling[variant];
    kPulse  = psgdacKPulse[variant];
    kTri    = psgdacKTriangle[variant];
    PSGDAC_setVariant(variant);
    PSGDAC_setDpcm(variant == PSGDAC_V2D && dpcmOn);

    // ---- pulses -------------------------------------------------------------
    for (i = 0; i < 2; i++)
    {
        u16 period = i ? p2Period : p1Period;
        u8  vol    = i ? p2Vol    : p1Vol;
        u8  duty   = i ? p2Duty   : p1Duty;
        u8  on     = i ? p2On     : p1On;
        u16 hz     = i ? hz2      : hz1;
        u8  att    = nesVolToAtten[vol];
        u8  useDac;

        if (!on || !vol || !period || !chanEnable[i])
        {
            PSGDAC_setPulse(i, 0, 0, 15, 0);
            attenOut(i, 15);
            voiceIsDac[i] = 0;
            continue;
        }

        // 50% duty is exactly what the tone generator already does, and it does
        // it without aliasing and at any pitch.  Only spend a DAC voice on the
        // duties the hardware cannot make, and only while the sample rate can
        // still resolve them.
        useDac = (synthMode != MODE_HW) && (duty != 2) && (hz <= ceiling);

        if (useDac)
        {
            u16 delta = (u16)(kPulse / (period + 1));
            PSGDAC_setPulse(i, delta, dutyThreshold[duty], att, 1);
            // Park at period 1, NOT 0: the classic SMS PCM trick. Period 1 is a
            // 55.9 kHz carrier -- ultrasonic on hardware, mean level V/2, and it
            // behaves identically on every chip revision and emulator. Period 0
            // does not: MAME-lineage cores toggle it at 112 kHz (same mean),
            // but older cores and some real silicon substitute 0x400, which is
            // an AUDIBLE 109 Hz square underneath every DAC voice. Hardware and
            // Gens r57shell agreed with each other and against PicoDrive here,
            // which is what betrayed it.
            toneOut(i, 1);
            lastAtten[i] = 0xFF;            // the Z80 owns this attenuator now
        }
        else
        {
            PSGDAC_setPulse(i, 0, 0, 15, 0);
            toneOut(i, period + 1);         // PSG clock is exactly 2x the NES's,
            attenOut(i, hwAtten(att));      // and it divides by 32 to the NES's 16
        }
        voiceIsDac[i] = useDac;
    }

    // ---- triangle: FM, the volume DAC, or the tone generator ----------------
    if (!fmTriangle) ymTriangleKey(0);

    if (fmTriangle)
    {
        // Off the PSG entirely. No pitch ceiling, no 109 Hz floor, no Z80 cycles,
        // and channel 2 is left for the noise generator to clock itself from.
        PSGDAC_setWave(0, 0);
        ymTriangleFreq(triPeriod);
        ymTriangleKey(1);
        if (!stealCh2) attenOut(2, 15);
        triSource = TRI_FM;
    }
    else if (stealCh2)
    {
        // Channel 2 is a clock now, not a voice.  Silence its output and hand
        // its period to the noise generator below.
        PSGDAC_setWave(0, 0);
        attenOut(2, 15);
        triSource = TRI_OFF;
    }
    else if (!triOn || !triPeriod || !chanEnable[2])
    {
        PSGDAC_setWave(0, 0);
        attenOut(2, 15);
        triSource = TRI_OFF;
    }
    else if (variant == PSGDAC_V3)
    {
        PSGDAC_setWave((u16)(kTri / (triPeriod + 1)), 1);
        toneOut(2, 1);                      // period 1, same reason as the pulses
        lastAtten[2] = 0xFF;                // wave table drives the attenuator
        triSource = TRI_DAC;
    }
    else
    {
        // Hardware fallback.  The PSG's period is 10 bits, so anything below
        // 109 Hz simply is not reachable here -- it pins at the floor, an
        // octave or more sharp.  That is the cost of not being in V3.
        u32 p = ((u32)triPeriod + 1) * 2;
        if (p > 1023) p = 1023;
        if (p < 1) p = 1;
        PSGDAC_setWave(0, 0);
        toneOut(2, (u16)p);
        attenOut(2, hwAtten(nesVolToAtten[12]));  // NES triangle has no volume control
        triSource = TRI_HW;
    }

    // ---- noise --------------------------------------------------------------
    if (noiseOn && noiseVol && chanEnable[3])
    {
        u8 white = (noiseMode == 0);        // NES mode 0 is the long sequence
        if (stealCh2)
        {
            // Period table depends on the mode. White matches the shift rate.
            // Periodic matches perceived pitch instead: the NES short sequence
            // is 93 steps to the PSG's 15, so matching the clock would put it
            // six octaves out. Period 0 is not usable as a clock, so the table
            // floors at 1 -- which is why NES period 0 lands an octave low
            // rather than exact. It is the closest the chip can get; the
            // nearest fixed rate is 32 times slower.
            u16 p = white ? noisePeriodWhite[noisePeriodIdx]
                          : noisePeriodPeriodic[noisePeriodIdx];
            toneOut(2, p);
            // Channel 2 still drives its own output while it clocks the noise,
            // and at the low end of the table that tone is audible (period 508
            // sits at 220 Hz). It has to be attenuated to silence explicitly.
            attenOut(2, 15);
            noiseOut(white, 3);
        }
        else
        {
            noiseOut(white, noiseFixedRate[noisePeriodIdx]);
        }
        attenOut(3, hwAtten(nesVolToAtten[noiseVol]));
    }
    else
    {
        attenOut(3, 15);
    }
    noiseStoleCh2 = stealCh2;
}

// -----------------------------------------------------------------------------
static void handleInput(u16 changed)
{
    if (changed & BUTTON_START)
    {
        synthMode = (synthMode + 1) % MODE_COUNT;
        fmTriPeriod = 0xFFFF;               // force the FM voice to re-tune
        // Anything could be parked in DC mode; force a full re-program.
        lastTone[0] = lastTone[1] = lastTone[2] = 0xFFFF;
        lastAtten[0] = lastAtten[1] = lastAtten[2] = lastAtten[3] = 0xFF;
        lastNoise = 0xFF;
    }

    if (changed & BUTTON_A) chanEnable[3] = !chanEnable[3];

    if (manualNoiseControl)
    {
        // Audition all 32 NES noise sounds by hand.  This is the fastest way to
        // hear what tone-clocked noise buys over the three fixed rates: flip
        // START between DAC and DAC+NOISE while holding a period.
        if (changed & BUTTON_C || changed & BUTTON_RIGHT) noisePeriodIdx = (noisePeriodIdx + 1) & 0x0F;
        if (changed & BUTTON_B || changed & BUTTON_LEFT)  noisePeriodIdx = (noisePeriodIdx + 15) & 0x0F;
        if (changed & BUTTON_Z) noiseMode = !noiseMode;
        if (changed & BUTTON_UP   && noiseVol < 15) noiseVol++;
        if (changed & BUTTON_DOWN && noiseVol > 0)  noiseVol--;
    }
    else
    {
        if (changed & BUTTON_B)
        {
            dataSource = !dataSource;
            cartFrame = 0;
        }
        if (changed & BUTTON_C) pcmStart();
        if (changed & BUTTON_UP   && fmTriLevel > 0)  { fmTriLevel--; ymTriangleLevel(); }
        if (changed & BUTTON_DOWN && fmTriLevel < 60) { fmTriLevel++; ymTriangleLevel(); }
        if (changed & BUTTON_X) chanEnable[0] = !chanEnable[0];
        if (changed & BUTTON_Y) chanEnable[1] = !chanEnable[1];
        if (changed & BUTTON_Z) chanEnable[2] = !chanEnable[2];
    }

    if (changed & BUTTON_MODE) manualNoiseControl = !manualNoiseControl;
}

static void drawUi(void)
{
    char t[64];
    u8 y = 10;

    if (dataSource == SRC_CART)
        sprintf(t, "MODE %-9s CART %4d/%d %ldfps", modeName[synthMode],
                cartFrame, APU_DATA_FRAMES, (long)SYS_getFPS());
    else
        sprintf(t, "MODE %-9s SCRIPT v%d %ldfps", modeName[synthMode],
                haveExt ? 2 : 1, (long)SYS_getFPS());
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y++;

    sprintf(t, "LOOP %-3s (%d cyc)  DPCM %s", variantName[psgdacVariant],
            psgdacCycles[psgdacVariant], dpcmAvailable ? (dpcmOn ? "ON " : "rdy") : "n/a");
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y += 2;

    sprintf(t, "P1 %4d Hz d%d v%2d %s", pulseHz(p1Period), p1Duty, p1Vol,
            !p1On ? "-  " : (voiceIsDac[0] ? "DAC" : "HW "));
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y++;

    sprintf(t, "P2 %4d Hz d%d v%2d %s", pulseHz(p2Period), p2Duty, p2Vol,
            !p2On ? "-  " : (voiceIsDac[1] ? "DAC" : "HW "));
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y++;

    if (triSource == TRI_FM)
        sprintf(t, "TR %4d Hz     FM  lvl%d", pulseHz(triPeriod) / 2, fmTriLevel);
    else
        sprintf(t, "TR %4d Hz     %s", pulseHz(triPeriod) / 2, triName[triSource]);
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y++;

    sprintf(t, "NZ p%2d %s v%2d %s", noisePeriodIdx,
            noiseMode ? "SHORT" : "LONG ", noiseVol,
            !noiseOn ? "-" : (noiseStoleCh2 ? "CH2-CLOCKED" : "FIXED"));
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y += 2;

    sprintf(t, "mute %c%c%c%c   nes frame %3d",
            chanEnable[0] ? '-' : '1', chanEnable[1] ? '-' : '2',
            chanEnable[2] ? '-' : 'T', chanEnable[3] ? '-' : 'N', nesFrame);
    VDP_clearText(2, y, 38); VDP_drawText(t, 2, y); y++;

    VDP_clearText(2, y, 38);
    VDP_drawText(manualNoiseControl ? "MANUAL NOISE  B/C period  Z mode"
                                    : "START mode B src C drum MODE noise", 2, y);
}

// SGDK calls main with a flag saying whether this was a cold boot; declaring it
// void mismatches the library's own prototype, which LTO rightly complains about.
int main(bool hardReset)
{
    u16 i;

    (void)hardReset;

    for (i = 0; i < 4096; i++) dpcmRing[i] = 0x80;

    // SGDK links RAM at 0xE0FF0000, a mirror of 0xFF0000 on the 68000's 24-bit
    // bus -- mask pointers down before comparing against bus addresses, or every
    // range check here silently fails and DPCM reports n/a.
    {
        u32 ring = (u32)dpcmRing & 0xFFFFFF;
        dpcmAvailable = (ring >= 0xFF1000) && ((ring + 4096) <= 0xFF8000) &&
                        ((ring & 0xFFF) == 0);
    }

    // One bus window for the whole of boot. PSGDAC_init pulses the Z80's reset
    // and then holds the bus: reset is already deasserted, so the YM2612 -- which
    // shares that reset line -- is awake and will keep what we write, while the
    // held bus keeps the Z80 stopped so the 68000 is the only master and may
    // write chips directly and busy-wait on the YM.
    PSGDAC_init((u32)dpcmRing & 0xFFFFFF);
    buildTriangleWave();
    for (i = 0; i < 4; i++) psgDirect(0x9F | (i << 5));   // silence all four channels
    if (dpcmAvailable) ymDacInit();
    ymTriangleInit();
    PSGDAC_start();                 // release the bus; the loop begins here

    // Publish the block so Lua can find it, magic last.
    apuBlock.ring  = dpcmAvailable ? ((u32)dpcmRing & 0xFFFFFF) : 0;
    apuBlock.self  = (u32)&apuBlock & 0xFFFFFF;
    apuBlock.magic = APU_BLOCK_MAGIC;

    cartStep = SYS_isPAL() ? 307 : 256;     // 1.2 or 1.0 capture frames a vblank

    JOY_setEventHandler(joyEvent);

    while (TRUE)
    {
        u16 joypad, changed;

        JOY_update();
        joypad  = JOY_readJoypad(JOY_1);
        changed = joypad & ~prevButtons;
        prevButtons = joypad;

        handleInput(changed);
        pcmService();
        if (dataSource == SRC_CART) readCartFrame();
        else                        readApuBlock();
        synthUpdate();      // fills a local command buffer; no bus, no chips

        // The only per-frame bus window: patch changed loop operands and copy
        // the frame's queued chip writes into the Z80's queue page. Plain byte
        // stores, no waits -- the stall is a few microseconds, and the Z80
        // itself replays the commands one per sample once it resumes.
        Z80_requestBus(TRUE);
        PSGDAC_flush();
        Z80_releaseBus();

        clearApuBlock();

        // The overlay is 9 lines of sprintf and VRAM writes -- far too heavy to
        // redraw every frame. Overrunning the frame budget makes the loop miss
        // vblanks, which is audible as slowed-down playback, so the music must
        // never wait for the UI.
        uiTick++;
        if ((uiTick & 7) == 0) drawUi();
        VDP_waitVSync();
    }

    return 0;
}
