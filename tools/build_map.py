#!/usr/bin/env python3
"""Build the APU2PSG technique map artifact. Every coordinate and statistic is
derived from the driver's real cycle counts and the repo's own capture file."""
import math, html

NES = 1789773.0
PSGCLK = 3579545.0

# ---- driver facts, straight from the assembler ------------------------------
VOICE_CYCLES = {"pulse": 63, "wave": 89, "pcm": 31, "overhead": 18}
VARIANTS = [
    ("V3",  "pulse + pulse + triangle",       ["pulse", "pulse", "wave", "overhead"]),
    ("V2",  "pulse + pulse",                  ["pulse", "pulse", "overhead"]),
    ("V2D", "pulse + pulse + PCM",            ["pulse", "pulse", "pcm", "overhead"]),
]
def rate(cyc): return PSGCLK / cyc
def total(parts): return sum(VOICE_CYCLES[p] for p in parts)

V = {}
for name, desc, parts in VARIANTS:
    c = total(parts)
    V[name] = {"cycles": c, "rate": rate(c), "ceil8": rate(c) / 8, "ceil32": rate(c) / 32,
               "desc": desc, "parts": parts}

PSG_FLOOR = PSGCLK / (32 * 1023)          # lowest note the tone generator can make
NES_TRI_FLOOR = NES / (32 * 2048)

# ---- measurements from the checked-in capture -------------------------------
pulse, tri, duty, noise = [], [], {0:0,1:0,2:0,3:0}, {}
for line in open("nes_apu_data.txt"):
    v = line.strip().split(",")
    if len(v) < 14: continue
    v = [int(x) for x in v]
    p, t = v[0] & 0x7FF, v[8] & 0x7FF
    if v[3]:
        duty[v[2]] += 1
        if p: pulse.append(NES / (16 * (p + 1)))
    if v[9] and t: tri.append(NES / (32 * (t + 1)))
    if v[12]: noise[v[10] & 15] = noise.get(v[10] & 15, 0) + 1
pulse.sort(); tri.sort()
def q(a, f): return a[min(len(a) - 1, int(len(a) * f))]
NOISE_TOTAL = sum(noise.values())
DUTY_NON50 = 100.0 * (duty[0] + duty[1] + duty[3]) / sum(duty.values())
PULSE_UNDER_V3 = 100.0 * sum(1 for f in pulse if f <= V["V3"]["ceil8"]) / len(pulse)

# ---- noise mapping ----------------------------------------------------------
NESPER = [4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068]
FIXED = {0: 16, 1: 32, 2: 64}
noise_rows = []
for i, p in enumerate(NESPER):
    ideal = PSGCLK * p / (16 * NES)
    best = min(FIXED, key=lambda r: abs(math.log(FIXED[r] / max(ideal, 0.25))))
    err = 100 * (FIXED[best] - ideal) / ideal
    ch2 = max(1, min(1023, round(ideal)))
    ch2err = 100 * (ch2 - ideal) / ideal
    noise_rows.append({
        "idx": i, "nesper": p, "shift": NES / p, "ideal": ideal,
        "fixed_err": err, "needs_ch2": abs(err) > 25,
        "ch2": ch2, "ch2err": ch2err, "reach": abs(ch2err) <= 25,
        "use": 100.0 * noise.get(i, 0) / NOISE_TOTAL,
    })
NEEDS_CH2_SHARE = sum(r["use"] for r in noise_rows if r["needs_ch2"])
REACHABLE = sum(1 for r in noise_rows if r["reach"])

# ---- pitch axis -------------------------------------------------------------
FMIN, FMAX = 24.0, 4400.0
def lx(f): return (math.log(f) - math.log(FMIN)) / (math.log(FMAX) - math.log(FMIN))
OCTAVES = [27.5, 55, 110, 220, 440, 880, 1760, 3520]
OCTNAME = ["A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"]

# =============================================================================
# figure 1 -- the pitch map
# =============================================================================
W, PAD_L, PAD_R = 1000, 148, 26
PLOT = W - PAD_L - PAD_R
LANE_H, LANE_GAP = 46, 26
def X(f): return PAD_L + lx(f) * PLOT

def band(f0, f1, y, cls, label, sub=""):
    x0, x1 = X(f0), X(f1)
    w = x1 - x0
    tip = "%s &middot; %s&ndash;%s Hz" % (label, fmt(f0), fmt(f1))
    if sub: tip += " &middot; " + sub
    inner = ""
    if w > 96:
        inner = ('<text class="bandlabel" x="%.1f" y="%.1f">%s</text>'
                 % (x0 + 12, y + LANE_H / 2 + 4, html.escape(label)))
    return ('<g class="band" data-tip="%s"><rect class="%s" x="%.1f" y="%.1f" '
            'width="%.1f" height="%d" rx="3"/>%s</g>' % (tip, cls, x0 + 1, y, max(w - 2, 1), LANE_H, inner))

def fmt(f):
    return "%d" % round(f) if f >= 100 else "%.0f" % f

