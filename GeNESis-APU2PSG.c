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
//   2. VOLUME DAC      -- park a channel's period at 0 so it outputs DC, then
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

// -----------------------------------------------------------------------------
// PSG register access.  Raw bytes rather than SGDK's helpers, because the volume
// DAC needs exact control of what lands in the attenuator and when.
// -----------------------------------------------------------------------------
#define PSG_PORT ((vu8*)0xC00011)

static inline void psgWrite(u8 b) { *PSG_PORT = b; }

static void psgTone(u8 ch, u16 period)
{
    psgWrite(0x80 | (ch << 5) | (period & 0x0F));
    psgWrite((period >> 4) & 0x3F);
}

static inline void psgAtten(u8 ch, u8 att) { psgWrite(0x90 | (ch << 5) | (att & 0x0F)); }

// -----------------------------------------------------------------------------
// NES -> PSG conversion tables.  Regenerate with tools/gen_tables.py.
// -----------------------------------------------------------------------------

// NES volume is linear 0..15.  PSG attenuation is logarithmic, 2 dB a step.  The
// old 15-v mapping treated one as the other, which is why envelope tails used to
// dive off a cliff: NES volume 8 is 2.5 dB down, not 14 dB down.
static const u8 nesVolToAtten[16] = {15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0};

// Duty as a threshold on the phase accumulator's high byte.
static const u8 dutyThreshold[4] = {32, 64, 128, 192};   // 12.5, 25, 50, 75 %

// Channel-2 tone period that clocks the noise generator at the NES rate.
// White (long) mode matches the shift rate; periodic (short) mode matches
// perceived pitch instead, because the NES's short sequence is 93 steps to the
// PSG's 15 and matching the clock would put it six octaves out.
static const u16 noisePeriodWhite[16]    = {1, 1, 2, 4, 8, 12, 16, 20, 25, 32, 47, 63, 95, 127, 254, 508};
static const u16 noisePeriodPeriodic[16] = {3, 6, 12, 25, 50, 74, 99, 124, 157, 197, 294, 394, 591, 787, 1023, 1023};

// Nearest of the PSG's three fixed noise rates, and whether that nearest is more
// than 25% out.  Only indices 6, 7, 9 and 11 are close; the other twelve need
// channel 2.  That is the whole answer to "can we do all 32 noise sounds".
static const u8 noiseFixedRate[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2};
static const u8 noiseNeedsCh2[16]  = {1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1};

// -----------------------------------------------------------------------------
// Shared memory protocol with the Lua side.
// -----------------------------------------------------------------------------
#define APU_LEGACY  ((volatile u8*)0xFF0000)    // v1 block, PSG-native, still honoured
#define APU_EXT     ((volatile u8*)0xFF0010)    // v2 block, NES-native
#define APU_MAGIC   0x47                        // 'G' -- v2 present this frame
#define APU_RINGPTR ((volatile u32*)0xFF002C)   // ROM publishes the PCM ring here

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
#define MODE_COUNT   3

static const char* const modeName[MODE_COUNT] = {"HW", "DAC", "DAC+NOISE"};
static const char* const variantName[PSGDAC_NVARIANT] = {"V3", "V2", "V2D"};

static u8 synthMode = MODE_DAC;
static u8 dpcmAvailable = 0;

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
static u8 voiceIsDac[3] = {0, 0, 0};
static u8 noiseStoleCh2 = 0;

static void toneOut(u8 ch, u16 period)
{
    if (lastTone[ch] != period) { psgTone(ch, period); lastTone[ch] = period; }
}

static void attenOut(u8 ch, u8 att)
{
    if (lastAtten[ch] != att) { psgAtten(ch, att); lastAtten[ch] = att; }
}

static void noiseOut(u8 white, u8 rate)
{
    u8 b = 0xE0 | (white ? 0x04 : 0) | (rate & 3);
    if (lastNoise != b) { psgWrite(b); lastNoise = b; }
}

// -----------------------------------------------------------------------------
// YM2612 channel 6 DAC, for DPCM.
// -----------------------------------------------------------------------------
static void ymWrite(u8 part, u8 reg, u8 val)
{
    vu8* addr = (vu8*)(0xA04000 + (part * 2));
    vu8* data = (vu8*)(0xA04001 + (part * 2));
    while (*addr & 0x80) ;
    *addr = reg;
    while (*addr & 0x80) ;
    *data = val;
}

