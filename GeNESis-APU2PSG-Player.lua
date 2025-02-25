-- NES to Genesis PSG Player Script
-- For use with Gens emulator (Lua 5.1)

local filename = "nes_apu_data.txt"
local file = io.open(filename, "r")
if not file then
    print("Error: NES APU Data file not found!")
    return
end

local frame_count = 0

-- Convert NES frequency values to Genesis PSG values
local function convertNESFreqToPSG(nesFreq, isTriangle)
    if nesFreq == 0 then return 0 end
    local clock_rate = 1789773
    local psg_base = 3579545
    local nes_actual_hz = clock_rate / (16 * (nesFreq + 1))
    if isTriangle then nes_actual_hz = nes_actual_hz * 0.5 end
    local psg_freq = math.floor(psg_base / (32 * nes_actual_hz))
    return psg_freq
end

-- Bitwise AND implementation for Lua 5.1
local function bit_and(a, b)
    local result = 0
    local bitval = 1
    while a > 0 and b > 0 do
        if a % 2 == 1 and b % 2 == 1 then result = result + bitval end
        bitval = bitval * 2
        a = math.floor(a / 2)
        b = math.floor(b / 2)
    end
    return result
end

-- Main function executed after each frame
function main()
    local line = file:read("*l")
    if not line then
        print("End of NES data stream.")
        file:close()
        return
    end

    local values = {}
    for num in string.gmatch(line, "[^,]+") do
        table.insert(values, tonumber(num) or 0)
    end

    if #values < 14 then
        print("Warning: Malformed line in NES data file, skipping...")
        return
    end

    -- Process pulse channel 1 (square wave 1)
    local pulse1_freq = convertNESFreqToPSG(values[1], false)
    local pulse1_vol = 15 - values[2]
    local pulse1_duty = values[3]
    local pulse1_active = values[4]
    
    -- Process pulse channel 2 (square wave 2)
    local pulse2_freq = convertNESFreqToPSG(values[5], false)
    local pulse2_vol = 15 - values[6]
    local pulse2_active = values[8]

    -- Process triangle channel
    local triangle_freq = convertNESFreqToPSG(values[9] or 0, true)
    local triangle_active = values[10] or 0
    
    -- Length lookup table for the triangle channel
    local length_table = {
        10, 254, 20, 2, 40, 4, 80, 6,
        160, 8, 60, 10, 14, 12, 26, 14,
        12, 16, 24, 18, 48, 20, 96, 22,
        192, 24, 72, 26, 16, 28, 32, 30
    }

    local length_index = math.floor((values[11] or 0) / 8)
    local triangle_length = length_table[length_index + 1] or 0
    local triangle_mode = values[12] or 0

    -- Process noise channel - CORRECTED INDICES
    local noise_reg = values[11] or 0     -- Changed from 13
    local noise_vol_reg = values[12] or 0 -- Changed from 14
    local noise_active = values[13] or 0  -- Changed from 15

    -- Noise envelope handling
    local envelope_enabled = bit_and(noise_vol_reg, 0x10) ~= 0
    local envelope_loop = bit_and(noise_vol_reg, 0x20) ~= 0
    local envelope_period = bit_and(noise_vol_reg, 0x0F)
    
    local noise_vol
    if envelope_enabled then
        local decay = math.floor(frame_count / envelope_period)
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

    -- Write pulse 1 channel data to Genesis memory
    if pulse1_active == 1 then
        memory.writebyte(0xFF0000, pulse1_freq % 256)
        memory.writebyte(0xFF0001, math.floor(pulse1_freq / 256))
        memory.writebyte(0xFF0002, pulse1_vol)
        memory.writebyte(0xFF0008, pulse1_duty)
    else
        memory.writebyte(0xFF0002, 15)  -- Mute
        memory.writebyte(0xFF0008, 0)
    end

    -- Write pulse 2 channel data to Genesis memory
    if pulse2_active == 1 then
        memory.writebyte(0xFF0003, pulse2_freq % 256)
        memory.writebyte(0xFF0004, math.floor(pulse2_freq / 256))
        memory.writebyte(0xFF0005, pulse2_vol)
    else
        memory.writebyte(0xFF0005, 15)  -- Mute
    end

    -- Triangle channel length and mode handling
    if triangle_mode >= 128 then
        triangle_length = 255  -- Play indefinitely if Constant Mode is set
    elseif triangle_length == 0 then
        triangle_active = 0  -- If length expires, disable triangle
    end

    -- Triangle envelope simulation
    local triangle_psg_vol = 0  -- Default (max volume)
    if triangle_active == 1 and triangle_freq > 0 then
        local decay_factor = math.max(0, 15 - math.floor(triangle_length / 16))
        triangle_psg_vol = decay_factor
    end

    -- Write triangle channel data to Genesis memory
    if triangle_active == 1 and triangle_freq > 0 then
        memory.writebyte(0xFF000A, triangle_freq % 256)
        memory.writebyte(0xFF000B, math.floor(triangle_freq / 256))
        memory.writebyte(0xFF000C, triangle_psg_vol)
        memory.writebyte(0xFF000D, 1)  -- Ensure it's active
    else
        memory.writebyte(0xFF000A, 0)  -- Mute triangle frequency
        memory.writebyte(0xFF000B, 0)
        memory.writebyte(0xFF000C, 15)  -- Mute volume
        memory.writebyte(0xFF000D, 0)
    end

    -- Write noise channel data to Genesis memory using the same formula as the working version
    if noise_active == 1 then
        memory.writebyte(0xFF0006, noise_value)
        memory.writebyte(0xFF0007, 10 - noise_vol)
    else
        memory.writebyte(0xFF0007, 15)  -- Mute noise
    end

    frame_count = frame_count + 1
    
    -- Debug output
    print(string.format("Triangle: Freq=%d, Length=%d, Active=%d, Vol=%d", 
          triangle_freq, triangle_length, triangle_active, triangle_psg_vol))
    print(string.format("Noise: Value=0x%X, Vol=%d, Active=%d, Indices=[%d,%d,%d]", 
          noise_value, noise_vol, noise_active, noise_reg, noise_vol_reg, noise_active))
end

-- Register the main function to run after each frame
gens.registerafter(main)