lanes = [
    ("Pulse 1 &amp; 2", "loop in V3", [
        (FMIN, V["V3"]["ceil8"], "s-dac", "Volume DAC", "true 12.5 / 25 / 75% duty"),
        (V["V3"]["ceil8"], FMAX, "s-hw", "Hardware tone", "pitch exact, duty falls back to 50%"),
    ]),
    ("Pulse 1 &amp; 2", "loop in V2", [
        (FMIN, V["V2"]["ceil8"], "s-dac", "Volume DAC", "faster loop, higher ceiling"),
        (V["V2"]["ceil8"], FMAX, "s-hw", "Hardware tone", "pitch exact, duty falls back to 50%"),
    ]),
    ("Triangle", "loop in V3", [
        (FMIN, PSG_FLOOR, "s-dac s-only", "DAC only", "the tone generator physically cannot reach here"),
        (PSG_FLOOR, V["V3"]["ceil32"], "s-dac", "Volume DAC", "32-step NES staircase"),
        (V["V3"]["ceil32"], FMAX, "s-hw", "Hardware tone", "staircase runs out of steps"),
    ]),
]

f1 = ['<defs><pattern id="hatch" width="7" height="7" patternUnits="userSpaceOnUse" '
      'patternTransform="rotate(45)"><rect width="7" height="7" fill="none"/>'
      '<line x1="0" y1="0" x2="0" y2="7" stroke="#fff" stroke-opacity=".38" stroke-width="2.5"/>'
      '</pattern></defs>']
f1.append('<rect class="plotbg" x="%d" y="0" width="%.1f" height="%.1f" rx="4"/>'
          % (PAD_L, PLOT, len(lanes) * (LANE_H + LANE_GAP) - LANE_GAP))
for f, nm in zip(OCTAVES, OCTNAME):
    f1.append('<line class="grid" x1="%.1f" y1="-6" x2="%.1f" y2="%.1f"/>'
              % (X(f), X(f), len(lanes) * (LANE_H + LANE_GAP) - LANE_GAP))
y = 0
for title, sub, bands in lanes:
    f1.append('<text class="lanename" x="%d" y="%.1f">%s</text>' % (PAD_L - 14, y + LANE_H / 2 - 2, title))
    f1.append('<text class="lanesub" x="%d" y="%.1f">%s</text>' % (PAD_L - 14, y + LANE_H / 2 + 13, sub))
    for f0, f1_, cls, lab, tip in bands:
        f1.append(band(f0, f1_, y, cls, lab, tip))
        if "s-only" in cls:
            f1.append('<rect class="hatch" x="%.1f" y="%.1f" width="%.1f" height="%d" rx="3"/>'
                      % (X(f0) + 1, y, max(X(f1_) - X(f0) - 2, 1), LANE_H))
    y += LANE_H + LANE_GAP
H1 = y - LANE_GAP

# crossover markers
marks = [
    (PSG_FLOOR, "PSG tone floor", "%d Hz" % round(PSG_FLOOR)),
    (V["V3"]["ceil32"], "triangle ceiling", "%d Hz" % round(V["V3"]["ceil32"])),
    (V["V3"]["ceil8"], "V3 ceiling", "%d Hz" % round(V["V3"]["ceil8"])),
    (V["V2"]["ceil8"], "V2 ceiling", "%d Hz" % round(V["V2"]["ceil8"])),
]
for f, lab, val in marks:
    f1.append('<line class="cross" x1="%.1f" y1="-10" x2="%.1f" y2="%.1f"/>' % (X(f), X(f), H1 + 8))

# observed range brackets
def bracket(f0, f1_, med, y, name):
    out = ['<line class="obs" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>' % (X(f0), y, X(f1_), y),
           '<line class="obs" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>' % (X(f0), y - 4, X(f0), y + 4),
           '<line class="obs" x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>' % (X(f1_), y - 4, X(f1_), y + 4),
           '<circle class="obsdot" cx="%.1f" cy="%.1f" r="3.5"/>' % (X(med), y),
           '<text class="obslabel end" x="%.1f" y="%.1f">%s, this capture</text>' % (X(f0) - 10, y + 4, name),
           '<text class="obslabel" x="%.1f" y="%.1f">%d&ndash;%d Hz</text>'
           % (X(f1_) + 9, y + 4, round(f0), round(f1_))]
    return "".join(out)

OBS_Y = H1 + 30
f1.append(bracket(pulse[0], pulse[-1], q(pulse, .5), OBS_Y, "pulse"))
f1.append(bracket(tri[0], tri[-1], q(tri, .5), OBS_Y + 26, "triangle"))

AX_Y = OBS_Y + 56
for f, nm in zip(OCTAVES, OCTNAME):
    f1.append('<text class="tick" x="%.1f" y="%.1f">%s</text>' % (X(f), AX_Y, "%d" % f))
    f1.append('<text class="ticknote" x="%.1f" y="%.1f">%s</text>' % (X(f), AX_Y + 14, nm))
