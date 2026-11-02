#!/usr/bin/env python3
"""Reads the score from audio.cpp and checks it musically: lead notes on
the beats against the chord chart, the duet harmony, range, and the loop seams."""
import re, sys, pathlib
src = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "audio.cpp").read_text()
PC = dict(C=0, D=2, E=4, F=5, G=7, A=9, B=11)
NAMES = "C C# D Eb E F F# G Ab A Bb B".split()
def grab(macro, name):
    m = re.search(r"constexpr auto %s = %s\((?:R\"\()?(.*?)(?:\)\")?\);" % (name, macro), src, re.S)
    return m.group(1).strip('"')
def notes(name):
    out = []
    for t in grab("NOTES", name).replace("|", " ").split():
        if t == "-": out.append("-")
        elif t == ".": out.append(".")
        else:
            pc = PC[t[0]] + (1 if "#" in t else -1 if t[1] == "b" else 0)
            out.append((int(t[-1]) + 1) * 12 + pc)
    return out
def chords(name):
    out = []
    for t in grab("CHORDS", name).replace("|", " ").split():
        root = PC[t[0]] + (1 if t[1:2] == "#" else -1 if t[1:2] == "b" else 0)
        q = t[1:].lstrip("#b")
        tones = {0, 3 if q.startswith("m") else 4, 7} | ({10} if "7" in q else set())
        out.append((t, {(root + x) % 12 for x in tones}))
    return out
def name(n): return f"{NAMES[n % 12]}{n // 12 - 1}"
total_issues = 0
for song in ["Title", "Run", "Dash", "Over"]:
    lead, ch = notes(f"k{song}Lead"), chords(f"k{song}Chords")
    bars = len(lead) // 16
    played = [x for x in lead if isinstance(x, int)]
    issues = []
    cur = None
    for i, x in enumerate(lead):
        if isinstance(x, int): cur = x
        elif x == ".": cur = None
        if i % 4 == 0 and cur is not None:   # what sounds on each beat
            cname, tones = ch[i // 8]
            if cur % 12 not in tones:
                issues.append(f"bar {i//16+1} beat {i%16//4+1}: {name(cur)} over {cname}")
    # seam: last sounding note -> first note
    last = [x for x in lead if isinstance(x, int)][-1]
    first = played[0]
    print(f"{song}: {bars} bars, {len(played)} notes, range {name(min(played))}-{name(max(played))}, "
          f"loop seam {name(last)} -> {name(first)} ({first-last:+d} semitones)")
    for s in issues: print("   non-chord tone on beat:", s)
    total_issues += len(issues)
print("beats checked; non-chord tones on beats:", total_issues)
