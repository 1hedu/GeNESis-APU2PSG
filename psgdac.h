// =============================================================================
// psgdac.h -- 68000 side of the PSG volume-DAC synthesis driver.
//
// The Z80 half (z80_psgdac.s80) free-runs a fixed-length loop that rewrites PSG
// attenuation registers thousands of times a second, and it owns every sound
// chip write on the machine -- PSG and YM2612 alike.  The 68000 never touches a
// chip register after boot.  It posts (address, data) triples into a queue in
// Z80 RAM and the loop replays one per sample.
//
// That division exists because the 68000 cannot reach a sound chip without
// holding the Z80 bus, and holding the Z80 bus stops the loop, which stops the
// audio.  With one writer there is also no way for a PSG tone period's latch
// and data bytes to be split by someone else's write, and no way for the
// YM2612's part-I address latch -- parked on 2Ah so PCM costs one store a
// sample -- to be clobbered out from under the DAC.
//
// What the 68000 still does inside a bus window is patch the loop's immediate
// operands and append to the queue.  Plain byte stores, no waits.
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
#define PSGDAC_NVARIANT 3

#define PSGDAC_PULSE_A  0
#define PSGDAC_PULSE_B  1

// Z80-side addresses the driver was assembled with.
#define Z80_DUMMY       0x0204          // 13-cycle store that goes nowhere
#define Z80_QPTR        0x0206          // queue read cursor (driver-owned)
#define Z80_QEND        0x0208          // queue write cursor (68000-owned)
#define Z80_QBASE       0x0300          // 256-byte queue page: 85 triples
#define Z80_WAVE        0x0400          // 256-byte wave table, page-aligned
#define Z80_PSG_PORT    0x7F11
#define Z80_YM_ADDR1    0x4000          // YM2612 part I  (channels 1-3, and 0x28)
#define Z80_YM_DATA1    0x4001
#define Z80_YM_ADDR2    0x4002          // YM2612 part II (channels 4-6)
#define Z80_YM_DATA2    0x4003
#define Z80_MEM(a)      ((vu8*)(0xA00000 + (a)))   // SGDK owns the name Z80_RAM

// Loop lengths, straight out of the assembler: the variant body plus the one
// jump through the service slot that every sample pays.
#define CYC_V3  (CYCLES_v3  + CYCLES_svcslot)
#define CYC_V2  (CYCLES_v2  + CYCLES_svcslot)
#define CYC_V2D (CYCLES_v2d + CYCLES_svcslot)

static const u16 psgdacCycles[PSGDAC_NVARIANT] = {CYC_V3, CYC_V2, CYC_V2D};
static const u16 psgdacRate[PSGDAC_NVARIANT]   = {14731, 23244, 19349};

// delta = K / (nesPeriod + 1).  K = nesClock * 65536 / (divider * sampleRate).
static const u32 psgdacKPulse[PSGDAC_NVARIANT]    = {497664, 315392, 378880};
static const u32 psgdacKTriangle[PSGDAC_NVARIANT] = {248832, 157696, 189440};

// Highest fundamental worth handing to a DAC voice: below this a 12.5% pulse
// still gets its 8 slots per period.  Above it the hardware tone generator wins.
static const u16 psgdacCeiling[PSGDAC_NVARIANT] = {14731 / 8, 23244 / 8, 19349 / 8};

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
static const u16 psgdacEntry[PSGDAC_NVARIANT] = {L_v3, L_v2, L_v2d};

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
static u8 psgdacQEnd     = 0;           // our shadow of the Z80's queue tail

// ---- the command queue -----------------------------------------------------
// Built up over the frame in 68000 RAM, then copied into the Z80's queue page
// in one burst inside the bus window.
#define PSGDAC_CMD_MAX  48

static u16 cmdAddr[PSGDAC_CMD_MAX];
static u8  cmdData[PSGDAC_CMD_MAX];
static u8  cmdCount = 0;
static u8  cmdDropped = 0;

