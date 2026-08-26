// =============================================================================
// psgdac.h -- 68000 side of the PSG volume-DAC synthesis driver.
//
// The Z80 half (z80_psgdac.s80) free-runs a fixed-length loop doing only the
// sample-rate work: DAC pulse voices, the V3 wave voice, and PCM streaming.
// The 68000 writes everything register-rate itself -- the PSG directly at
// 0xC00011 (it is in the VDP, not on the Z80 bus), the YM2612 under a short
// bus hold.  What this file manages is the loop itself: uploading it, patching
// its immediate operands once a frame with the bus held, and switching which
// loop body runs by re-pointing the tail jumps.
// =============================================================================

#ifndef _PSGDAC_H_
#define _PSGDAC_H_

#include <genesis.h>
#include <z80_ctrl.h>
#include "psgdac_z80.h"

// ---- loop variants ---------------------------------------------------------
// Sample rate is loop length, so the variant you pick *is* your pitch ceiling.
#define PSGDAC_V3       0   // pulse A + pulse B + wave C
#define PSGDAC_V2       1   // pulse A + pulse B      (triangle on FM, or hardware)
#define PSGDAC_V2D      2   // pulse A + pulse B + PCM
#define PSGDAC_V1       3   // one pulse; the other pulse is on the YM2612
#define PSGDAC_VW       4   // wave voice alone; both pulses are on the YM2612
#define PSGDAC_NVARIANT 5

#define PSGDAC_PULSE_A  0
#define PSGDAC_PULSE_B  1

// Z80-side addresses the driver was assembled with.
#define Z80_DUMMY       0x0204          // 13-cycle store that goes nowhere
#define Z80_DPCMPAGE    0x0205          // page the PCM reader is in (driver-owned)
#define Z80_WAVE        0x0400          // 256-byte wave table, page-aligned
#define Z80_PSG_PORT    0x7F11
#define Z80_YM_DATA1    0x4001          // YM2612 part I data: the PCM store target
#define Z80_MEM(a)      ((vu8*)(0xA00000 + (a)))   // SGDK owns the name Z80_RAM

// The mute byte a voice adds to its scaled span: it names the PSG channel the
// voice writes, and in the log domain doubles as the volume offset.
#define Z80_MUTE_A      0x9F            // latch channel 0 volume, attenuation 15
#define Z80_MUTE_B      0xBF            // latch channel 1 volume, attenuation 15

// Loop lengths, straight out of the assembler.
static const u16 psgdacCycles[PSGDAC_NVARIANT] =
    {CYCLES_v3, CYCLES_v2, CYCLES_v2d, CYCLES_v1, CYCLES_vw};
static const u16 psgdacRate[PSGDAC_NVARIANT]   = {15363, 24858, 20455, 49035, 36157};

// delta = K / (nesPeriod + 1).  K = nesClock * 65536 / (divider * sampleRate).
static const u32 psgdacKPulse[PSGDAC_NVARIANT]    = {477184, 294912, 358400, 149504, 202752};
static const u32 psgdacKTriangle[PSGDAC_NVARIANT] = {238592, 147456, 179200, 74752, 101376};

// Highest fundamental worth handing to a DAC voice: below this a 12.5% pulse
// still gets its 8 slots per period.  Above it the hardware tone generator wins.
static const u16 psgdacCeiling[PSGDAC_NVARIANT] =
    {15363 / 8, 24858 / 8, 20455 / 8, 49035 / 8, 36157 / 8};

// How many voices of each kind a variant's loop body really contains.  Patching
// a voice a body does not have would write over another body's instructions, so
// every loop over the patch tables is bounded by these.
static const u8 psgdacPulseVoices[PSGDAC_NVARIANT] = {2, 2, 2, 1, 0};
static const u8 psgdacWaveVoice[PSGDAC_NVARIANT]   = {1, 0, 0, 0, 1};

