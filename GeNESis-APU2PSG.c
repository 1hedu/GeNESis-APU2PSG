#include <genesis.h>
#include <z80_ctrl.h>
#include <psg.h>

#define TRIANGLE_MAX_VOL 4  // Adjustable: 0 (max) to 14 (soft), 15 = mute

void joyEvent(u16 joy, u16 changed, u16 state) {}

int selectedChannel = 0;
int channelActive[4] = {0, 0, 0, 0};  
int channelFreq[4] = {440, 440, 440, 440};  
int channelVol[4] = {4, 4, 4, 4};  

u16 prevButtons = 0;

u16 nesPulse1Freq, nesPulse1Vol;
u16 nesPulse2Freq, nesPulse2Vol;
u16 nesTriangleFreq, nesTriangleVol;
u8 nesNoiseValue, nesNoiseVol;
u8 nesFrameDelta;
u8 nesPulse1Duty;
u8 nesTriangleActive;

bool manualNoiseControl = FALSE;

const char* getNoiseTypeName(u8 type) {
    return type ? "WHITE" : "PERIODIC";
}

const char* getNoiseFreqName(u8 freq) {
    switch(freq) {
        case 0: return "HIGH";
        case 1: return "MED";
        case 2: return "LOW";
        default: return "CH2";
    }
}

void updateChannel(int channel) {
    Z80_requestBus(TRUE);
    if (channelActive[channel]) {
        PSG_setTone(channel, channelFreq[channel]);
        PSG_setEnvelope(channel, channelVol[channel]);
    } else {
        PSG_setEnvelope(channel, 15); 
    }
    Z80_releaseBus();
}

