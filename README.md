# GeNESis-APU2PSG
Playback NES audio data on Genesis/Mega Drive

Requirements:
- SGDK
- FCEUX NES Emulator
- Gens r57Shell Genesis/MegaDrive Emulator

You must use SGDK to build the ROM.

# To record NES audio:
Open up FCEUX, load GeNESis-APU2PSG-Recorder lua script. It should run without error. 
Load a NES Rom file and the script will imediately start logging the audio data

# To playback NES audio on Genesis/MegaDrive:
Both the Sega ROM and the lua script GeNESis-APU2PSG-Player must be loaded. 
As long as the NES data file exists, it should playback the song using the PSG chip.


# NOTES:

- The noise channel only, must be enabled by pressing A on the controller. If the lua script is not running, this will just blare noise once you enable it.
- A live synced version exists, so that you can directly play your NES game, mute the FCEUX emulator in your OS, and have the Sega emulator running alongside it, playing the audio.
- Gens r57shell may be hard to find. I downloaded it, and tried a couple days later from the same location, and the link was broken.  I'm working on a BizHawk version of the Gens lua.

# TODO:
1. Fix Triangle. Not playing correct note lengths.
2. Complete mapping of 32 Noise sounds possible on NES. FM Synth to help if applicable, by layering with noise channel.

3. Genesis/MegaDrive FM synth integration.

Some possibilities:
- DC Offset trick + Volume Modulation (VM) to produce pulse waves of various duty, and triangle. 
- 2 detuned 50% square waves can give us a pulse wave similar to what NES produces. Can we get 3 to sound like 2?
- FM synth DAC mode channel to play NES DPCM channel.
- DC Offset trick on 1 FM synth channel using separated operators?
- FM synth layered over 50% square, to color the waveform appropriately per whichever duty the NES is playing.  (IE 50% square + some FM = 12.5% pulse or 25% or 75%)

4. If the FM layering does not produce adequate results, it may be possible to write some custom Z80 assembly, to allow Volume Modulation during a square wave's ON Duty. (IE mute square once it completes half of its ON duty, allow the native 50% cycle to complete while muted, and restore volume during the 50% OFF Duty. This should give us a 25% Duty pulse wave)