// ---- patch point tables ----------------------------------------------------
// Every patch label names an instruction; its operand starts one byte later.
static const u16 pchDelta[2][PSGDAC_NVARIANT] = {
    {P_v3_a_delta + 1, P_v2_a_delta + 1, P_v2d_a_delta + 1, P_v1_a_delta + 1, 0},
    {P_v3_b_delta + 1, P_v2_b_delta + 1, P_v2d_b_delta + 1, P_v1_a_delta + 1, 0},
};
static const u16 pchDuty[2][PSGDAC_NVARIANT] = {
    {P_v3_a_duty + 1, P_v2_a_duty + 1, P_v2d_a_duty + 1, P_v1_a_duty + 1, 0},
    {P_v3_b_duty + 1, P_v2_b_duty + 1, P_v2d_b_duty + 1, P_v1_a_duty + 1, 0},
};
static const u16 pchSpan[2][PSGDAC_NVARIANT] = {
    {P_v3_a_span + 1, P_v2_a_span + 1, P_v2d_a_span + 1, P_v1_a_span + 1, 0},
    {P_v3_b_span + 1, P_v2_b_span + 1, P_v2d_b_span + 1, P_v1_a_span + 1, 0},
};
static const u16 pchOut[2][PSGDAC_NVARIANT] = {
    {P_v3_a_out + 1, P_v2_a_out + 1, P_v2d_a_out + 1, P_v1_a_out + 1, 0},
    {P_v3_b_out + 1, P_v2_b_out + 1, P_v2d_b_out + 1, P_v1_a_out + 1, 0},
};
// The wave voice lives in two bodies, at different addresses in each.
static const u16 pchWaveDelta[PSGDAC_NVARIANT] = {P_v3_c_delta + 1, 0, 0, 0, P_vw_c_delta + 1};
static const u16 pchWaveOut[PSGDAC_NVARIANT]   = {P_v3_c_out + 1,   0, 0, 0, P_vw_c_out + 1};
static const u16 pchWavePage[PSGDAC_NVARIANT]  = {P_v3_c_page + 1,  0, 0, 0, P_vw_c_page + 1};

static const u16 psgdacEntry[PSGDAC_NVARIANT] = {L_v3, L_v2, L_v2d, L_v1, L_vw};
// Every loop body's tail jump, plus V2D's out-of-line page-wrap tail.  All are
// re-pointed together on a variant switch, with the bus held, so it does not
// matter which body the stopped Z80 happens to be inside.
#define PSGDAC_NTAIL 6
static const u16 pchNext[PSGDAC_NTAIL] = {P_v3_next + 1, P_v2_next + 1, P_v2d_next + 1,
                                          P_v2d_wrapnext + 1, P_v1_next + 1, P_vw_next + 1};

// ---- shadow state ----------------------------------------------------------
typedef struct {
    u16 delta;
    u8  duty;       // phase-high threshold: 32 = 12.5%, 64 = 25%, 128 = 50%, 192 = 75%
    u8  atten;      // 0 loudest .. 15 silent
    u8  on;
} DacVoice;

static DacVoice psgdacVoice[3];         // 0,1 = pulses; 2 = wave (duty unused)
static DacVoice psgdacSent[3];
static u8 psgdacVariant  = PSGDAC_V3;
static u8 psgdacSentVar  = 0xFF;
static u8 psgdacDpcmOn   = 0;
static u8 psgdacDpcmSent = 0xFF;
static u8 psgdacReady    = 0;
static u8 psgdacDpcmPage = 0;           // reader's current ring page, from the flush
static u8 psgdacSolo     = 0;           // in V1, which pulse owns the single voice
static u8 psgdacSoloSent = 0xFF;

static inline void z80w8(u16 addr, u8 v)   { *Z80_MEM(addr) = v; }
static inline void z80w16(u16 addr, u16 v) { *Z80_MEM(addr) = v & 0xFF;
                                             *Z80_MEM(addr + 1) = v >> 8; }
static inline u8   z80r8(u16 addr)         { return *Z80_MEM(addr); }

// The 'and' operand that makes mute + span come out as 0x90|attenuation.  In the
// log domain, scaling a waveform is just adding a constant, which is the reason
// a whole pulse voice fits in eight instructions with no table at all.
static inline u8 psgdacSpanByte(u8 atten) { return (u8)((atten - 0x0F) & 0xFF); }

