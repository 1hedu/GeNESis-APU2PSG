-- NES to Genesis PSG Live Sync Script
-- For use with Gens emulator (Lua 5.1)

local filename = "nes_apu_data.txt" --Make sure to point to same location as NES lua
local file
local last_pos = 0  
local buffer = {}    
local sync_timer = os.clock()  
local BUFFER_SIZE = 4  

local SYNC_INTERVAL = 240  
local last_nes_frame = 0
local genesis_frame_count = 0

-- Function to open and prepare the data file
local function openFile()
    file = io.open(filename, "r")
    if file then
        print("✅ NES APU Data file found! Seeking to end of file...")
        file:seek("end")  
        last_pos = file:seek()
    else
        print("⚠️ NES APU Data file not found! Waiting for file...")
    end
end

-- Initial file opening
openFile()

-- Convert NES frequency values to Genesis PSG values with pitch adjustment
local function convertNESFreqToPSG(nesFreq, applyPitchDown)
    if nesFreq == 0 then return 0, 0 end
    local clock_rate = 1789773
    local psg_base = 3579545
    local nes_actual_hz = clock_rate / (16 * (nesFreq + 1))

    if applyPitchDown then 
        nes_actual_hz = nes_actual_hz * 0.5  -- Triangle channel pitch adjustment
    end

    local psg_freq = math.floor(psg_base / (32 * nes_actual_hz))

    -- Calculate actual frequency for debugging
    local debug_hz = (psg_base / (32 * psg_freq))  

    return psg_freq, debug_hz
end

-- Bitwise AND implementation for Lua 5.1
local function bit_and(a, b)
    local result, bitval = 0, 1
    while a > 0 and b > 0 do
        if a % 2 == 1 and b % 2 == 1 then result = result + bitval end
        bitval = bitval * 2
        a, b = math.floor(a / 2), math.floor(b / 2)
    end
    return result
end

-- Read new data from the file
local function readAPUData()
    if not file then openFile() end
    if not file then return end  

    file:seek("set", last_pos)  
    for i = 1, BUFFER_SIZE do  
        local line = file:read("*l")
        if not line then break end  
        table.insert(buffer, line)
        last_pos = file:seek()
    end
end

-- Process one frame of APU data
local function processAPUFrame()
    if #buffer == 0 then return end  

    local line = table.remove(buffer, 1)  
    local values = {}
    for num in string.gmatch(line, "[^,]+") do
        table.insert(values, tonumber(num) or 0)
    end

    if #values < 14 then
        print("⚠️ Warning: Malformed line in NES data file, skipping...")
        return
    end

    -- Extract frame number for synchronization
    local nes_frame = values[14] or 0  
    if last_nes_frame == 0 then last_nes_frame = nes_frame end  

    -- Process pulse channel 1
    local pulse1_freq = convertNESFreqToPSG(values[1], false)
    local pulse1_vol = 15 - values[2]
    local pulse1_active = values[4]
    
    -- Process pulse channel 2
    local pulse2_freq = convertNESFreqToPSG(values[5], false)
    local pulse2_vol = 15 - values[6]
    local pulse2_active = values[8]

    -- Process triangle channel with pitch shift
    local triangle_freq = convertNESFreqToPSG(values[9], true)
    local triangle_vol = 15 - values[10]  
    local triangle_active = values[10] > 0  

    -- Process noise channel
    local noise_reg = values[11]
    local noise_vol_reg = values[12]
    local noise_active = values[13]
    
    -- Handle noise envelope
    local envelope_enabled = bit_and(noise_vol_reg, 0x10) ~= 0
    local envelope_loop = bit_and(noise_vol_reg, 0x20) ~= 0
    local envelope_period = bit_and(noise_vol_reg, 0x0F)
    
    local noise_vol
    if envelope_enabled then
        local decay = math.floor(genesis_frame_count / envelope_period)
        if envelope_loop then 
            decay = decay % 16 
        else 
            decay = math.min(decay, 15) 
        end
        noise_vol = decay
    else
        noise_vol = bit_and(noise_vol_reg, 0x0F)
    end
    
    -- Map NES noise parameters to Genesis PSG values
    local noise_mode = math.floor(noise_reg / 128) % 2
    local noise_period = noise_reg % 16
    local noise_value
    if noise_period == 8 then
        noise_value = 0xE4
    elseif noise_period == 13 then
        noise_value = 0xE6
    else
        local base = noise_mode == 1 and 0xE4 or 0xE0
        local freq = (noise_period < 2 and 0) or (noise_period < 3 and 1) or 2
        noise_value = base + freq
    end

    -- Write pulse channel 1 data to Genesis memory
    if pulse1_active == 1 then
        memory.writebyte(0xFF0000, pulse1_freq % 256)
        memory.writebyte(0xFF0001, math.floor(pulse1_freq / 256))
        memory.writebyte(0xFF0002, pulse1_vol)
    else
        memory.writebyte(0xFF0002, 15)  -- Mute
    end

    -- Write pulse channel 2 data to Genesis memory
    if pulse2_active == 1 then
        memory.writebyte(0xFF0003, pulse2_freq % 256)
        memory.writebyte(0xFF0004, math.floor(pulse2_freq / 256))
        memory.writebyte(0xFF0005, pulse2_vol)
    else
        memory.writebyte(0xFF0005, 15)  -- Mute
    end

    -- Write triangle channel data to Genesis memory
    if triangle_active then
        memory.writebyte(0xFF000A, triangle_freq % 256)
        memory.writebyte(0xFF000B, math.floor(triangle_freq / 256))
        memory.writebyte(0xFF000C, triangle_vol)
        memory.writebyte(0xFF000D, 1)  -- Ensure it's ON
    else
        memory.writebyte(0xFF000C, 15)  -- Mute
        memory.writebyte(0xFF000D, 0)   -- Disable
    end

    -- Write noise channel data to Genesis memory
    if noise_active == 1 then
        memory.writebyte(0xFF0006, noise_value)
        memory.writebyte(0xFF0007, 10 - noise_vol)
    else
        memory.writebyte(0xFF0007, 15)  -- Mute
    end

    -- Debug output
    print(string.format("TRI: %d Hz | VOL: %d | ACTIVE: %d", 
          triangle_freq, triangle_vol, triangle_active and 1 or 0))

    -- Synchronization logic
    if genesis_frame_count % SYNC_INTERVAL == 0 then
        local frame_diff = nes_frame - last_nes_frame
        if frame_diff > SYNC_INTERVAL + 2 then
            print("⚠️ Genesis lagging behind, skipping a frame...")
            table.remove(buffer, 1)
        elseif frame_diff < SYNC_INTERVAL - 2 then
            print("⚠️ Genesis ahead, waiting a frame...")
            buffer = {}
        end
        last_nes_frame = nes_frame
    end

    genesis_frame_count = genesis_frame_count + 1
end

-- Register the main function to run after each frame
gens.registerafter(function()
    -- Read new data approximately every 33ms (30fps)
    if os.clock() - sync_timer >= 0.033 then  
        sync_timer = os.clock()
        readAPUData()
    end
    
    -- Process the next frame of data
    processAPUFrame()
end)
