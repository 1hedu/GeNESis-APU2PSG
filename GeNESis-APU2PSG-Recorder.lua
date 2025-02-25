-- Hybrid NES APU Recorder for FCEUX
-- Combines direct register reading with sound.get() for triangle channel
-- For optimal compatibility with Genesis PSG player

local outputFile = io.open("nes_apu_data.txt", "w")
if not outputFile then
    print("Error: Failed to open file for writing")
    return
end

emu.registerbefore(function()
    -- Read APU Status (which channels are active)
    local apu_status = memory.readbyte(0x4015)
    
    -- Get sound data for triangle channel (more accurate for duration)
    local snd = sound.get()
    
    -- Pulse 1 (direct register reading)
    local pulse1_freq = memory.readword(0x4002)
    local pulse1_vol = memory.readbyte(0x4000) % 16
    local pulse1_duty = math.floor(memory.readbyte(0x4000) / 64) % 4
    local pulse1_active = apu_status % 2
    
    -- Pulse 2 (direct register reading)
    local pulse2_freq = memory.readword(0x4006)
    local pulse2_vol = memory.readbyte(0x4004) % 16
    local pulse2_duty = math.floor(memory.readbyte(0x4004) / 64) % 4
    local pulse2_active = math.floor(apu_status / 2) % 2
    
    -- Triangle (hybrid approach)
    local triangle_freq = memory.readword(0x400A)  -- Register for frequency
    local triangle_active_reg = math.floor(apu_status / 4) % 2  -- From register
    
    -- If sound.get triangle volume is 0 but the register shows it active,
    -- the channel might be in a special state - use sound.get for activation
    local triangle_active = (snd.rp2a03.triangle.volume > 0) and 1 or 0
    
    -- Noise (direct register reading)
    local noise_reg = memory.readbyte(0x400E)
    local noise_mode = math.floor(noise_reg / 128) % 2
    local noise_freq = noise_reg % 16
    local noise_vol = memory.readbyte(0x400C) % 16
    local noise_active = math.floor(apu_status / 8) % 2
    
    -- DPCM (direct register reading)
    local dpcm_sample = memory.readbyte(0x4011)
    local dpcm_freq = memory.readbyte(0x4010)
    local dpcm_addr = memory.readbyte(0x4012)
    local dpcm_len = memory.readbyte(0x4013)
    local dpcm_active = math.floor(apu_status / 16) % 2
    
    -- Format data as CSV (comma-separated values)
    local data = string.format(
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        pulse1_freq, pulse1_vol, pulse1_duty, pulse1_active,
        pulse2_freq, pulse2_vol, pulse2_duty, pulse2_active,
        triangle_freq, triangle_active,  -- Using the hybrid approach for triangle
        noise_freq, noise_vol, noise_active, noise_mode,
        dpcm_sample, dpcm_freq, dpcm_addr, dpcm_len
    )
    
    -- Write to file
    outputFile:write(data)
    outputFile:flush()
    
    -- Simple on-screen display
    gui.text(5, 220, "Recording APU data (Hybrid method)")
    gui.text(5, 230, string.format("Triangle Active: %d", triangle_active))
end)

emu.registerexit(function()
    outputFile:close()
    print("NES APU recording complete")
end)

print("Hybrid NES APU Recorder started")
print("Using direct registers + sound.get() for triangle")
