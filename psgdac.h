// =============================================================================
// psgdac.h -- 68000 side of the PSG volume-DAC synthesis driver.
//
// The Z80 half (z80_psgdac.s80) free-runs a fixed-length loop that rewrites PSG
// attenuation registers thousands of times a second, turning the chip's 4-bit
// logarithmic attenuator into a waveform generator.  Everything here exists to
// keep that loop fed without ever stalling it for long: parameters are poked
// into the loop as immediate operands, and only the bytes that actually changed
// get written, because every write happens with the Z80 held off the bus.
//
// A voice is either DAC (true NES duty / triangle staircase, pitch-limited by
// the loop's sample rate) or hardware (the PSG's own tone generator: pitch-exact
// anywhere, but 50% duty only).  The caller decides per frame; see the crossover
// policy in GeNESis-APU2PSG.c.
// =============================================================================

#ifndef _PSGDAC_H_
#define _PSGDAC_H_

#include <genesis.h>
#include <z80_ctrl.h>
#include "psgdac_z80.h"

// ---- loop variants ---------------------------------------------------------
// Sample rate is loop length, so the variant you pick *is* your pitch ceiling.
#define PSGDAC_V3       0   // pulse A + pulse B + wave C
#define PSGDAC_V2       1   // pulse A + pulse B          (triangle -> hardware tone)
#define PSGDAC_V2D      2   // pulse A + pulse B + PCM    (triangle -> hardware tone)
#define PSGDAC_NVARIANT 3

#define PSGDAC_PULSE_A  0
#define PSGDAC_PULSE_B  1

// Z80-side addresses the driver was assembled with.
#define Z80_DUMMY       0x0304          // 13-cycle store that goes nowhere
#define Z80_WAVE        0x0400          // 256-byte wave table, page-aligned
#define Z80_PSG_PORT    0x7F11
#define Z80_YM_DATA     0x4001          // YM2612 part I data (reg 2Ah stays latched)
#define Z80_RAM(a)      ((vu8*)(0xA00000 + (a)))

// Loop lengths, straight out of the assembler, and what they buy.
static const u16 psgdacCycles[PSGDAC_NVARIANT] = {CYCLES_v3, CYCLES_v2, CYCLES_v2d};
static const u16 psgdacRate[PSGDAC_NVARIANT]   = {15363, 24858, 20455};

// delta = K / (nesPeriod + 1).  K = nesClock * 65536 / (divider * sampleRate).
static const u32 psgdacKPulse[PSGDAC_NVARIANT]    = {477184, 294912, 358400};
static const u32 psgdacKTriangle[PSGDAC_NVARIANT] = {238592, 147456, 179200};

// Highest fundamental worth handing to a DAC voice: below this a 12.5% pulse
// still gets its 8 slots per period.  Above it the hardware tone generator wins.
static const u16 psgdacCeiling[PSGDAC_NVARIANT] = {15363 / 8, 24858 / 8, 20455 / 8};

// ---- patch point tables ----------------------------------------------------
// Every patch label names an instruction; its operand starts one byte later.
static const u16 pchDelta[2][PSGDAC_NVARIANT] = {
    {P_v3_a_delta + 1, P_v2_a_delta + 1, P_v2d_a_delta + 1},
    {P_v3_b_delta + 1, P_v2_b_delta + 1, P_v2d_b_delta + 1},
};
static const u16 pchDuty[2][PSGDAC_NVARIANT] = {
    {P_v3_a_duty + 1, P_v2_a_duty + 1, P_v2d_a_duty + 1},
    {P_v3_b_duty + 1, P_v2_b_duty + 1, P_v2d_b_duty + 1},
};
static const u16 pchSpan[2][PSGDAC_NVARIANT] = {
    {P_v3_a_span + 1, P_v2_a_span + 1, P_v2d_a_span + 1},
    {P_v3_b_span + 1, P_v2_b_span + 1, P_v2d_b_span + 1},
};
static const u16 pchOut[2][PSGDAC_NVARIANT] = {
    {P_v3_a_out + 1, P_v2_a_out + 1, P_v2d_a_out + 1},
    {P_v3_b_out + 1, P_v2_b_out + 1, P_v2d_b_out + 1},
};
static const u16 pchNext[PSGDAC_NVARIANT] = {P_v3_next + 1, P_v2_next + 1, P_v2d_next + 1};
static const u16 psgdacEntry[PSGDAC_NVARIANT] = {L_v3, L_v2, L_v2d};

// ---- shadow state ----------------------------------------------------------
// Patching costs a Z80 stall, so nothing is written twice.  Only the running
// variant is kept current; switching variants flushes everything into the new
// one first, which is free because the Z80 is stopped anyway at that moment.
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

static inline void z80w8(u16 addr, u8 v)   { *Z80_RAM(addr) = v; }
static inline void z80w16(u16 addr, u16 v) { *Z80_RAM(addr) = v & 0xFF;
                                             *Z80_RAM(addr + 1) = v >> 8; }

// The 'and' operand that makes mute + span come out as 0x90|attenuation.  In the
// log domain, scaling a waveform is just adding a constant, which is the reason
// a whole pulse voice fits in eight instructions with no table at all.
static inline u8 psgdacSpanByte(u8 atten) { return (u8)((atten - 0x0F) & 0xFF); }

// ---- lifecycle -------------------------------------------------------------

