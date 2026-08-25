#!/usr/bin/env node
/*
 * gen_apu_capture.js -- regenerate nes_apu_data.txt from a user-supplied NES
 * ROM, instead of shipping the capture (a derivative of the game's own
 * copyrighted music) in this repository.
 *
 * The capture format encodes a specific game's melody -- pitches, durations,
 * envelopes -- which is exactly the part of a ROM that copyright protects.
 * The button-input schedule below is not that: it is a sequence of NES
 * controller presses, timed to reach a stretch of real gameplay, and holds
 * no game content of its own. Given someone's own legally-obtained ROM, this
 * script drives the game and reads the same 26 fields the FCEUX Lua recorder
 * (GeNESis-APU2PSG-Recorder.lua) reads, so the output is consumed identically
 * by every downstream tool (tools/gen_apudata.py, tools/gen_dpcm.py) without
 * distributing the game's own audio data.
 *
 * Uses jsnes (github.com/jsnes/jsnes) because it is a from-scratch NES
 * implementation with every APU register write visible as a plain JS call --
 * no native emulator core to fight for headless operation, and no GUI to
 * crash under a virtual display (both were tried first; see BUILDING.md).
 *
 * Usage:
 *   node tools/gen_apu_capture.js rom.nes schedule.txt frames -o out.txt
 *
 * Schedule file: one line per input event, "startFrame holdFrames buttonMask"
 * (mask bits: A=0 B=1 SELECT=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7). Blank and
 * '#'-comment lines are ignored.
 */
const fs = require("fs");
const { NES } = require("jsnes");

function usage() {
  console.error("usage: gen_apu_capture.js rom.nes schedule.txt frames [-o out.txt]");
  process.exit(1);
}

const args = process.argv.slice(2);
if (args.length < 3) usage();
const [romPath, schedPath, framesArg] = args;
const totalFrames = parseInt(framesArg, 10);
const oIdx = args.indexOf("-o");
const outPath = oIdx >= 0 ? args[oIdx + 1] : "nes_apu_data.txt";

const schedule = [];
if (fs.existsSync(schedPath)) {
  for (const line of fs.readFileSync(schedPath, "utf8").split("\n")) {
    const t = line.trim();
    if (!t || t.startsWith("#")) continue;
    const [start, hold, mask] = t.split(/\s+/).map(Number);
    schedule.push({ start, end: start + hold, mask });
  }
}

// ---- raw register shadow, exactly mirroring what memory.readbyte(0x40xx)
// returns in FCEUX: the last byte written, unmasked, regardless of what the
// engine does internally with it. -----------------------------------------
const reg = new Uint8Array(0x18); // $4000-$4017
let trigFlag = 0, trigAddr = 0, trigLen = 0, trigRate = 0;

const nes = new NES({
  onFrame: function () {},
  onAudioSample: null,
});

const origWriteReg = nes.papu.writeReg.bind(nes.papu);
nes.papu.writeReg = function (address, value) {
  if (address >= 0x4000 && address <= 0x4017) {
    reg[address - 0x4000] = value & 0xff;
    // DMC trigger latch: mirrors the Lua recorder's memory.registerwrite hook
    // on $4015. A per-frame poll can miss a same-frame rewrite of $4012/$4013
    // right after the trigger; latching at the write instant cannot.
    if (address === 0x4015 && value & 0x10) {
      trigFlag = 1;
      trigAddr = reg[0x12]; // $4012, already latched above since writes are in order
      trigLen = reg[0x13];  // $4013
      trigRate = reg[0x10] & 0x0f;
    }
  }
  return origWriteReg(address, value);
};

// The published dist build (npm's "jsnes") is a binary-string-only ROM
// loader, older than the Buffer/Uint8Array-accepting version in its GitHub
// source; latin1 round-trips every byte value 1:1, so this is lossless.
const romData = fs.readFileSync(romPath).toString("binary");
nes.loadROM(romData);

const out = fs.createWriteStream(outPath);
out.write("#GAPU2 v2\n");

const BTN = { A: 0, B: 1, SELECT: 2, START: 3, UP: 4, DOWN: 5, LEFT: 6, RIGHT: 7 };
const BTN_BY_BIT = [BTN.A, BTN.B, BTN.SELECT, BTN.START, BTN.UP, BTN.DOWN, BTN.LEFT, BTN.RIGHT];

let held = 0;
function applyInput(frame) {
  let want = 0;
  for (const ev of schedule) if (frame >= ev.start && frame < ev.end) want |= ev.mask;
  const changed = want ^ held;
  for (let bit = 0; bit < 8; bit++) {
    if ((changed >> bit) & 1) {
      const btn = BTN_BY_BIT[bit];
      if ((want >> bit) & 1) nes.buttonDown(1, btn);
      else nes.buttonUp(1, btn);
    }
  }
  held = want;
}

const sq1 = nes.papu.square1, sq2 = nes.papu.square2, tri = nes.papu.triangle,
      noi = nes.papu.noise, dmc = nes.papu.dmc;

for (let frame = 0; frame < totalFrames; frame++) {
  applyInput(frame);
  nes.frame();

  const p1_period = (reg[0x02] | (reg[0x03] << 8)) % 2048;
  const p1_reg = reg[0x00];
  const p1_duty = Math.floor(p1_reg / 64) % 4;
  const p1_on = sq1.getLengthStatus();
  const p1_vol = sq1.masterVolume;

  const p2_period = (reg[0x06] | (reg[0x07] << 8)) % 2048;
  const p2_reg = reg[0x04];
  const p2_duty = Math.floor(p2_reg / 64) % 4;
  const p2_on = sq2.getLengthStatus();
  const p2_vol = sq2.masterVolume;

  const tri_period = (reg[0x0a] | (reg[0x0b] << 8)) % 2048;
  const tri_on = tri.getLengthStatus();

  const noise_reg = reg[0x0e];
  const noise_mode = Math.floor(noise_reg / 128) % 2;
  const noise_period = noise_reg % 16;
  const noise_on = noi.getLengthStatus();
  const noise_vol = noi.masterVolume;

  const dpcm_sample = reg[0x11];
  const dpcm_freq = reg[0x10];
  const dpcm_addr = reg[0x12];
  const dpcm_len = reg[0x13];
  const dpcm_on = dmc.getLengthStatus();

  const fields = [
    p1_period, p1_reg % 16, p1_duty, p1_on,
    p2_period, p2_reg % 16, p2_duty, p2_on,
    tri_period, tri_on,
    noise_period, noise_vol, noise_on, noise_mode,
    dpcm_sample, dpcm_freq, dpcm_addr, dpcm_len,
    p1_vol, p2_vol, dpcm_on, frame % 256,
    trigFlag, trigAddr, trigLen, trigRate,
  ];
  out.write(fields.join(",") + "\n");

  trigFlag = 0; trigAddr = 0; trigLen = 0; trigRate = 0;
}

out.end(() => {
  console.error("wrote " + totalFrames + " frames -> " + outPath);
});