for f, lab, val in marks:
    f1.append('<text class="crosslab" x="%.1f" y="%.1f">%s</text>' % (X(f), -22, val))
    f1.append('<text class="crosslab crosslab2" x="%.1f" y="%.1f">%s</text>' % (X(f), -10, lab))

FIG1 = ('<svg viewBox="-4 -46 %d %d" role="img" aria-label="Pitch map: which technique '
        'covers which frequency range for each NES voice">%s</svg>'
        % (W + 8, AX_Y + 70, "".join(f1)))

# =============================================================================
# figure 2 -- loop budget
# =============================================================================
BW, BPAD_L, BPAD_R = 1000, 148, 210
BPLOT = BW - BPAD_L - BPAD_R
MAXC = 260
BAR_H, BAR_GAP = 40, 22
PARTLABEL = {"pulse": "pulse voice", "wave": "wave voice", "pcm": "PCM voice", "overhead": "loop overhead"}
PARTCLS = {"pulse": "s-dac", "wave": "s-dac s-wave", "pcm": "s-pcm", "overhead": "s-over"}

f2, y = [], 0
for name, desc, parts in VARIANTS:
    d = V[name]
    f2.append('<text class="lanename" x="%d" y="%.1f">%s</text>' % (BPAD_L - 14, y + BAR_H / 2 - 2, name))
    f2.append('<text class="lanesub" x="%d" y="%.1f">%s</text>' % (BPAD_L - 14, y + BAR_H / 2 + 13, desc))
    x = BPAD_L
    for p in parts:
        w = VOICE_CYCLES[p] / MAXC * BPLOT
        tip = "%s &middot; %d Z80 cycles per sample" % (PARTLABEL[p], VOICE_CYCLES[p])
        f2.append('<g class="band" data-tip="%s"><rect class="%s" x="%.1f" y="%.1f" width="%.1f" '
                  'height="%d" rx="3"/><text class="bandlabel" x="%.1f" y="%.1f">%d</text></g>'
                  % (tip, PARTCLS[p], x + 1, y, max(w - 2, 1), BAR_H, x + w / 2, y + BAR_H / 2 + 4, VOICE_CYCLES[p]))
        x += w
    f2.append('<text class="barout" x="%.1f" y="%.1f">%d cyc</text>' % (x + 14, y + BAR_H / 2 - 2, d["cycles"]))
    f2.append('<text class="barout barout2" x="%.1f" y="%.1f">%s Hz &rarr; %d Hz ceiling</text>'
              % (x + 14, y + BAR_H / 2 + 13, "{:,}".format(int(round(d["rate"]))), round(d["ceil8"])))
    y += BAR_H + BAR_GAP
H2 = y - BAR_GAP
FIG2 = ('<svg viewBox="-4 -14 %d %d" role="img" aria-label="Z80 loop budget: cycles per '
        'voice, and the sample rate and pitch ceiling each variant buys">%s</svg>'
        % (BW + 8, H2 + 30, "".join(f2)))

# =============================================================================
# figure 3 -- noise coverage
# =============================================================================
NW, NPAD_L, NPAD_T = 1000, 148, 0
CELL_W = (NW - NPAD_L - 26) / 16.0
USE_H = 96
f3 = []
maxuse = max(r["use"] for r in noise_rows)
for r in noise_rows:
    x = NPAD_L + r["idx"] * CELL_W
    h = (r["use"] / maxuse) * USE_H if maxuse else 0
    cls = "s-noise" if r["needs_ch2"] else "s-hw"
    if not r["reach"]: cls = "s-none"
    tip = ("period %d &middot; NES shift %s Hz &middot; %s" % (
        r["idx"], "{:,}".format(int(round(r["shift"]))),
        "out of reach" if not r["reach"] else
        ("fixed rate within %.0f%%" % abs(r["fixed_err"]) if not r["needs_ch2"]
         else "needs channel 2 (fixed rate is %+.0f%% out)" % r["fixed_err"])))
    if r["use"] > 0: tip += " &middot; %.1f%% of noise frames" % r["use"]
    f3.append('<g class="band" data-tip="%s">' % tip)
    f3.append('<rect class="cellbg" x="%.1f" y="%.1f" width="%.1f" height="%d" rx="3"/>'
              % (x + 2, USE_H - h, CELL_W - 4, max(h, 2)))
    f3.append('<rect class="%s" x="%.1f" y="%.1f" width="%.1f" height="18" rx="3"/>'
              % (cls, x + 2, USE_H + 10, CELL_W - 4))
    f3.append('<text class="cellnum" x="%.1f" y="%.1f">%d</text>' % (x + CELL_W / 2, USE_H + 44, r["idx"]))
    if r["use"] >= 1:
        f3.append('<text class="cellpct" x="%.1f" y="%.1f">%.0f%%</text>'
                  % (x + CELL_W / 2, USE_H - h - 7, r["use"]))
    f3.append("</g>")