// ---- lifecycle -------------------------------------------------------------

// Pulse the Z80's reset, upload the driver, and RETURN WITH THE BUS STILL HELD.
// Reset is deasserted before the upload, which matters because the Z80 reset
// line also resets the YM2612 -- anything written to the FM chip while it is
// asserted is lost. Holding the bus keeps the Z80 stopped regardless, so the
// caller can initialise the sound chips directly, then call PSGDAC_start().
static void PSGDAC_init(u32 dpcmRingBase)
{
    u16 i;

    Z80_unloadDriver();                         // make sure no SGDK driver is resident

    Z80_requestBus(TRUE);
    Z80_startReset();
    Z80_endReset();     // Z80 wants to run, but we hold the bus; YM now awake

    for (i = 0; i < PSGDAC_Z80_SIZE; i++)
        *Z80_MEM(i) = psgdac_z80[i];
    for (i = 0; i < 256; i++)                   // silence until a wave is loaded
        *Z80_MEM(Z80_WAVE + i) = 0xDF;          // ch2 latch, attenuation 15

    // Point the PCM reader at the ring the 68000 actually reserved.  The bank
    // window maps 68000 0xFF0000 to Z80 0x8000, so this is pure arithmetic.
    // Must be 4 KB aligned, inside the window, and clear of the control block.
    if (dpcmRingBase >= 0xFF1000 && (dpcmRingBase + 4096) <= 0xFF8000 &&
        (dpcmRingBase & 0xFFF) == 0)
    {
        u16 z80base = 0x8000 + (u16)(dpcmRingBase - 0xFF0000);
        z80w16(P_dpcm_base + 1, z80base);
        z80w8(P_dpcm_page + 1, z80base >> 8);
        z80w8(Z80_DPCMPAGE, z80base >> 8);      // reader starts at the ring base
    }

    // All voices start silent and detached from the PSG.
    for (i = 0; i < 2; i++)
    {
        u16 v;
        for (v = 0; v < PSGDAC_NVARIANT; v++)
        {
            if (i >= psgdacPulseVoices[v]) continue;
            z80w16(pchDelta[i][v], 0);
            z80w8(pchDuty[i][v], 0);
            z80w8(pchSpan[i][v], psgdacSpanByte(15));
            z80w16(pchOut[i][v], Z80_DUMMY);
        }
    }
    for (i = 0; i < PSGDAC_NVARIANT; i++)
    {
        if (!psgdacWaveVoice[i]) continue;
        z80w16(pchWaveDelta[i], 0);
        z80w8(pchWavePage[i], Z80_WAVE >> 8);
        z80w16(pchWaveOut[i], Z80_DUMMY);
    }
    z80w16(P_v2d_d_out + 1, Z80_DUMMY);
    z80w16(P_entry + 1, psgdacEntry[PSGDAC_V3]);
    for (i = 0; i < PSGDAC_NTAIL; i++) z80w16(pchNext[i], psgdacEntry[PSGDAC_V3]);

    for (i = 0; i < 3; i++)
    {
        psgdacVoice[i].delta = 0; psgdacVoice[i].duty = 0;
        psgdacVoice[i].atten = 15; psgdacVoice[i].on = 0;
        psgdacSent[i] = psgdacVoice[i];
    }
    psgdacVariant = PSGDAC_V3;
    psgdacSentVar = PSGDAC_V3;
}

// Release the bus held since PSGDAC_init. The loop starts here, and from this
// point the Z80 owns every chip register.
static void PSGDAC_start(void)
{
    psgdacReady = 1;
    Z80_releaseBus();
}

// Fill the 256-byte wave table.  One page = eight copies of a 32-step waveform,
// so the inner loop can index it with the raw phase high byte and no shifting.
// Entries are finished PSG bytes for channel 2 (0xD0 | attenuation).
// Call between PSGDAC_init and PSGDAC_start, while the bus is already held.
static void PSGDAC_loadWave(const u8 *atten32)
{
    u16 i;
    for (i = 0; i < 256; i++)
        *Z80_MEM(Z80_WAVE + i) = 0xD0 | (atten32[i >> 3] & 0x0F);
}

