-- =============================================================================
-- GeNESis-APU2PSG -- NES APU playback driver for Gens (Lua 5.1)
--
-- Reads the recorder's log and hands one frame of NES APU state to the Genesis
-- ROM through shared 68000 RAM.  Two blocks are written every frame:
--
--   base + OFF_LEGACY + 0  v1 block, PSG-native (periods and attenuations, pre-converted).
--             Written so an older ROM build still plays.
--   base + OFF_EXT + 0  v2 block, NES-native (periods, 0..15 volumes, duties, noise index).
--             The current ROM prefers this, because deciding between a hardware
--             tone generator and a volume-DAC voice needs the NES numbers, not
--             numbers already flattened into PSG terms.
--
-- The v1 block was lossy in ways that mattered.  Its noise field carried a
-- three-way rate where the NES has sixteen; its volumes were 15-v, which treats
-- a linear scale as a logarithmic one; and it had no field for pulse 2's duty
-- at all.  v2 carries all of it.
-- =============================================================================

local BASENAME  = "nes_apu_data.txt"   -- must match the FCEUX-side script
local PLAY_DPCM = true                 -- honoured only if the log has PCM in it

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

local FILENAME, file
local why = {}
for _, path in ipairs({ (scriptDir() or "") .. BASENAME, BASENAME }) do
    local f, err = io.open(path, "r")
    if f then file, FILENAME = f, path; break end
    why[#why + 1] = "  " .. path .. "\n      " .. tostring(err)
end
if not file then
    -- Report what the OS said, not what we assumed. io.open's second return
    -- distinguishes a missing file from a locked one, a bad name from a
    -- protected folder -- all of which used to print "not found".
    print("Error: could not open " .. BASENAME)
    for _, line in ipairs(why) do print(line) end
    print("  Load tools/diagnose.lua for a fuller report.")
    return
end

-- ---------------------------------------------------------------- tables ----
-- NES linear volume 0..15 -> PSG logarithmic attenuation, 2 dB a step.
local VOL_TO_ATTEN = {[0]=15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0}

-- Nearest of the PSG's three fixed noise rates for each NES noise period.
-- The shift register advances once per tone-3 output cycle, so the fixed
-- rates land on periods 9, 11 and 13. Only the legacy v1 block uses this;
-- the ROM does the full mapping itself from the v2 block.
local NOISE_FIXED = {[0]=0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2}


-- ---------------------------------------------------------------- discovery --
-- The ROM's shared block is a linker-placed C object, so its address is not
-- knowable in advance -- and hardcoding one is what used to crash the ROM, by
-- landing on SGDK's saved program counter. Find it by scanning RAM for the
-- magic and checking that the block's 'self' field points back at where we
-- found it. Done once; a false positive cannot survive the self check.
local MAGIC = { 0x47, 0x41, 0x50, 0x55 }   -- 'GAPU'
local base                                  -- discovered block address

local function rd32(a)
    return memory.readbyte(a) * 16777216 + memory.readbyte(a + 1) * 65536
         + memory.readbyte(a + 2) * 256   + memory.readbyte(a + 3)
end

local function findBlock()
    for a = base + OFF_LEGACY + 0, 0xFFFFFC, 4 do
        if memory.readbyte(a) == MAGIC[1] and memory.readbyte(a + 1) == MAGIC[2]
       and memory.readbyte(a + 2) == MAGIC[3] and memory.readbyte(a + 3) == MAGIC[4] then
            local self_ = rd32(a + 4) % 0x1000000 + base + OFF_LEGACY + 0
            -- the ROM stores the raw pointer; compare only the RAM-relevant bits
            if (rd32(a + 4) % 0x10000) == (a % 0x10000) then return a end
        end
    end
    return nil
end

-- Offsets inside the block, matching the ApuBlock struct in the ROM.
local OFF_RING, OFF_LEGACY, OFF_EXT = 8, 12, 28

-- ---------------------------------------------------------------- helpers ---
local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function atten(vol)
    return VOL_TO_ATTEN[clamp(math.floor(vol or 0), 0, 15)]
end

-- The Genesis PSG clock is exactly twice the NES CPU clock, and it divides by 32
-- where the NES divides by 16.  So a NES pulse period t becomes a PSG period of
-- t+1, and the triangle -- which divides by 32 -- becomes 2(t+1).  No rounding,
-- no lookup table, no drift.
local function pulsePeriodToPsg(t) return clamp(t + 1, 1, 1023) end
local function triPeriodToPsg(t)   return clamp((t + 1) * 2, 1, 1023) end

local function w8(addr, v)  memory.writebyte(addr, clamp(math.floor(v), 0, 255)) end
local function w16(addr, v)
    v = clamp(math.floor(v), 0, 65535)
    w8(addr, v % 256)
    w8(addr + 1, math.floor(v / 256))
end

-- ---------------------------------------------------------------- parsing ---
local function parseLine(line)
    local csv, pcm = line, nil
    local bar = string.find(line, "|", 1, true)
    if bar then
        csv = string.sub(line, 1, bar - 1)
        pcm = string.sub(line, bar + 1)
    end

    local v = {}
    for num in string.gmatch(csv, "[^,]+") do
        v[#v + 1] = tonumber(num) or 0
    end
    if #v < 14 then return nil end

    local f = {}
    f.p1_period = v[1] % 2048
    f.p1_duty   = v[3] % 4
    f.p1_on     = v[4]
    f.p2_period = v[5] % 2048
    f.p2_duty   = v[7] % 4
    f.p2_on     = v[8]
    f.tri_period = v[9] % 2048
    f.tri_on     = v[10]
    f.noise_period = v[11] % 16
    f.noise_vol    = v[12] % 16
    f.noise_on     = v[13]
    f.noise_mode   = v[14] % 2
    f.dpcm_on = 0
    f.frame   = 0

    if #v >= 22 then
        -- v2: post-envelope volumes, so decay tails come out at the right level.
        f.p1_vol  = v[19] % 16
        f.p2_vol  = v[20] % 16
        f.dpcm_on = v[21]
        f.frame   = v[22] % 256
        f.v2 = true
    else
        -- v1: the raw register nibble is the best we have.
        f.p1_vol = v[2] % 16
        f.p2_vol = v[6] % 16
        f.v2 = false
    end

    f.pcm = pcm
    return f
end

-- ---------------------------------------------------------------- output ----
local function writeLegacyBlock(f)
    if f.p1_on == 1 and f.p1_vol > 0 then
        w16(base + OFF_LEGACY + 0, pulsePeriodToPsg(f.p1_period))
        w8(base + OFF_LEGACY + 2, atten(f.p1_vol))
    else
        w8(base + OFF_LEGACY + 2, 15)
    end
    w8(base + OFF_LEGACY + 8, f.p1_duty)

    if f.p2_on == 1 and f.p2_vol > 0 then
        w16(base + OFF_LEGACY + 3, pulsePeriodToPsg(f.p2_period))
        w8(base + OFF_LEGACY + 5, atten(f.p2_vol))
    else
        w8(base + OFF_LEGACY + 5, 15)
    end

    if f.tri_on == 1 then
        w16(base + OFF_LEGACY + 10, triPeriodToPsg(f.tri_period))
        w8(base + OFF_LEGACY + 12, 4)
        w8(base + OFF_LEGACY + 13, 1)
    else
        w16(base + OFF_LEGACY + 10, 0)
        w8(base + OFF_LEGACY + 12, 15)
        w8(base + OFF_LEGACY + 13, 0)
    end

    if f.noise_on == 1 and f.noise_vol > 0 then
        local white = (f.noise_mode == 0) and 4 or 0
        w8(base + OFF_LEGACY + 6, 0xE0 + white + NOISE_FIXED[f.noise_period])
        w8(base + OFF_LEGACY + 7, atten(f.noise_vol))
    else
        w8(base + OFF_LEGACY + 7, 15)
    end
    w8(base + OFF_LEGACY + 9, f.frame)
end

local function writeExtBlock(f)
    w8(base + OFF_EXT + 1, f.p1_period % 256)
    w8(base + OFF_EXT + 2, math.floor(f.p1_period / 256))
    w8(base + OFF_EXT + 3, f.p1_vol)
    w8(base + OFF_EXT + 4, f.p1_duty)
    w8(base + OFF_EXT + 5, f.p1_on)

    w8(base + OFF_EXT + 6, f.p2_period % 256)
    w8(base + OFF_EXT + 7, math.floor(f.p2_period / 256))
    w8(base + OFF_EXT + 8, f.p2_vol)
    w8(base + OFF_EXT + 9, f.p2_duty)
    w8(base + OFF_EXT + 10, f.p2_on)

    w8(base + OFF_EXT + 11, f.tri_period % 256)
    w8(base + OFF_EXT + 12, math.floor(f.tri_period / 256))
    w8(base + OFF_EXT + 13, f.tri_on)

    w8(base + OFF_EXT + 14, f.noise_period)
    w8(base + OFF_EXT + 15, f.noise_mode)
    w8(base + OFF_EXT + 16, f.noise_vol)
    w8(base + OFF_EXT + 17, f.noise_on)

    w8(base + OFF_EXT + 18, f.dpcm_on)
    w8(base + OFF_EXT + 19, f.frame)

    -- Magic last: the ROM reads it first, so writing it last means it never sees
    -- a half-filled block.
    w8(base + OFF_EXT + 0, 0x47)
end

-- ---------------------------------------------------------------- PCM -------
-- The ROM reserves a 4 KB ring in 68000 RAM and publishes its address; the Z80
-- streams samples straight out of it through its bank window.  Writing here goes
-- to 68000 RAM directly, so PCM never has to cross the Z80 bus and never stalls
-- the synthesis loop.
local ringBase, ringCursor = 0, 0

local function readRingBase()
    return rd32(base + OFF_RING) % 0x1000000
end

local function writePcm(hex)
    if not PLAY_DPCM or not hex or hex == "" then return end
    if ringBase < 0xFF1000 or ringBase >= 0xFF8000 then
        ringBase = readRingBase()
        if ringBase < 0xFF1000 or ringBase >= 0xFF8000 then return end
    end
    for i = 1, #hex - 1, 2 do
        local b = tonumber(string.sub(hex, i, i + 1), 16)
        if b then
            memory.writebyte(ringBase + ringCursor, b)
            ringCursor = (ringCursor + 1) % 4096
        end
    end
end

-- ---------------------------------------------------------------- main ------
local frames, finished = 0, false

local function main()
    if finished then return end

    -- Find the ROM's shared block once. Until it appears the ROM has not booted
    -- far enough to publish it, so there is nothing to write to yet.
    if not base then
        base = findBlock()
        if not base then
            if frames == 0 then
                print("Waiting for the ROM: no GAPU block in RAM yet.")
                print("  If this never clears, the ROM is not the current build --")
                print("  its overlay should show MODE and LOOP on the top two lines.")
                frames = 1
            end
            return
        end
        print(string.format("Found ROM block at 0x%06X", base))
        frames = 0
    end

    local line = file:read("*l")
    while line and (line == "" or string.sub(line, 1, 1) == "#") do
        line = file:read("*l")
    end
    if not line then
        print("End of NES data stream.")
        file:close()
        finished = true
        return
    end

    local f = parseLine(line)
    if not f then return end

    writeLegacyBlock(f)
    writeExtBlock(f)
    writePcm(f.pcm)

    frames = frames + 1
    if frames % 60 == 0 then
        print(string.format("frame %d  proto %s  P1 %d/v%d/d%d  TRI %d  NZ p%d %s",
              frames, f.v2 and "v2" or "v1", f.p1_period, f.p1_vol, f.p1_duty,
              f.tri_period, f.noise_period, f.noise_mode == 1 and "short" or "long"))
    end
end

gens.registerafter(main)
print("GeNESis-APU2PSG player v2 reading " .. FILENAME)
