# GeNESis-APU2PSG: Playback NES audio data on Genesis/Mega Drive

This project is devoted to Krikkz, the inventor of EverDrive.  

I used it to play a MegaDrive port of Super Mario Bros. 1, and it mapped some of the soundtrack to the FM synth. I think it was just the bass, but still very cool to hear it playback with that Sega character. Then I learned, that the Everdrive PRO, actually included a NES core on the FPGA. I just found that so cool. however I was sorry to learn that it does NOT use any of Sega's internal processing, and where possible(it has some limitations I believe mostly related to video), reproduces a faithful copy of the original NES game. I was then struck by the idea to do this project--after all it should be possible. 

I chose to start with the PSG only because even without layering FM color, the difference in soundchip and circuitry on the two different hardware platforms, should still produce some distinctly-Sega timbre, in theory. I thought about how to use the PSG to emulate the Triangle channel, and pulse waves with Duty other than 50%. Ultimately, to avoid the FM synth to the utmost, probably requires Z80 assembly.

Currently, frequency-accurate playback of each channel is working. Volume modulation per NES envelopes is working on all channels except Square 3, which is Triangle on NES.  This is enough to get a song to playback very recognizably.   


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
As long as the NES data file exists, in same directory as gens executable, it should playback the song using the PSG chip.

# To use the live synced version:
Have to have both scripts running at the same time, in the same directory. Turn down the NES emulator audio in OS settings.


# NOTES:

- Alter the filepath in the scripts, to point to same dir, OR put nes_apu_data.txt, in same directory as Gens.exe.
- The noise channel only, must be enabled by pressing A on the controller. If the lua script is not running, this will just blare noise once you enable it.
- A live synced version exists, so that you can directly play your NES game, mute the FCEUX emulator in your OS, and have the Sega emulator running alongside it, playing the audio.
- Gens r57shell may be hard to find. I downloaded it, and tried a couple days later from the same location, and the link was broken.  <s>I'm working on a BizHawk version of the Gens lua.</s> Link is back.
- PWM might be faked on the PSG alone, by setting Freq to max and modulating volume. Will have to compare.

# TODO:
1. <s>Fix Triangle. Not playing correct note lengths.</s>
2. Complete mapping of 32 Noise sounds possible on NES. FM Synth to help if applicable, by layering with noise channel.

3. Timbre tricks and Genesis/MegaDrive FM synth integration.
   
  Some possibilities:
  
  - DC Offset trick + Volume Modulation (VM) to produce pulse waves of various duty, and triangle. 
  - 2 detuned 50% square waves can give us a pulse wave similar to what NES produces. Can we get 3 to sound like 2?
  - FM synth DAC mode channel to play NES DPCM channel.
  - DC Offset trick on 1 FM synth channel using separated operators?
  - FM synth layered over 50% square, to color the waveform appropriately per whichever duty the NES is playing.  (IE 50% square + some FM = 12.5% pulse or 25% or 75%)
      
4. If the PSG-faked PWM, or FM layering, or DC Offset layering, does not produce adequate results, it may be possible to write some custom Z80 assembly, to allow Volume Modulation during a square wave's ON Duty. (IE mute square once it completes half of its ON duty, allow the native 50% cycle to complete while muted, and restore volume during the 50% OFF Duty. This should give us a 25% Duty pulse wave)

5.  Get the attention of Krikkz, so he might add this to his NES core on his Mega Everdrive PRO