// ---- per-frame parameter setting (call, then flush) ------------------------

static inline void PSGDAC_setVariant(u8 v)        { psgdacVariant = v; }

// In V1 only one pulse voice exists; this names which of the two the loop runs.
// The other pulse is being played by the YM2612 (or by the tone generator).
static inline void PSGDAC_setSolo(u8 idx)         { psgdacSolo = idx & 1; }
static inline void PSGDAC_setDpcm(u8 on)          { psgdacDpcmOn = on; }

static inline void PSGDAC_setPulse(u8 idx, u16 delta, u8 duty, u8 atten, u8 on)
{
    psgdacVoice[idx].delta = delta;
    psgdacVoice[idx].duty  = duty;
    psgdacVoice[idx].atten = atten;
    psgdacVoice[idx].on    = on;
}

static inline void PSGDAC_setWave(u16 delta, u8 on)
{
    psgdacVoice[2].delta = delta;
    psgdacVoice[2].on    = on;
}

// Push everything into the running loop: changed operands, then the frame's
// queued chip writes.  The Z80 bus must be held.  Nothing here waits on
// anything, so the window is a burst of byte stores and nothing else.
static void PSGDAC_flush(void)
{
    u8 v = psgdacVariant;
    u8 all = (v != psgdacSentVar) ||
             (v == PSGDAC_V1 && psgdacSolo != psgdacSoloSent);
    u8 i;

    if (!psgdacReady) return;

    // V1's one voice serves whichever pulse stayed on the PSG, so it is the
    // mute byte -- the channel the voice writes -- that gets patched with it.
    if (v == PSGDAC_V1 && all)
    {
        z80w8(P_v1_a_mute + 1, psgdacSolo ? Z80_MUTE_B : Z80_MUTE_A);
        psgdacSoloSent = psgdacSolo;
    }

    for (i = 0; i < 2; i++)
    {
        DacVoice *now = &psgdacVoice[i];
        DacVoice *was = &psgdacSent[i];

        if (i >= psgdacPulseVoices[v]) continue;           // no voice to write to
        if (v == PSGDAC_V1 && i != psgdacSolo) continue;   // ...nor for this pulse
        if (all || now->delta != was->delta) z80w16(pchDelta[i][v], now->delta);
        if (all || now->duty  != was->duty ) z80w8(pchDuty[i][v], now->duty);
        if (all || now->atten != was->atten) z80w8(pchSpan[i][v], psgdacSpanByte(now->atten));
        if (all || now->on    != was->on   )
            z80w16(pchOut[i][v], now->on ? Z80_PSG_PORT : Z80_DUMMY);
        *was = *now;
    }

    if (psgdacWaveVoice[v])
    {
        DacVoice *now = &psgdacVoice[2];
        DacVoice *was = &psgdacSent[2];
        if (all || now->delta != was->delta) z80w16(pchWaveDelta[v], now->delta);
        if (all || now->on    != was->on   )
            z80w16(pchWaveOut[v], now->on ? Z80_PSG_PORT : Z80_DUMMY);
        *was = *now;
    }

    if (v == PSGDAC_V2D && (all || psgdacDpcmOn != psgdacDpcmSent))
    {
        z80w16(P_v2d_d_out + 1, psgdacDpcmOn ? Z80_YM_DATA1 : Z80_DUMMY);
        psgdacDpcmSent = psgdacDpcmOn;
    }

    // Re-point every tail jump at the running variant.  The Z80 is stopped, so
    // all of them land before it takes another branch.
    if (all)
    {
        for (i = 0; i < PSGDAC_NTAIL; i++) z80w16(pchNext[i], psgdacEntry[v]);
        psgdacSentVar = v;
    }

    // Grab the PCM reader's position while we already hold the bus, so the
    // refill logic outside the window knows which pages are behind the reader.
    psgdacDpcmPage = z80r8(Z80_DPCMPAGE);
}

#endif // _PSGDAC_H_