int main() {
    Z80_requestBus(TRUE);
    Z80_startReset();
    Z80_endReset();
    Z80_setBank(0);
    PSG_reset();
    Z80_releaseBus();

    JOY_setEventHandler(joyEvent);

    while(TRUE) {
        JOY_update();
        u16 joypad = JOY_readJoypad(JOY_1);
        u16 changed = joypad & ~prevButtons;

        if (changed & BUTTON_X) { channelActive[0] = !channelActive[0]; updateChannel(0); }
        if (changed & BUTTON_Y) { channelActive[1] = !channelActive[1]; updateChannel(1); }
        if (changed & BUTTON_Z) { channelActive[2] = !channelActive[2]; updateChannel(2); }
        if (changed & BUTTON_A) { channelActive[3] = !channelActive[3]; }

        if (changed & BUTTON_RIGHT) { 
            if (selectedChannel != 3 || !manualNoiseControl) {
                selectedChannel = (selectedChannel + 1) % 4;
            } else {
                u8 currentType = (nesNoiseValue >> 2) & 0x01;
                u8 newType = !currentType;
                u8 currentFreq = nesNoiseValue & 0x03;
                nesNoiseValue = (newType << 2) | currentFreq;
            }
        }
        if (changed & BUTTON_LEFT) {
            if (selectedChannel != 3 || !manualNoiseControl) {
                selectedChannel = (selectedChannel - 1 + 4) % 4;
            } else {
                u8 currentType = (nesNoiseValue >> 2) & 0x01;
                u8 newType = !currentType;
                u8 currentFreq = nesNoiseValue & 0x03;
                nesNoiseValue = (newType << 2) | currentFreq;
            }
        }

        if (selectedChannel != 3 || !manualNoiseControl) {
            if (changed & BUTTON_C) { 
                channelFreq[selectedChannel] *= 2; 
                if (channelFreq[selectedChannel] > 4000) 
                    channelFreq[selectedChannel] = 4000; 
                updateChannel(selectedChannel); 
            }
            if (changed & BUTTON_B) { 
                channelFreq[selectedChannel] /= 2; 
                if (channelFreq[selectedChannel] < 55) 
                    channelFreq[selectedChannel] = 55; 
                updateChannel(selectedChannel); 
            }
        } else {
            if (changed & BUTTON_C) {
                u8 currentType = (nesNoiseValue >> 2) & 0x01;
                u8 currentFreq = nesNoiseValue & 0x03;
                u8 newFreq = (currentFreq + 1) % 3;
                nesNoiseValue = (currentType << 2) | newFreq;
            }
            if (changed & BUTTON_B) {
                u8 currentType = (nesNoiseValue >> 2) & 0x01;
                u8 currentFreq = nesNoiseValue & 0x03;
                u8 newFreq = (currentFreq + 2) % 3;
                nesNoiseValue = (currentType << 2) | newFreq;
            }
        }

        if (changed & BUTTON_UP) {
            if (selectedChannel == 3 && manualNoiseControl) {
                if (nesNoiseVol > 0) nesNoiseVol--;
            } else {
                if (channelVol[selectedChannel] < 15) 
                    channelVol[selectedChannel]++; 
                updateChannel(selectedChannel);
            }
        }
        if (changed & BUTTON_DOWN) {
            if (selectedChannel == 3 && manualNoiseControl) {
                if (nesNoiseVol < 15) nesNoiseVol++;
            } else {
                if (channelVol[selectedChannel] > 0) 
                    channelVol[selectedChannel]--; 
                updateChannel(selectedChannel);
            }
        }

        if (!manualNoiseControl) {
            nesPulse1Freq = (*(volatile u8*)0xFF0000) | ((*(volatile u8*)0xFF0001) << 8);
            nesPulse1Vol = *(volatile u8*)0xFF0002;
            nesPulse2Freq = (*(volatile u8*)0xFF0003) | ((*(volatile u8*)0xFF0004) << 8);
            nesPulse2Vol = *(volatile u8*)0xFF0005;
            nesNoiseValue = *(volatile u8*)0xFF0006;
            nesNoiseVol = *(volatile u8*)0xFF0007;
            nesPulse1Duty = *(volatile u8*)0xFF0008;
            nesFrameDelta = *(volatile u8*)0xFF0009;
            nesTriangleFreq = (*(volatile u8*)0xFF000A) | ((*(volatile u8*)0xFF000B) << 8);
            nesTriangleVol = *(volatile u8*)0xFF000C;
            nesTriangleActive = *(volatile u8*)0xFF000D;
        }

        Z80_requestBus(TRUE);

        if (nesPulse1Freq > 0) {
            PSG_setTone(0, nesPulse1Freq);
            PSG_setEnvelope(0, nesPulse1Vol);
        } else {
            PSG_setEnvelope(0, 15);
        }

        if (nesPulse2Freq > 0) {
            PSG_setTone(1, nesPulse2Freq);
            PSG_setEnvelope(1, nesPulse2Vol);
        } else {
            PSG_setEnvelope(1, 15);
        }

        if (nesTriangleActive == 1) {  // Active check first, freq secondary
            PSG_setTone(2, nesTriangleFreq > 0 ? nesTriangleFreq : 0);
            PSG_setEnvelope(2, TRIANGLE_MAX_VOL);
        } else {
            PSG_setTone(2, 0);  // Explicitly zero freq
            PSG_setEnvelope(2, 15);  // Force mute
        }

        if (channelActive[3]) {
            u8 type = (nesNoiseValue >> 2) & 0x01;
            u8 frequency = nesNoiseValue & 0x03;
            PSG_setNoise(type, frequency);
            PSG_setEnvelope(3, nesNoiseVol);
        } else {
            PSG_setEnvelope(3, 15);
        }

        Z80_releaseBus();

        if (!manualNoiseControl) {
            *(volatile u8*)0xFF0000 = 0;
            *(volatile u8*)0xFF0001 = 0;
            *(volatile u8*)0xFF0002 = 0;
            *(volatile u8*)0xFF0003 = 0;
            *(volatile u8*)0xFF0004 = 0;
            *(volatile u8*)0xFF0005 = 0;
            *(volatile u8*)0xFF0006 = 0;
            *(volatile u8*)0xFF0007 = 0;
            *(volatile u8*)0xFF0008 = 0;
            *(volatile u8*)0xFF0009 = 0;
            *(volatile u8*)0xFF000A = 0;
            *(volatile u8*)0xFF000B = 0;
            *(volatile u8*)0xFF000C = 0;
            *(volatile u8*)0xFF000D = 0;
        }

        prevButtons = joypad;

        VDP_clearText(5, 5, 40);
        char debugText[60];
        sprintf(debugText, "Selected CH%d %s", selectedChannel + 1,
            selectedChannel == 3 && manualNoiseControl ? "(MANUAL)" : "");
        VDP_drawText(debugText, 5, 12);

        sprintf(debugText, "P1: %d N, V: %d, D: %d", nesPulse1Freq, nesPulse1Vol, nesPulse1Duty);
        VDP_drawText(debugText, 5, 13);

        sprintf(debugText, "P2: %d N, V: %d", nesPulse2Freq, nesPulse2Vol);
        VDP_drawText(debugText, 5, 14);

        sprintf(debugText, "TRI: %d N, V: %d, A: %d", nesTriangleFreq, nesTriangleVol, nesTriangleActive);
        VDP_drawText(debugText, 5, 15);

        u8 noiseType = (nesNoiseValue >> 2) & 0x01;
        u8 noiseFreq = nesNoiseValue & 0x03;
        sprintf(debugText, "NOISE: %s %s V:%d %s", 
            getNoiseTypeName(noiseType),
            getNoiseFreqName(noiseFreq),
            nesNoiseVol,
            channelActive[3] ? "ON" : "OFF");
        VDP_drawText(debugText, 5, 16);

        if (selectedChannel == 3) {
            VDP_drawText("A:Manual L/R:Type B/C:Freq UP/DN:Vol", 5, 20);
        }

        VDP_waitVSync();
    }

    return 0;
}