f3.append('<text class="lanename" x="%d" y="%d">usage</text>' % (NPAD_L - 14, USE_H - 20))
f3.append('<text class="lanesub" x="%d" y="%d">this capture</text>' % (NPAD_L - 14, USE_H - 6))
f3.append('<text class="lanename" x="%d" y="%d">reach</text>' % (NPAD_L - 14, USE_H + 23))
f3.append('<text class="lanesub" x="%d" y="%d">NES noise period</text>' % (NPAD_L - 14, USE_H + 44))
FIG3 = ('<svg viewBox="-4 -20 %d %d" role="img" aria-label="Noise coverage: which of the 16 '
        'NES noise periods the PSG fixed rates reach, and how often each is used">%s</svg>'
        % (NW + 8, USE_H + 76, "".join(f3)))

CTX = dict(FIG1=FIG1, FIG2=FIG2, FIG3=FIG3, V=V, PSG_FLOOR=PSG_FLOOR,
           NES_TRI_FLOOR=NES_TRI_FLOOR, DUTY_NON50=DUTY_NON50,
           PULSE_UNDER_V3=PULSE_UNDER_V3, NEEDS_CH2_SHARE=NEEDS_CH2_SHARE,
           REACHABLE=REACHABLE, noise_rows=noise_rows, pulse=pulse, tri=tri,
           NOISE_TOTAL=NOISE_TOTAL)

# =============================================================================
# page
# =============================================================================
def n(x, d=0):
    return ("{:,.%df}" % d).format(x)

noise_table = "".join(
    "<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
        r["idx"], n(r["shift"]),
        "&mdash;" if not r["reach"] else str(r["ch2"]),
        ("out of reach" if not r["reach"] else
         ("fixed rate, %+.0f%%" % r["fixed_err"] if not r["needs_ch2"]
          else "channel 2, %+.1f%%" % r["ch2err"])),
        "%.1f%%" % r["use"] if r["use"] >= 0.05 else "&mdash;")
    for r in noise_rows)

loop_table = "".join(
    "<tr><td>%s</td><td>%s</td><td>%d</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
        k, V[k]["desc"], V[k]["cycles"], n(V[k]["rate"]), n(V[k]["ceil8"]), n(V[k]["ceil32"]))
    for k, _, _ in VARIANTS)