// Upload the driver and start it.  Must be called with the Z80 bus NOT held.
static void PSGDAC_init(u32 dpcmRingBase)
{
    u16 i;

    Z80_loadDriver(Z80_DRIVER_NULL, FALSE);     // stop SGDK's own driver first

    Z80_requestBus(TRUE);
    Z80_startReset();

    for (i = 0; i < PSGDAC_Z80_SIZE; i++)
        *Z80_RAM(i) = psgdac_z80[i];
    for (i = 0; i < 256; i++)                   // silence until a wave is loaded
        *Z80_RAM(Z80_WAVE + i) = 0xDF;          // ch2 latch, attenuation 15

    // Point the PCM reader at the ring the 68000 actually reserved.  The bank
    // window maps 68000 0xFF0000 to Z80 0x8000, so this is pure arithmetic.
    // Must be 4 KB aligned, inside the bank window, and clear of the shared
    // control block at 0xFF0000.  Same test the ROM uses to decide whether to
    // offer DPCM at all, so the two can never disagree.
    if (dpcmRingBase >= 0xFF1000 && (dpcmRingBase + 4096) <= 0xFF8000 &&
        (dpcmRingBase & 0xFFF) == 0)
    {
        u16 z80base = 0x8000 + (u16)(dpcmRingBase - 0xFF0000);
        z80w16(P_dpcm_base + 1, z80base);
        z80w8(P_dpcm_page + 1, z80base >> 8);
    }

    // All voices start silent and detached from the PSG.
    for (i = 0; i < 2; i++)
    {
        u16 v;
        for (v = 0; v < PSGDAC_NVARIANT; v++)
        {
            z80w16(pchDelta[i][v], 0);
            z80w8(pchDuty[i][v], 0);
            z80w8(pchSpan[i][v], psgdacSpanByte(15));
            z80w16(pchOut[i][v], Z80_DUMMY);
        }
    }
    z80w16(P_v3_c_delta + 1, 0);
    z80w8(P_v3_c_page + 1, Z80_WAVE >> 8);
    z80w16(P_v3_c_out + 1, Z80_DUMMY);
    z80w16(P_v2d_d_out + 1, Z80_DUMMY);

    for (i = 0; i < PSGDAC_NVARIANT; i++)
        z80w16(pchNext[i], psgdacEntry[PSGDAC_V3]);
    z80w16(P_entry + 1, psgdacEntry[PSGDAC_V3]);

    Z80_endReset();
    Z80_releaseBus();

    for (i = 0; i < 3; i++)
    {
        psgdacVoice[i].delta = 0; psgdacVoice[i].duty = 0;
        psgdacVoice[i].atten = 15; psgdacVoice[i].on = 0;
        psgdacSent[i] = psgdacVoice[i];
    }
    psgdacVariant = PSGDAC_V3;
    psgdacSentVar = PSGDAC_V3;
    psgdacReady = 1;
}

// Fill the 256-byte wave table.  One page = eight copies of a 32-step waveform,
// so the inner loop can index it with the raw phase high byte and no shifting.
// Entries are finished PSG bytes for channel 2 (0xD0 | attenuation).
static void PSGDAC_loadWave(const u8 *atten32)
{
    u16 i;
    Z80_requestBus(TRUE);
    for (i = 0; i < 256; i++)
        *Z80_RAM(Z80_WAVE + i) = 0xD0 | (atten32[i >> 3] & 0x0F);
    Z80_releaseBus();
}

// ---- per-frame parameter setting (call, then flush) ------------------------

static inline void PSGDAC_setVariant(u8 v)        { psgdacVariant = v; }
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

// Push whatever changed into the running loop.  The Z80 bus must be held; keep
// the window short, because the Z80 is stopped and so is the audio.
static void PSGDAC_flush(void)
{
    u8 v = psgdacVariant;
    u8 all = (v != psgdacSentVar);
    u8 i;

    if (!psgdacReady) return;

    for (i = 0; i < 2; i++)
    {
        DacVoice *now = &psgdacVoice[i];
        DacVoice *was = &psgdacSent[i];
        if (all || now->delta != was->delta) z80w16(pchDelta[i][v], now->delta);
        if (all || now->duty  != was->duty ) z80w8(pchDuty[i][v], now->duty);
        if (all || now->atten != was->atten) z80w8(pchSpan[i][v], psgdacSpanByte(now->atten));
        if (all || now->on    != was->on   )
            z80w16(pchOut[i][v], now->on ? Z80_PSG_PORT : Z80_DUMMY);
        *was = *now;
    }

    if (v == PSGDAC_V3)
    {
        DacVoice *now = &psgdacVoice[2];
        DacVoice *was = &psgdacSent[2];
        if (all || now->delta != was->delta) z80w16(P_v3_c_delta + 1, now->delta);
        if (all || now->on    != was->on   )
            z80w16(P_v3_c_out + 1, now->on ? Z80_PSG_PORT : Z80_DUMMY);
        *was = *now;
    }

    if (v == PSGDAC_V2D && (all || psgdacDpcmOn != psgdacDpcmSent))
    {
        z80w16(P_v2d_d_out + 1, psgdacDpcmOn ? Z80_YM_DATA : Z80_DUMMY);
        psgdacDpcmSent = psgdacDpcmOn;
    }

    if (all)
    {
        // Patch every variant's back-jump, so it does not matter which one the
        // Z80 happens to be sitting in right now.
        for (i = 0; i < PSGDAC_NVARIANT; i++)
            z80w16(pchNext[i], psgdacEntry[v]);
        psgdacSentVar = v;
    }
}

#endif // _PSGDAC_H_