static inline void PSGDAC_cmd(u16 addr, u8 data)
{
    if (cmdCount >= PSGDAC_CMD_MAX) { cmdDropped = 1; return; }
    cmdAddr[cmdCount] = addr;
    cmdData[cmdCount] = data;
    cmdCount++;
}

// A YM2612 register is an address write then a data write. They land one sample
// apart, which is far longer than the chip's busy flag lasts, so there is no
// wait to do -- the queue's own pacing is the wait.
static inline void PSGDAC_ym(u8 part, u8 reg, u8 val)
{
    if (part) { PSGDAC_cmd(Z80_YM_ADDR2, reg); PSGDAC_cmd(Z80_YM_DATA2, val); }
    else      { PSGDAC_cmd(Z80_YM_ADDR1, reg); PSGDAC_cmd(Z80_YM_DATA1, val); }
}

static inline void PSGDAC_psg(u8 byte) { PSGDAC_cmd(Z80_PSG_PORT, byte); }

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
    z80w16(P_svc + 1, psgdacEntry[PSGDAC_V3]);
    z80w16(P_svcout + 1, psgdacEntry[PSGDAC_V3]);

    // Own the queue cursors from here: the driver deliberately does not set
    // them, because its preamble runs after the bus is released and would race
    // this frame's first flush.
    z80w16(Z80_QPTR, Z80_QBASE);
    z80w8(Z80_QEND, Z80_QBASE & 0xFF);

    // Fill the queue with harmless commands rather than leaving it as whatever
    // the Z80's RAM powered up holding. If anything ever does replay a stale
    // entry, it writes a zero to the driver's own scratch byte instead of to an
    // arbitrary address -- which, through the bank window, could be 68000 RAM.
    for (i = 0; i < 256; i += 3)
    {
        *Z80_MEM(Z80_QBASE + i)     = Z80_DUMMY & 0xFF;
        if (i + 1 < 256) *Z80_MEM(Z80_QBASE + i + 1) = Z80_DUMMY >> 8;
        if (i + 2 < 256) *Z80_MEM(Z80_QBASE + i + 2) = 0;
    }

    for (i = 0; i < 3; i++)
    {
        psgdacVoice[i].delta = 0; psgdacVoice[i].duty = 0;
        psgdacVoice[i].atten = 15; psgdacVoice[i].on = 0;
        psgdacSent[i] = psgdacVoice[i];
    }
    psgdacVariant = PSGDAC_V3;
    psgdacSentVar = PSGDAC_V3;
    psgdacQEnd = Z80_QBASE & 0xFF;
    cmdCount = 0;
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
    u8 all = (v != psgdacSentVar);
    u8 i;

    if (!psgdacReady) { cmdCount = 0; return; }

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
        z80w16(P_v2d_d_out + 1, psgdacDpcmOn ? Z80_YM_DATA1 : Z80_DUMMY);
        psgdacDpcmSent = psgdacDpcmOn;
    }

    // The service routine restores the idle jump from here when it drains, so
    // this must always name the running variant.
    if (all) { z80w16(P_svcout + 1, psgdacEntry[v]); psgdacSentVar = v; }

    // Append this frame's chip writes as (address, data) triples.
    for (i = 0; i < cmdCount; i++)
    {
        z80w8(Z80_QBASE | psgdacQEnd, cmdAddr[i] & 0xFF);      psgdacQEnd++;
        z80w8(Z80_QBASE | psgdacQEnd, cmdAddr[i] >> 8);        psgdacQEnd++;
        z80w8(Z80_QBASE | psgdacQEnd, cmdData[i]);             psgdacQEnd++;
    }

    if (cmdCount)
    {
        z80w8(Z80_QEND, psgdacQEnd);
        z80w16(P_svc + 1, L_svcrun);        // wake the service slot
    }
    else if (all)
    {
        // No work queued. Only re-point the idle jump if the queue really is
        // empty -- otherwise the service routine is mid-drain and owns it.
        if (z80r8(Z80_QPTR) == psgdacQEnd) z80w16(P_svc + 1, psgdacEntry[v]);
    }
    cmdCount = 0;
}

#endif // _PSGDAC_H_
