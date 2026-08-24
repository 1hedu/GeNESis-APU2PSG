-- =============================================================================
-- GeNESis-APU2PSG -- live sync playback for Gens (Lua 5.1)
--
-- Same job as the Player, except the log is being written by FCEUX right now.
-- We tail the file instead of reading it start to finish, and keep a small
-- buffer so a stutter on either emulator does not become a dropout.
--
-- Run this and the FCEUX recorder at the same time, pointed at the same file,
-- and turn the NES emulator's own audio down in your OS mixer.
-- =============================================================================

local BASENAME  = "nes_apu_data.txt"   -- must match the FCEUX-side script
local PLAY_DPCM = true

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

-- Candidates in preference order; openFile() tries each until one appears.
local CANDIDATES = { (scriptDir() or "") .. BASENAME, BASENAME }
local FILENAME = CANDIDATES[1]

local BUFFER_TARGET = 4                -- frames of slack we try to hold
local BUFFER_MAX    = 12               -- past this we are lagging; catch up
local READ_INTERVAL = 0.008            -- seconds between file polls

-- ---------------------------------------------------------------- tables ----
local VOL_TO_ATTEN = {[0]=15, 12, 9, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1, 1, 0, 0}
local NOISE_FIXED  = {[0]=0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2}


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

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

local function atten(vol) return VOL_TO_ATTEN[clamp(math.floor(vol or 0), 0, 15)] end
local function w8(addr, v) memory.writebyte(addr, clamp(math.floor(v), 0, 255)) end
local function w16(addr, v)
    v = clamp(math.floor(v), 0, 65535)
    w8(addr, v % 256)
    w8(addr + 1, math.floor(v / 256))
end

-- PSG clock is exactly 2x the NES CPU clock and divides by 32 to the NES's 16.
local function pulsePeriodToPsg(t) return clamp(t + 1, 1, 1023) end
local function triPeriodToPsg(t)   return clamp((t + 1) * 2, 1, 1023) end

-- ---------------------------------------------------------------- file ------
local file, lastPos, buffer = nil, 0, {}

local reported = false