static void ymDacInit(void)
{
    ymWrite(0, 0x2B, 0x80);     // channel 6 becomes the DAC
    ymWrite(1, 0xB6, 0xC0);     // both speakers
    ymWrite(0, 0x28, 0x06);     // key off, the DAC does not need an envelope
    ymWrite(0, 0x2A, 0x80);     // sit at mid scale
    // Latch the DAC register and leave it latched: from here on the Z80 feeds
    // samples with a single store per sample to the data port.
    *(vu8*)0xA04000 = 0x2A;
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
        dpcmOn         = APU_EXT[X_DPCM_ON];
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
    u8 variant, wantWave, headroom, i;
    u8 stealCh2 = 0;
    u16 ceiling;
    u32 kPulse, kTri;

    u16 hz1 = pulseHz(p1Period);
    u16 hz2 = pulseHz(p2Period);

    // ---- decide who gets the noise generator's clock ------------------------
    // Tone-clocked noise buys 15 of 16 NES periods instead of 3, and costs
    // channel 2 -- which is the triangle.  Only spend that when the nearest
    // fixed rate is genuinely wrong.
    if (synthMode == MODE_DAC_N && noiseOn && noiseVol && chanEnable[3] &&
        noiseNeedsCh2[noisePeriodIdx])
        stealCh2 = 1;

    // ---- pick a loop variant ------------------------------------------------
    // Sample rate is loop length, so this choice is a pitch ceiling.  A pulse
    // that needs duty above V3's ceiling is worth more than a DAC triangle,
    // because the hardware tone generator can still carry the triangle -- badly
    // above 109 Hz, not at all below it.
    wantWave = triOn && chanEnable[2] && !stealCh2 && (synthMode != MODE_HW);
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
            toneOut(i, 0);                  // period 0 -> DC, the DAC does the rest
            lastAtten[i] = 0xFF;            // the Z80 owns this attenuator now
        }
        else
        {
            PSGDAC_setPulse(i, 0, 0, 15, 0);
            toneOut(i, period + 1);         // PSG clock is exactly 2x the NES's,
            attenOut(i, att);               // and it divides by 32 to the NES's 16
        }
        voiceIsDac[i] = useDac;
    }

    // ---- triangle, or whatever is left of channel 2 --------------------------
    if (stealCh2)
    {
        // Channel 2 is a clock now, not a voice.  Silence its output and hand
        // its period to the noise generator below.
        PSGDAC_setWave(0, 0);
        attenOut(2, 15);
        voiceIsDac[2] = 0;
    }
    else if (!triOn || !triPeriod || !chanEnable[2])
    {
        PSGDAC_setWave(0, 0);
        attenOut(2, 15);
        voiceIsDac[2] = 0;
    }
    else if (variant == PSGDAC_V3)
    {
        PSGDAC_setWave((u16)(kTri / (triPeriod + 1)), 1);
        toneOut(2, 0);
        lastAtten[2] = 0xFF;                // wave table drives the attenuator
        voiceIsDac[2] = 1;
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
        attenOut(2, nesVolToAtten[12]);     // NES triangle has no volume control
        voiceIsDac[2] = 0;
    }

    // ---- noise --------------------------------------------------------------
    if (noiseOn && noiseVol && chanEnable[3])
    {
        u8 white = (noiseMode == 0);        // NES mode 0 is the long sequence
        if (stealCh2)
        {
            u16 p = white ? noisePeriodWhite[noisePeriodIdx]
                          : noisePeriodPeriodic[noisePeriodIdx];
            toneOut(2, p);
            noiseOut(white, 3);             // rate 3 = clocked by channel 2
        }
        else
        {
            noiseOut(white, noiseFixedRate[noisePeriodIdx]);
        }
        attenOut(3, nesVolToAtten[noiseVol]);
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

    sprintf(t, "MODE %-9s  PROTO v%d  %5d Hz", modeName[synthMode],
            haveExt ? 2 : 1, psgdacRate[psgdacVariant]);
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

    sprintf(t, "TR %4d Hz     %s", pulseHz(triPeriod) / 2,
            !triOn ? "-  " : (voiceIsDac[2] ? "DAC" : (noiseStoleCh2 ? "OFF" : "HW ")));
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
                                    : "START mode  MODE noise-audition", 2, y);
}

int main(void)
{
    u16 i;

    for (i = 0; i < 4096; i++) dpcmRing[i] = 0x80;

    Z80_requestBus(TRUE);
    PSG_reset();
    Z80_releaseBus();

    PSGDAC_init((u32)dpcmRing);
    buildTriangleWave();

    dpcmAvailable = ((u32)dpcmRing >= 0xFF1000) &&
                    (((u32)dpcmRing + 4096) <= 0xFF8000) &&
                    ((((u32)dpcmRing) & 0xFFF) == 0);
    if (dpcmAvailable)
    {
        Z80_requestBus(TRUE);
        ymDacInit();
        Z80_releaseBus();
    }

    // Tell the Lua side where to put PCM, so it can write 68000 RAM directly and
    // the samples never have to cross the Z80 bus.
    *APU_RINGPTR = dpcmAvailable ? (u32)dpcmRing : 0;

    JOY_setEventHandler(joyEvent);

    while (TRUE)
    {
        u16 joypad, changed;

        JOY_update();
        joypad  = JOY_readJoypad(JOY_1);
        changed = joypad & ~prevButtons;
        prevButtons = joypad;

        handleInput(changed);
        readApuBlock();
        synthUpdate();

        // One short window with the Z80 stopped: PSG registers first, then the
        // loop's immediate operands.  Only changed bytes are written, so this is
        // typically a handful of stores rather than a hole in the audio.
        Z80_requestBus(TRUE);
        PSGDAC_flush();
        Z80_releaseBus();

        clearApuBlock();
        *APU_RINGPTR = dpcmAvailable ? (u32)dpcmRing : 0;

        drawUi();
        VDP_waitVSync();
    }

    return 0;
}