HTML = """<title>APU2PSG Technique Map</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@500;600;700&family=JetBrains+Mono:wght@400;500;700&family=Source+Serif+4:ital,opsz,wght@0,8..60,400;0,8..60,600;1,8..60,400&display=swap">
<style>
:root {
  color-scheme: light;
  --ground:    #E9ECF1;
  --surface:   #FBFCFD;
  --surface-2: #F1F4F8;
  --ink:       #121A23;
  --ink-2:     #47535F;
  --ink-3:     #6E7A88;
  --rule:      #D3DAE3;
  --rule-2:    #E2E7ED;

  --hw:    #2C6FB5;   /* hardware tone generator */
  --dac:   #C8322F;   /* PSG volume DAC */
  --noise: #A08D00;   /* tone-clocked noise */
  --pcm:   #14876A;   /* YM2612 DAC */
  --none:  #9AA5B1;   /* out of reach */
  --over:  #6B7684;   /* loop overhead - carries white numerals */

  --shadow: 0 1px 2px rgba(18,26,35,.06), 0 8px 24px -12px rgba(18,26,35,.18);

  --step--1: 0.8125rem;
  --step-0:  1.0rem;
  --step-1:  1.1875rem;
  --step-2:  1.5rem;
  --step-3:  2rem;
  --step-4:  2.875rem;

  --display: "Archivo", "Helvetica Neue", Arial, sans-serif;
  --body: "Source Serif 4", Georgia, "Times New Roman", serif;
  --mono: "JetBrains Mono", ui-monospace, "SF Mono", Menlo, Consolas, monospace;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --ground:    #0D1218;
    --surface:   #151C24;
    --surface-2: #1B242E;
    --ink:       #E4E9EF;
    --ink-2:     #A6B2C0;
    --ink-3:     #7C8896;
    --rule:      #27313D;
    --rule-2:    #1F2833;
    --hw:    #4C8BD9;
    --dac:   #D13E38;
    --noise: #AE9000;
    --pcm:   #269C79;
    --none:  #55616E;
    --over:  #4E5A67;
    --shadow: 0 1px 2px rgba(0,0,0,.5), 0 10px 28px -14px rgba(0,0,0,.7);
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --ground:    #0D1218;
  --surface:   #151C24;
  --surface-2: #1B242E;
  --ink:       #E4E9EF;
  --ink-2:     #A6B2C0;
  --ink-3:     #7C8896;
  --rule:      #27313D;
  --rule-2:    #1F2833;
  --hw:    #4C8BD9;
  --dac:   #D13E38;
  --noise: #AE9000;
  --pcm:   #269C79;
  --none:  #55616E;
  --over:  #4E5A67;
  --shadow: 0 1px 2px rgba(0,0,0,.5), 0 10px 28px -14px rgba(0,0,0,.7);
}

* { box-sizing: border-box; }
body {
  margin: 0;
  background: var(--ground);
  color: var(--ink);
  font-family: var(--body);
  font-size: var(--step-0);
  line-height: 1.6;
  -webkit-font-smoothing: antialiased;
}
.wrap { max-width: 1120px; margin: 0 auto; padding: clamp(28px, 5vw, 72px) clamp(18px, 4vw, 40px) 96px; }
.prose { max-width: 62ch; }
h1, h2, h3, .lab, .stat-v, .barout, th { font-family: var(--display); }
h1 {
  font-size: var(--step-4); font-weight: 700; line-height: 1.02;
  letter-spacing: -0.028em; margin: 0 0 14px; text-wrap: balance;
}
h2 {
  font-size: var(--step-2); font-weight: 700; letter-spacing: -0.018em;
  line-height: 1.15; margin: 0 0 10px; text-wrap: balance;
}
h3 { font-size: var(--step-0); font-weight: 700; letter-spacing: -0.01em; margin: 0 0 4px; }
p { margin: 0 0 1em; }
p:last-child { margin-bottom: 0; }
a { color: var(--dac); text-underline-offset: 2px; }
:focus-visible { outline: 2px solid var(--dac); outline-offset: 3px; border-radius: 3px; }
code, .m { font-family: var(--mono); font-size: 0.86em; }

.lab {
  font-family: var(--mono); font-size: 0.6875rem; font-weight: 700;
  letter-spacing: 0.14em; text-transform: uppercase; color: var(--ink-3);
}

/* ---------- masthead ---------- */
header.mast { border-bottom: 1px solid var(--rule); padding-bottom: 30px; margin-bottom: 34px; }
.mast .lab { display: block; margin-bottom: 20px; }
.lede { font-size: var(--step-1); line-height: 1.5; color: var(--ink-2); max-width: 58ch; }
.lede strong { color: var(--ink); font-weight: 600; }

/* ---------- stats ---------- */
.stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 1px;
         background: var(--rule); border: 1px solid var(--rule); border-radius: 8px;
         overflow: hidden; margin: 34px 0 52px; }
.stat { background: var(--surface); padding: 18px 20px 16px; display: flex; flex-direction: column; gap: 2px; }
.stat-v { font-size: var(--step-3); font-weight: 700; letter-spacing: -0.03em; line-height: 1;
          font-variant-numeric: tabular-nums; }
.stat-k { font-size: var(--step--1); color: var(--ink-2); line-height: 1.4; }
.stat.a .stat-v { color: var(--dac); }
.stat.b .stat-v { color: var(--noise); }
.stat.c .stat-v { color: var(--hw); }
.stat.d .stat-v { color: var(--pcm); }

/* ---------- figures ---------- */
figure { margin: 0 0 52px; }
.fighead { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px 18px; margin-bottom: 4px; }
.figbody { background: var(--surface); border: 1px solid var(--rule); border-radius: 10px;
           padding: 30px 26px 22px; box-shadow: var(--shadow); overflow-x: auto; margin-top: 16px; }
.figbody > svg { display: block; width: 100%; min-width: 720px; height: auto; }
figcaption { font-size: var(--step--1); color: var(--ink-2); max-width: 66ch; margin-top: 14px; line-height: 1.55; }

.legend { display: flex; flex-wrap: wrap; gap: 6px 20px; margin-top: 18px; }
.lg { display: inline-flex; align-items: center; gap: 8px; font-size: var(--step--1); color: var(--ink-2); }
.sw { width: 22px; height: 11px; border-radius: 2px; flex: none; }
.sw.hw { background: var(--hw); } .sw.dac { background: var(--dac); }
.sw.noise { background: var(--noise); } .sw.pcm { background: var(--pcm); }
.sw.none { background: var(--none); }
.sw.over { background: var(--over); }

/* ---------- svg ---------- */
.plotbg { fill: var(--surface-2); }
.grid { stroke: var(--rule); stroke-width: 1; }
.cross { stroke: var(--ink-3); stroke-width: 1; stroke-dasharray: 2 4; }
.s-hw   { fill: var(--hw); }
.s-dac  { fill: var(--dac); }
.s-wave { fill: var(--dac); opacity: .82; }
.s-pcm  { fill: var(--pcm); }
.s-noise{ fill: var(--noise); }
.s-over { fill: var(--over); }
.s-none { fill: var(--none); }
.s-only { }
.hatch { fill: url(#hatch); pointer-events: none; }
.cellbg { fill: var(--ink-3); opacity: .38; }
.band { cursor: default; }
.band:hover rect { filter: brightness(1.12); }
text { font-family: var(--mono); }
.bandlabel { fill: #fff; font-size: 12px; font-weight: 500; }
.lanename { fill: var(--ink); font-size: 12.5px; font-weight: 700; text-anchor: end; font-family: var(--display); }
.lanesub  { fill: var(--ink-3); font-size: 11px; text-anchor: end; }
.tick { fill: var(--ink-2); font-size: 11px; text-anchor: middle; font-variant-numeric: tabular-nums; }
.ticknote { fill: var(--ink-3); font-size: 9.5px; text-anchor: middle; letter-spacing: .06em; }
.crosslab { fill: var(--ink); font-size: 11px; font-weight: 700; text-anchor: middle; font-variant-numeric: tabular-nums; }
.crosslab2 { fill: var(--ink-3); font-size: 9.5px; font-weight: 400; letter-spacing: .04em; }
.obs { stroke: var(--ink-2); stroke-width: 1.5; }
.obsdot { fill: var(--ink); }
.obslabel { fill: var(--ink-2); font-size: 10.5px; }
.obslabel.end { text-anchor: end; }
.barout { fill: var(--ink); font-size: 13px; font-weight: 700; font-variant-numeric: tabular-nums; }
.barout2 { fill: var(--ink-3); font-size: 11px; font-weight: 400; font-family: var(--mono); }
.cellnum { fill: var(--ink-2); font-size: 11px; text-anchor: middle; font-variant-numeric: tabular-nums; }
.cellpct { fill: var(--ink); font-size: 10px; text-anchor: middle; font-weight: 700; }

/* ---------- cards ---------- */
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(248px, 1fr)); gap: 16px; margin: 26px 0 0; }
.card { background: var(--surface); border: 1px solid var(--rule); border-radius: 10px;
        padding: 18px 20px 20px; border-top: 3px solid var(--tone, var(--rule)); }
.card.hw { --tone: var(--hw); } .card.dac { --tone: var(--dac); }
.card.noise { --tone: var(--noise); } .card.pcm { --tone: var(--pcm); }
.card .lab { display: block; margin-bottom: 10px; }
.card p { font-size: var(--step--1); color: var(--ink-2); line-height: 1.55; margin-bottom: .7em; }
.cost { font-family: var(--mono); font-size: 0.6875rem; color: var(--ink-3);
        border-top: 1px solid var(--rule-2); padding-top: 10px; margin-top: 2px; }
.cost b { color: var(--ink-2); font-weight: 500; }

/* ---------- tables ---------- */
details { margin-top: 16px; border-top: 1px solid var(--rule-2); padding-top: 12px; }
summary { font-family: var(--mono); font-size: 0.6875rem; letter-spacing: .12em;
          text-transform: uppercase; color: var(--ink-3); cursor: pointer; }
summary:hover { color: var(--ink); }
.tbl { overflow-x: auto; margin-top: 12px; }
table { border-collapse: collapse; font-family: var(--mono); font-size: var(--step--1);
        font-variant-numeric: tabular-nums; width: 100%; }
th, td { text-align: right; padding: 5px 12px; border-bottom: 1px solid var(--rule-2); white-space: nowrap; }
th:first-child, td:first-child, th:nth-child(2), td:nth-child(2) { text-align: left; }
th { font-size: 0.6875rem; letter-spacing: .08em; text-transform: uppercase; color: var(--ink-3); font-weight: 600; }
td { color: var(--ink-2); }

/* ---------- section rhythm ---------- */
section { margin-bottom: 56px; }
.sechead { margin-bottom: 18px; }
.sechead .lab { display: block; margin-bottom: 8px; }
.note { border-left: 2px solid var(--rule); padding-left: 18px; color: var(--ink-2);
        font-size: var(--step--1); max-width: 62ch; }
footer { border-top: 1px solid var(--rule); padding-top: 22px; color: var(--ink-2); font-size: var(--step--1); line-height: 1.6; max-width: 78ch; }

/* ---------- tooltip ---------- */
#tip { position: fixed; z-index: 20; pointer-events: none; opacity: 0;
       transition: opacity .1s linear; background: var(--ink); color: var(--ground);
       font-family: var(--mono); font-size: 11px; line-height: 1.45; padding: 6px 9px;
       border-radius: 5px; max-width: 280px; box-shadow: var(--shadow); }
#tip.on { opacity: 1; }
@media (prefers-reduced-motion: reduce) { * { transition: none !important; animation: none !important; } }
@media (max-width: 640px) { :root { --step-4: 2.1rem; --step-3: 1.6rem; --step-2: 1.3rem; } }
</style>

<div class="wrap">

<header class="mast">
  <span class="lab">GeNESis-APU2PSG &middot; voice allocation</span>
  <h1>Four techniques, one PSG</h1>
  <p class="lede">The NES has two pulses with four duties, a triangle, sixteen noise periods and DPCM.
  The Genesis PSG has three squares and a noise generator. <strong>No single trick covers that gap</strong> &mdash;
  so here is which one wins where, and exactly what each costs.</p>
</header>

<div class="stats">
  <div class="stat a"><span class="stat-v">{DUTY_NON50}%</span>
    <span class="stat-k">of pulse frames use a duty the PSG hardware cannot make</span></div>
  <div class="stat c"><span class="stat-v">{PULSE_UNDER_V3}%</span>
    <span class="stat-k">sit below the volume DAC's pitch ceiling in V3</span></div>
  <div class="stat b"><span class="stat-v">{NEEDS_CH2}%</span>
    <span class="stat-k">of noise frames need a period the three fixed rates miss</span></div>
  <div class="stat d"><span class="stat-v">{REACHABLE}/16</span>
    <span class="stat-k">noise periods reachable once channel 2 clocks the shift register</span></div>
</div>

<section>
  <div class="sechead">
    <span class="lab">Figure 1 &middot; the crossover</span>
    <h2>Where each technique wins</h2>
  </div>
  <div class="prose">
    <p>The volume DAC is the main solution: park a channel's period at 0 so it outputs DC, then rewrite
    its logarithmic attenuator from a free-running Z80 loop. That buys real 12.5&thinsp;/&thinsp;25&thinsp;/&thinsp;75% duty and a
    real triangle staircase. What it cannot buy is pitch &mdash; a 12.5% pulse needs eight samples per
    period to exist at all, so the loop's sample rate divided by eight <em>is</em> the ceiling. Above it,
    the voice goes back to the hardware tone generator, where the pitch stays exact and only the duty
    degrades to 50%.</p>
  </div>
  <figure>
    <div class="legend">
      <span class="lg"><span class="sw dac"></span>Volume DAC &mdash; any duty, pitch-limited</span>
      <span class="lg"><span class="sw hw"></span>Hardware tone &mdash; any pitch, 50% only</span>
    </div>
    <div class="figbody">{FIG1}</div>
    <figcaption>Log pitch, labelled in octaves of A. The dashed rules are the four hard edges: the PSG
    tone generator's 10-bit period floor at {PSG_FLOOR}&nbsp;Hz, the triangle staircase running out of steps at
    {TRI_CEIL}&nbsp;Hz, and the 12.5%-duty ceilings of the two loop variants. Brackets show the range actually
    used by the capture in this repo, dot at the median.</figcaption>
  </figure>
  <p class="note">The triangle is the one place where the volume DAC is not an upgrade but the only
  option. NES triangles reach down to {NES_TRI_FLOOR}&nbsp;Hz; the PSG's tone generator stops at {PSG_FLOOR}&nbsp;Hz and
  does not go flat below it &mdash; it simply cannot go there.</p>
</section>

<section>
  <div class="sechead">
    <span class="lab">Figure 2 &middot; the budget</span>
    <h2>Why the ceiling sits where it does</h2>
  </div>
  <div class="prose">
    <p>Sample rate is loop length. That single fact is why &ldquo;more voices&rdquo; and &ldquo;more pitch&rdquo;
    are the same problem seen from two sides: every voice added to the loop lengthens it, which lowers the
    rate, which lowers the ceiling for <em>every</em> voice at once. So the driver ships three loop
    variants and the ROM picks one per frame.</p>
  </div>
  <figure>
    <div class="legend">
      <span class="lg"><span class="sw dac"></span>DAC voice</span>
      <span class="lg"><span class="sw pcm"></span>PCM voice</span>
      <span class="lg"><span class="sw over"></span>loop overhead</span>
    </div>
    <div class="figbody">{FIG2}</div>
    <figcaption>Z80 cycles per sample at 3.579545&nbsp;MHz. A pulse voice is 63 cycles because the
    attenuator is logarithmic, which makes volume scaling an <span class="m">add</span> and lets a whole
    pulse fit in eight instructions with no wavetable. The triangle needs a real 32-step table, so it
    costs 89.</figcaption>
    <details>
      <summary>Table view</summary>
      <div class="tbl"><table>
        <thead><tr><th>Variant</th><th>Voices</th><th>Cycles</th><th>Sample rate</th>
        <th>12.5% ceiling</th><th>Triangle ceiling</th></tr></thead>
        <tbody>{LOOP_TABLE}</tbody>
      </table></div>
    </details>
  </figure>
</section>

<section>
  <div class="sechead">
    <span class="lab">Figure 3 &middot; the trade</span>
    <h2>Noise costs the triangle</h2>
  </div>
  <div class="prose">
    <p>The PSG's noise generator has three fixed rates. The NES has sixteen periods in two modes.
    Setting the PSG's rate selector to 3 clocks the shift register from <em>tone channel 2</em> instead,
    and channel 2's period is 10 bits &mdash; which brings {REACHABLE} of the 16 within about 1%. Only the fastest
    is out of range: it wants a 447&nbsp;kHz shift rate and the chip tops out at half that.</p>
    <p>The price is channel 2, which is where the triangle lives. That is a real choice, not a free win,
    so it sits on the START button rather than being always on.</p>
  </div>
  <figure>
    <div class="legend">
      <span class="lg"><span class="sw hw"></span>a fixed rate is close enough</span>
      <span class="lg"><span class="sw noise"></span>needs channel 2 as the clock</span>
      <span class="lg"><span class="sw none"></span>out of reach entirely</span>
    </div>
    <div class="figbody">{FIG3}</div>
    <figcaption>Bars above the strip are each period's share of noise-active frames in this repo's
    capture. Only period&nbsp;6 sees real use among the ones a fixed rate covers &mdash; drums almost never
    land on one of the three.</figcaption>
    <details>
      <summary>Table view</summary>
      <div class="tbl"><table>
        <thead><tr><th>Period</th><th>NES shift rate</th><th>Ch2 period</th><th>Mapping</th>
        <th>Usage</th></tr></thead>
        <tbody>{NOISE_TABLE}</tbody>
      </table></div>
    </details>
  </figure>
</section>

<section>
  <div class="sechead">
    <span class="lab">The four</span>
    <h2>What each one costs</h2>
  </div>
  <div class="cards">
    <div class="card hw">
      <span class="lab">Hardware tone</span>
      <h3>The PSG's own square</h3>
      <p>Pitch-exact anywhere above its floor, and free. It is 50% duty and only 50% duty, and its
      10-bit period cannot reach below {PSG_FLOOR}&nbsp;Hz.</p>
      <p class="cost"><b>costs</b> nothing &middot; <b>gives</b> pitch, never timbre</p>
    </div>
    <div class="card dac">
      <span class="lab">Volume DAC</span>
      <h3>The attenuator as a waveform</h3>
      <p>Any duty, a real triangle staircase, and pitch below the PSG's own floor. Bounded above by
      sample rate over steps, and by 2&nbsp;dB of amplitude resolution &mdash; the staircase peaks quantise
      coarsely.</p>
      <p class="cost"><b>costs</b> 63&ndash;89 Z80 cycles a voice &middot; <b>gives</b> timbre, and the bottom octave</p>
    </div>
    <div class="card noise">
      <span class="lab">Tone-clocked noise</span>
      <h3>Channel 2 as a shift clock</h3>
      <p>Turns 3 reachable noise periods into {REACHABLE}, in both long and short mode. Short mode needs its own
      table: the NES sequence is 93 steps to the PSG's 15, so it matches pitch rather than clock.</p>
      <p class="cost"><b>costs</b> the triangle &middot; <b>gives</b> the drum kit</p>
    </div>
    <div class="card pcm">
      <span class="lab">YM2612 DAC</span>
      <h3>The one job for FM</h3>
      <p>DPCM does not fit on the volume DAC &mdash; it would eat the whole loop for four logarithmic bits.
      Channel 6's 8-bit linear DAC costs one store per sample once register 2Ah is latched.</p>
      <p class="cost"><b>costs</b> 31 cycles &middot; <b>gives</b> the sampled channel</p>
    </div>
  </div>
</section>

<footer>
  <p>Cycle counts come from the assembler; the percentages come from the 8,697-frame capture checked
  into the repository. The Z80 loop is verified by executing it instruction by instruction and checking
  the bytes it emits &mdash; <strong>none of this has run on real hardware yet</strong>. The two things
  most likely to move are the per-frame bus stall and V2D's true rate, whose PCM read picks up bank-window
  wait states the cycle count does not model.</p>
</footer>

</div>

<div id="tip" role="status" aria-live="polite"></div>
<script>
(function () {
  var tip = document.getElementById("tip");
  function show(e, t) {
    tip.innerHTML = t;
    tip.classList.add("on");
    move(e);
  }
  function move(e) {
    var r = tip.getBoundingClientRect();
    var x = e.clientX + 14, y = e.clientY + 16;
    if (x + r.width > window.innerWidth - 8) x = e.clientX - r.width - 14;
    if (y + r.height > window.innerHeight - 8) y = e.clientY - r.height - 14;
    tip.style.left = x + "px";
    tip.style.top = y + "px";
  }
  document.addEventListener("mouseover", function (e) {
    var g = e.target.closest ? e.target.closest(".band[data-tip]") : null;
    if (g) show(e, g.getAttribute("data-tip"));
  });
  document.addEventListener("mousemove", function (e) {
    if (tip.classList.contains("on")) move(e);
  });
  document.addEventListener("mouseout", function (e) {
    if (e.target.closest && e.target.closest(".band[data-tip]")) tip.classList.remove("on");
  });
})();
</script>
"""

out = (HTML
       .replace("{FIG1}", FIG1).replace("{FIG2}", FIG2).replace("{FIG3}", FIG3)
       .replace("{DUTY_NON50}", "%.0f" % DUTY_NON50)
       .replace("{PULSE_UNDER_V3}", "%.0f" % PULSE_UNDER_V3)
       .replace("{NEEDS_CH2}", "%.1f" % NEEDS_CH2_SHARE)
       .replace("{REACHABLE}", str(REACHABLE))
       .replace("{PSG_FLOOR}", "%d" % round(PSG_FLOOR))
       .replace("{TRI_CEIL}", "%d" % round(V["V3"]["ceil32"]))
       .replace("{NES_TRI_FLOOR}", "%d" % round(NES_TRI_FLOOR))
       .replace("{LOOP_TABLE}", loop_table)
       .replace("{NOISE_TABLE}", noise_table))

import sys
open(sys.argv[1] if len(sys.argv) > 1 else "technique-map.html", "w").write(out)
print("wrote %d bytes" % len(out))
