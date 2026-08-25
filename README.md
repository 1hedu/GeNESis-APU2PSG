<img width="640" height="448" alt="rom-260824-200447" src="https://github.com/user-attachments/assets/e6f8faea-dbc2-4eb5-bdfa-c3f3e3a36be5" />


# GeNESis-APU2PSG: Playback NES audio data on Genesis/Mega Drive

This project is devoted to Krikkz, the inventor of EverDrive.  

I used it to play a MegaDrive port of Super Mario Bros. 1, and it mapped some of the soundtrack to the FM synth. I think it was just the bass, but still very cool to hear it playback with that Sega character. Then I learned, that the Everdrive PRO, actually included a NES core on the FPGA. I was then struck by the idea to do this project--after all it should be possible. 

I chose to start with the PSG only because even without layering FM color, the difference in soundchip and circuitry on the two different hardware platforms, different sample rates, should still produce some distinctly-Sega timbre, in theory.

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

- A live synced version <s>exists,</s> is added.
- <s>Gens r57shell may be hard to find. I downloaded it, and tried a couple days later from the same location, and the link was broken.  I'm working on a BizHawk version of the Gens lua.</s> Link is back.
- Thank you to AlyJames, who helped elucidate the potential of pulse waves on the Genesis, for me, a random DM.

# TODO:


6.  Get the attention of Krikkz, so he might add this to his NES core on his Mega Everdrive PRO
