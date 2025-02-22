# APU2PSG
Playback NES audio data on Genesis/Mega Drive

Requirements:
- SGDK
- FCEUX NES Emulator
- Gens r57Shell Genesis/MegaDrive Emulator

You must use SGDK to build the ROM.

# To record NES audio:
Open up FCEUX, load GeNESis-APU2PSG lua script. It should run without error.
Load a NES Rom file and the script will imediately start logging the audio data

# To playback NES audio on Genesis/MegaDrive:
Both the Sega ROM and the lua script must be loaded.
As long as the NES data file exists, it should playback the song using the PSG chip.


# NOTES:

-The noise channel only, must be enabled by pressing A on the controller. If the lua script is not running, this will just blare noise once you enable it.

-A live synced version exists, so that you can directly play your NES game, mute the FCEUX emulator in your OS, and have the Sega emulator running alongside it, playing the audio.

# TODO:
1. Fix Triangle. Not playing correct note lengths.
2. Complete mapping of 32 Noise sounds possible on NES. FM Synth to help if applicable, by layering with noise channel.

3. Genesis/MegaDrive FM synth integration.

Some possibilities:
- DC Offset trick + Volume Modulation (VM) to produce pulse waves of various duty, and triangle. 
- 2 detuned 50% square waves can give us a pulse wave similar to what NES produces. Can we get 3 to sound like 2?
- FM synth DAC mode channel to play NES DPCM channel.
- DC Offset trick on 1 FM synth channel using separated operators?
- FM synth layerd over 50% square, to color the waveform appropriately per whichever duty the NES is playing.  (IE 50% square + some FM = 12.5% pulse or 25% or 75%)
