-- =============================================================================
-- GeNESis-APU2PSG -- NES APU recorder for FCEUX
--
-- Writes one CSV line per NES frame.  Fields 1..18 are byte-for-byte the v1
-- format, so old players keep working; v2 appends the things v1 could not say:
-- post-envelope volumes, the full noise period index, the noise mode, and
-- optionally a resampled DPCM stream.
--
-- The v2 fields matter because v1 logged raw register nibbles.  A NES channel's
-- audible volume is not its $4000 low nibble -- it is that nibble only when the
-- constant-volume flag is set, and the envelope's decay level otherwise.  Reading
-- the register got the loud notes right and every envelope tail wrong.
-- =============================================================================

-- Relative paths resolve against the EMULATOR'S working directory (wherever
-- Gens.exe or FCEUX was launched from), not against this script -- which is why
-- "file not found" was the first thing so many runs ever printed. Resolve next
-- to this script instead, so the repo folder works as cloned, checked-in
-- capture included. The old behaviour (file beside the emulator) still works
-- as a fallback.
local function scriptDir()
    -- Must be a direct call: through pcall, level 1 is pcall's own C frame.
    if not debug or not debug.getinfo then return nil end
    local info = debug.getinfo(1, "S")
    if not info or not info.source then return nil end
    local src = info.source
    if string.sub(src, 1, 1) == "@" then src = string.sub(src, 2) end
    return string.match(src, "^(.*[/\\])")
end

local BASENAME    = "nes_apu_data.txt"   -- must match the Gens-side script
local FILENAME    = (scriptDir() or "") .. BASENAME
local RECORD_DPCM = false                -- see the DPCM note at the bottom
-- Must match the Genesis driver's V2D loop rate: PCM only plays in the variant
-- that has a PCM voice, and that variant runs at 19349 Hz.  Get this wrong and
-- everything sampled plays back at the wrong speed.
local DPCM_RATE   = 19349
local DPCM_PER_FRAME = math.floor(DPCM_RATE / 60)

local outputFile, openErr = io.open(FILENAME, "w")
if not outputFile then
    -- Fall back to the emulator's working directory if the script's own folder
    -- is not writable (a repo on a read-only share, Program Files, and so on).
    print("Could not write " .. FILENAME .. ": " .. tostring(openErr))
    outputFile, openErr = io.open(BASENAME, "w")
    if outputFile then
        FILENAME = BASENAME
        print("Falling back to the emulator's working directory instead.")
    else
        print("Error: could not write " .. BASENAME .. " either: " .. tostring(openErr))
        print("Load tools/diagnose.lua for a fuller report.")
        return
    end
end

outputFile:write("#GAPU2 v2\n")

-- FCEUX has reported sound.get() volumes as 0..1 floats in some builds and as
-- 0..15 in others.  Accept both rather than guess.
local function toNesVol(v)
    if v == nil then return nil end
    if v <= 1.0 then return math.floor(v * 15 + 0.5) end
    return math.floor(v + 0.5)
end

local function chan(snd, name)
    if snd and snd.rp2a03 then return snd.rp2a03[name] end
    return nil
end

-- Fallback when sound.get() is unavailable.  The envelope's decay level is not
-- readable from the registers at all, so this is only right while the
-- constant-volume flag is set -- which is exactly the v1 behaviour.
local function regVolume(reg)
    return memory.readbyte(reg) % 16
end

-- ---------------------------------------------------------------- DPCM ------
-- $4011 is the DPCM output level.  It changes far faster than once a frame, so
-- polling it per frame captures nothing usable; we hook every write instead and
-- spread the frame's writes evenly over the frame.  DPCM's own writes arrive at
-- a steady rate, so even placement is close to the truth.
local dpcmWrites = {}
local dpcmLast = 0x40

