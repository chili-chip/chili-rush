#!/usr/bin/env python3
"""Numeric checks on tools/out: step timing and tempo from events.csv, and
loudness envelopes / silence checks from the WAVs."""
import csv, wave, sys, math, collections
import numpy as np
out = sys.argv[1] if len(sys.argv) > 1 else "tools/out"
ev = collections.defaultdict(list)
for r in csv.DictReader(open(f"{out}/events.csv")):
    ev[(r["scene"], int(r["channel"]))].append((int(r["ms"]), int(r["hz"])))
def wav(name):
    w = wave.open(f"{out}/{name}"); return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float)
def rms_db(x): return 20 * math.log10(max(1e-9, math.sqrt(np.mean(x * x))) / 32768)

# drums hit on a 16th grid; tempo = 60000 / (4 * median step)
for scene in ["title", "run", "dash"]:
    t = np.array([e[0] for e in ev[(scene, 6)]])
    d = np.diff(t)
    step = np.min(d)  # closest hits are one 16th apart (with 10 ms jitter)
    print(f"{scene}: {len(t)} drum hits, first at {t[0]} ms, inter-hit min/max {d.min()}/{d.max()} ms")
# tempo over time in RUN from the lead/drum onsets: count drum hits per 5 s window
t = np.array([e[0] for e in ev[("run", 6)]])
print("run drum hits per 5 s:", [int(((t >= a) & (t < a + 5000)).sum()) for a in range(0, 40000, 5000)])
# exact tempo: RUN pattern E has hits at steps 0,2,3,4,6,7,8,10,11,12,14,15 -> use kick+snare bar starts:
# bar starts = crash/kick at step 0 of each bar; estimate bar length from every-16-step lead positions instead:
def fit_step(times):
    d = np.diff(np.unique(times)).astype(float)
    best = min(np.arange(70, 140, 0.1), key=lambda st: np.sum((d / st - np.maximum(1, np.round(d / st))) ** 2))
    return best
for scene in ["title", "run", "dash"]:
    allt = np.concatenate([np.array([e[0] for e in ev[(scene, c)]]) for c in (3, 4, 5, 6)])
    for a in ((0, 5000, 10000, 15000, 20000, 25000, 30000, 35000) if scene == "run" else (0,)):
        sel = allt[(allt >= a) & (allt < a + 5000)]
        st = fit_step(sel)
        print(f"  {scene} {a/1000:4.0f}-{a/1000+5:.0f}s: 16th step {st:5.1f} ms -> {60000/(4*st):5.1f} BPM")
for scene, chn in []:
    tt = np.array([e[0] for e in ev[(scene, chn)]])
    # bass pattern C/D plays on every even step -> 8th notes
    d = np.diff(tt)
    for a in (0, 10000, 19000, 22000, 25000, 30000, 35000, 39000):
        sel = d[(tt[:-1] >= a) & (tt[:-1] < a + 1000)]
        if len(sel): print(f"  run t={a/1000:4.0f}s: bass 8th = {sel.mean():6.1f} ms -> {60000/(2*sel.mean()):5.1f} BPM")

for name in ["title", "run", "dash", "game_over", "sfx", "stress", "run_dash_run"]:
    x = wav(f"{name}.wav")
    sec = [rms_db(x[i:i + 22050]) for i in range(0, len(x) - 22049, 22050)]
    print(f"{name:13s} per-second RMS dBFS:", " ".join(f"{v:5.1f}" for v in sec))

go = wav("game_over.wav")
env = np.array([np.sqrt(np.mean(go[i:i + 220] ** 2)) for i in range(0, len(go), 220)])  # 10 ms
audible = np.where(env > 30)[0]
print("game over: hit at 2.00 s, last audible 10 ms block at %.2f s" % (audible.max() / 100))
st = wav("stress.wav")
print("stress: music off at 6.50 s (GOLDEN sfx plays) -> RMS 6.85-7.5 s: %.1f dBFS" % rms_db(st[int(6.85*22050):int(7.5*22050)]))
print("stress: sound off at 7.50 s (HIT requested) -> RMS 7.5-8.5 s: %.1f dBFS" % rms_db(st[int(7.5*22050):]))