local function openFile()
    local why = {}
    for _, path in ipairs(CANDIDATES) do
        local f, err = io.open(path, "r")
        file = f
        if not f then why[#why + 1] = "  " .. path .. "\n      " .. tostring(err) end
        if file then
            FILENAME = path
            file:seek("end")      -- live sync: only care about what happens next
            lastPos = file:seek()
            print("NES APU data file found, tailing " .. path)
            reported = false
            return
        end
    end
    -- Live sync legitimately starts before the recorder has made the file, so
    -- say why once rather than every frame.
    if not reported then
        reported = true
        print("Waiting for " .. BASENAME .. ":")
        for _, line in ipairs(why) do print(line) end
    end
end

openFile()

local function readMore()
    if not file then openFile() end
    if not file then return end
    file:seek("set", lastPos)
    while #buffer < BUFFER_MAX do
        local line = file:read("*l")
        if not line then break end
        lastPos = file:seek()
        if line ~= "" and string.sub(line, 1, 1) ~= "#" then
            buffer[#buffer + 1] = line
        end
    end
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

    local f = {
        p1_period = v[1] % 2048, p1_duty = v[3] % 4, p1_on = v[4],
        p2_period = v[5] % 2048, p2_duty = v[7] % 4, p2_on = v[8],
        tri_period = v[9] % 2048, tri_on = v[10],
        noise_period = v[11] % 16, noise_vol = v[12] % 16,
        noise_on = v[13], noise_mode = v[14] % 2,
        dpcm_on = 0, frame = 0, pcm = pcm,
    }

    if #v >= 22 then
        f.p1_vol, f.p2_vol = v[19] % 16, v[20] % 16
        f.dpcm_on, f.frame = v[21], v[22] % 256
        f.v2 = true
    else
        f.p1_vol, f.p2_vol = v[2] % 16, v[6] % 16
        f.v2 = false
    end
    return f
end

-- ---------------------------------------------------------------- output ----
local function writeBlocks(f)
    -- v1 block, so an older ROM build still plays.
    if f.p1_on == 1 and f.p1_vol > 0 then
        w16(base + OFF_LEGACY + 0, pulsePeriodToPsg(f.p1_period)); w8(base + OFF_LEGACY + 2, atten(f.p1_vol))
    else w8(base + OFF_LEGACY + 2, 15) end
    w8(base + OFF_LEGACY + 8, f.p1_duty)

    if f.p2_on == 1 and f.p2_vol > 0 then
        w16(base + OFF_LEGACY + 3, pulsePeriodToPsg(f.p2_period)); w8(base + OFF_LEGACY + 5, atten(f.p2_vol))
    else w8(base + OFF_LEGACY + 5, 15) end

    if f.tri_on == 1 then
        w16(base + OFF_LEGACY + 10, triPeriodToPsg(f.tri_period)); w8(base + OFF_LEGACY + 12, 4); w8(base + OFF_LEGACY + 13, 1)
    else w16(base + OFF_LEGACY + 10, 0); w8(base + OFF_LEGACY + 12, 15); w8(base + OFF_LEGACY + 13, 0) end

    if f.noise_on == 1 and f.noise_vol > 0 then
        w8(base + OFF_LEGACY + 6, 0xE0 + ((f.noise_mode == 0) and 4 or 0) + NOISE_FIXED[f.noise_period])
        w8(base + OFF_LEGACY + 7, atten(f.noise_vol))
    else w8(base + OFF_LEGACY + 7, 15) end
    w8(base + OFF_LEGACY + 9, f.frame)

    -- v2 block, NES-native, which is what the volume-DAC allocator needs.
    w8(base + OFF_EXT + 1, f.p1_period % 256); w8(base + OFF_EXT + 2, math.floor(f.p1_period / 256))
    w8(base + OFF_EXT + 3, f.p1_vol); w8(base + OFF_EXT + 4, f.p1_duty); w8(base + OFF_EXT + 5, f.p1_on)
    w8(base + OFF_EXT + 6, f.p2_period % 256); w8(base + OFF_EXT + 7, math.floor(f.p2_period / 256))
    w8(base + OFF_EXT + 8, f.p2_vol); w8(base + OFF_EXT + 9, f.p2_duty); w8(base + OFF_EXT + 10, f.p2_on)
    w8(base + OFF_EXT + 11, f.tri_period % 256); w8(base + OFF_EXT + 12, math.floor(f.tri_period / 256))
    w8(base + OFF_EXT + 13, f.tri_on)
    w8(base + OFF_EXT + 14, f.noise_period); w8(base + OFF_EXT + 15, f.noise_mode)
    w8(base + OFF_EXT + 16, f.noise_vol); w8(base + OFF_EXT + 17, f.noise_on)
    w8(base + OFF_EXT + 18, f.dpcm_on); w8(base + OFF_EXT + 19, f.frame)
    w8(base + OFF_EXT + 0, 0x47)                  -- magic last, so the ROM never sees half a block
end

-- ---------------------------------------------------------------- PCM -------
local ringBase, ringCursor = 0, 0

local function writePcm(hex)
    if not PLAY_DPCM or not hex or hex == "" then return end
    if ringBase < 0xFF1000 or ringBase >= 0xFF8000 then
        ringBase = rd32(base + OFF_RING) % 0x1000000
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
local syncTimer = os.clock()
local frames = 0

local announced = false

gens.registerafter(function()
    if not base then
        base = findBlock()
        if not base then
            if not announced then
                announced = true
                print("Waiting for the ROM: no GAPU block in RAM yet.")
            end
            return
        end
        print(string.format("Found ROM block at 0x%06X", base))
    end

    if os.clock() - syncTimer >= READ_INTERVAL then
        syncTimer = os.clock()
        readMore()
    end

    if #buffer == 0 then return end

    -- Buffer depth is the sync signal.  Too deep means the NES is ahead of us,
    -- so burn a frame; too shallow just means we wait, which the last-written
    -- register state covers for us.
    if #buffer > BUFFER_TARGET * 2 then table.remove(buffer, 1) end

    local f = parseLine(table.remove(buffer, 1))
    if not f then return end

    writeBlocks(f)
    writePcm(f.pcm)

    frames = frames + 1
    if frames % 120 == 0 then
        print(string.format("sync %s  buffer %d  P1 %d/v%d/d%d  NZ p%d",
              f.v2 and "v2" or "v1", #buffer, f.p1_period, f.p1_vol,
              f.p1_duty, f.noise_period))
    end
end)

print("GeNESis-APU2PSG live sync started, tailing " .. FILENAME)
