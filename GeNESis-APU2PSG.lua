local outputFile = io.open("nes_apu_data.txt", "w")

if not outputFile then
    print("Error: Failed to open NES APU data file for writing.")
    return
end

emu.registerbefore(function()
    local apu_status = memory.readbyte(0x4015)

    -- Pulse 1
    local pulse1_freq = memory.readword(0x4002) % 2048
    local pulse1_vol = memory.readbyte(0x4000) % 16
    local pulse1_duty = math.floor(memory.readbyte(0x4000) / 64) % 4
    local pulse1_active = apu_status % 2

    -- Pulse 2
    local pulse2_freq = memory.readword(0x4006) % 2048
    local pulse2_vol = memory.readbyte(0x4004) % 16
    local pulse2_duty = math.floor(memory.readbyte(0x4004) / 64) % 4
    local pulse2_active = math.floor(apu_status / 2) % 2

    -- Triangle
    local triangle_freq = memory.readword(0x400A) % 2048
    local triangle_active = math.floor(apu_status / 4) % 2
    local triangle_mode = memory.readbyte(0x4008) or 0
    local triangle_length_index = memory.readbyte(0x400B) or 0

    -- Length Lookup Table
    local length_table = {
        10, 254, 20, 2, 40, 4, 80, 6,
        160, 8, 60, 10, 14, 12, 26, 14,
        12, 16, 24, 18, 48, 20, 96, 22,
        192, 24, 72, 26, 16, 28, 32, 30
    }

    local triangle_length = length_table[math.floor(triangle_length_index / 8) + 1] or 0

    if triangle_mode >= 128 then
        triangle_length = 255  -- Play indefinitely
    end

    -- Noise
    local noise_reg = memory.readbyte(0x400E)
    local noise_mode = math.floor(noise_reg / 128) % 2
    local noise_freq = noise_reg % 16
    local noise_vol = memory.readbyte(0x400C) % 16
    local noise_active = math.floor(apu_status / 8) % 2

    -- DPCM
    local dpcm_sample = memory.readbyte(0x4011)
    local dpcm_freq = memory.readbyte(0x4010)
    local dpcm_addr = memory.readbyte(0x4012)
    local dpcm_len = memory.readbyte(0x4013)
    local dpcm_active = math.floor(apu_status / 16) % 2

    local data = string.format(
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        pulse1_freq, pulse1_vol, pulse1_duty, pulse1_active,
        pulse2_freq, pulse2_vol, pulse2_duty, pulse2_active,
        triangle_freq, triangle_active, triangle_length, triangle_mode,
        noise_freq, noise_vol, noise_active, noise_mode,
        dpcm_sample, dpcm_freq, dpcm_addr, dpcm_len
    )

    print("NES APU Data:", data)

    outputFile:write(data)
    outputFile:flush()
end)

emu.registerexit(function()
    if outputFile then
        outputFile:close()
    end
end)