if RECORD_DPCM and memory.registerwrite then
    memory.registerwrite(0x4011, 1, function(addr, size, value)
        dpcmWrites[#dpcmWrites + 1] = value % 128
    end)
end

-- DMC trigger capture. Registers sampled once a frame can lie: a game may
-- rewrite $4012 right after triggering, so the frame-boundary read would name
-- the NEXT sample, not the one playing. Hook $4015 and latch address, length
-- and rate at the trigger instant instead. tools/gen_dpcm.py reads these
-- fields to know which samples to pull out of the .nes file.
local trig = {0, 0, 0, 0}
if memory.registerwrite then
    memory.registerwrite(0x4015, 1, function(addr, size, value)
        if value % 32 >= 16 then
            trig = {1, memory.readbyte(0x4012), memory.readbyte(0x4013),
                       memory.readbyte(0x4010) % 16}
        end
    end)
end

local function dpcmField()
    local n = #dpcmWrites
    local out = {}
    for i = 1, DPCM_PER_FRAME do
        local v
        if n == 0 then
            v = dpcmLast
        else
            local idx = math.floor((i - 1) * n / DPCM_PER_FRAME) + 1
            v = dpcmWrites[idx] or dpcmLast
        end
        -- $4011 is 7-bit; the YM2612 DAC is 8-bit unsigned centred on 0x80.
        out[i] = string.format("%02X", (v * 2) % 256)
    end
    if n > 0 then dpcmLast = dpcmWrites[n] end
    dpcmWrites = {}
    return table.concat(out)
end

-- ---------------------------------------------------------------- main ------
local frame = 0

emu.registerbefore(function()
    local status = memory.readbyte(0x4015)
    local snd = sound.get and sound.get() or nil

    local sq1 = chan(snd, "square1")
    local sq2 = chan(snd, "square2")
    local tri = chan(snd, "triangle")
    local noi = chan(snd, "noise")

    -- Pulse 1
    local p1_period = memory.readword(0x4002) % 2048
    local p1_reg    = memory.readbyte(0x4000)
    local p1_duty   = math.floor(p1_reg / 64) % 4
    local p1_on     = status % 2
    local p1_vol    = toNesVol(sq1 and sq1.volume) or regVolume(0x4000)

    -- Pulse 2
    local p2_period = memory.readword(0x4006) % 2048
    local p2_reg    = memory.readbyte(0x4004)
    local p2_duty   = math.floor(p2_reg / 64) % 4
    local p2_on     = math.floor(status / 2) % 2
    local p2_vol    = toNesVol(sq2 and sq2.volume) or regVolume(0x4004)

    -- Triangle.  It has no volume, only a length counter and a linear counter,
    -- so "is it audible" is the only question worth asking -- and sound.get()
    -- answers it far more reliably than $4015 does.
    local tri_period = memory.readword(0x400A) % 2048
    local tri_on
    if tri and tri.volume then
        tri_on = (tri.volume > 0) and 1 or 0
    else
        tri_on = math.floor(status / 4) % 2
    end

    -- Noise.  v1 only ever transmitted a coarse rate; the period index is what
    -- actually selects one of the NES's 16 noise pitches.
    local noise_reg    = memory.readbyte(0x400E)
    local noise_mode   = math.floor(noise_reg / 128) % 2   -- 1 = short/periodic
    local noise_period = noise_reg % 16
    local noise_on     = math.floor(status / 8) % 2
    local noise_vol    = toNesVol(noi and noi.volume) or regVolume(0x400C)

    -- DPCM
    local dpcm_sample = memory.readbyte(0x4011)
    local dpcm_freq   = memory.readbyte(0x4010)
    local dpcm_addr   = memory.readbyte(0x4012)
    local dpcm_len    = memory.readbyte(0x4013)
    local dpcm_on     = math.floor(status / 16) % 2

    -- Fields 1..18: the v1 layout, unchanged.
    local line = string.format(
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
        p1_period, p1_reg % 16, p1_duty, p1_on,
        p2_period, p2_reg % 16, p2_duty, p2_on,
        tri_period, tri_on,
        noise_period, noise_vol, noise_on, noise_mode,
        dpcm_sample, dpcm_freq, dpcm_addr, dpcm_len)

    -- Fields 19..22: what v1 could not express.  Everything else v2 needs is
    -- already in the v1 fields, it was just never read correctly.
    line = line .. string.format(",%d,%d,%d,%d",
        p1_vol, p2_vol, dpcm_on, frame % 256)

    -- Fields 23..26: DMC trigger events, latched at the $4015 write.
    line = line .. string.format(",%d,%d,%d,%d", trig[1], trig[2], trig[3], trig[4])
    trig = {0, 0, 0, 0}

    if RECORD_DPCM then
        line = line .. "|" .. dpcmField()
    end

    outputFile:write(line .. "\n")
    outputFile:flush()
    frame = frame + 1

    gui.text(5, 210, "GAPU2 recording  frame " .. frame)
    gui.text(5, 220, string.format("P1 v%d d%d  P2 v%d d%d  TRI %d",
             p1_vol, p1_duty, p2_vol, p2_duty, tri_on))
    gui.text(5, 230, string.format("NOISE p%d %s v%d", noise_period,
             noise_mode == 1 and "short" or "long", noise_vol))
end)

emu.registerexit(function()
    outputFile:close()
    print("NES APU recording complete")
end)

print("GeNESis-APU2PSG recorder v2 started -> " .. FILENAME)
if RECORD_DPCM then
    print("DPCM capture ON: ~" .. (DPCM_PER_FRAME * 2) ..
          " extra chars per frame. Set RECORD_DPCM=false to shrink the log.")
end
