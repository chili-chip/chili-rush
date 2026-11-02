#!/usr/bin/env python3
"""Chili Rush art generator.

Draws every sprite pixel by pixel (string grids + small procedural helpers)
and writes:
  assets/sprites.png   RGBA sheet, rectangles read from sprites.hpp
  assets/splash.png    128x96 RGB launcher title art
  assets/icon.png      8x8 RGB launcher icon
  assets/preview.png   the sheet at 4x on a checkerboard, for review

Run from anywhere:  python3 tools/make_sprites.py
All art is original. Light comes from the top-left.
"""
import math
import os
import random
import re
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")


def hexc(s):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


# ---------------------------------------------------------------- palette
K = hexc("1a0f1f")  # outline
PAL = {
    "k": K,
    # chili red
    "W": hexc("ffd6c4"), "h": hexc("ff7350"), "R": hexc("e8302a"),
    "r": hexc("a8182a"), "d": hexc("6a0e26"), "m": hexc("5a0a1e"),
    "P": hexc("ff8aa8"),
    # greens
    "l": hexc("b4ea6a"), "G": hexc("4cb83a"), "g": hexc("23703a"),
    "q": hexc("124a2e"),
    # whites / greys
    "w": hexc("fffaf0"), "e": hexc("c9c3d6"), "x": hexc("8a8299"),
    "z": hexc("4e465c"),
    # sneaker teal
    "T": hexc("2ec4d6"), "t": hexc("17809e"),
    # yellows / oranges
    "F": hexc("fff6b0"), "Y": hexc("ffd23c"), "y": hexc("e89a1e"),
    "O": hexc("ff8a1e"), "o": hexc("c45414"),
    # wood
    "n": hexc("f0b070"), "B": hexc("c47a40"), "b": hexc("824628"),
    "j": hexc("4e2818"),
    # sand / sandstone
    "s": hexc("f4c48a"), "a": hexc("dc8a52"), "A": hexc("a8553a"),
    # agave blue-green
    "U": hexc("9ed8c0"), "u": hexc("4fa08e"), "v": hexc("2a6460"),
    # neon / city
    "c": hexc("5ff6ff"), "C": hexc("1fa8d8"), "i": hexc("ff4fd8"),
    "I": hexc("a8209c"), "p": hexc("6a3a9c"), "N": hexc("3a2460"),
    "M": hexc("251838"),
    # basalt / obsidian
    "1": hexc("8a7a9a"), "2": hexc("5a4a6a"), "3": hexc("3a2c48"),
    "4": hexc("241a30"),
    # milk blue
    "L": hexc("8ad0ff"), "D": hexc("2e6ee0"),
    # gold
    "6": hexc("fff4b0"), "7": hexc("ffd84a"), "8": hexc("e8a020"),
    "9": hexc("a86010"),
}


def col(c):
    if c is None:
        return None
    if isinstance(c, tuple):
        return c
    return PAL[c]


# ---------------------------------------------------------------- canvas
class Spr:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[None] * w for _ in range(h)]

    def get(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.p[y][x]
        return None

    def px(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = col(c)

    def grid(self, lines, ox=0, oy=0, skip=".", strict_w=None):
        for j, row in enumerate(lines):
            if strict_w is not None:
                assert len(row) == strict_w, (len(row), row)
            for i, ch in enumerate(row):
                if ch in skip or ch == " ":
                    continue
                if ch == "_":  # explicit erase
                    if 0 <= ox + i < self.w and 0 <= oy + j < self.h:
                        self.p[oy + j][ox + i] = None
                    continue
                self.px(ox + i, oy + j, ch)

    def rect(self, x, y, w, h, c):
        for j in range(y, y + h):
            for i in range(x, x + w):
                self.px(i, j, c)

    def line(self, x0, y0, x1, y1, c):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def ellipse(self, cx, cy, rx, ry, c):
        for y in range(self.h):
            for x in range(self.w):
                if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    self.px(x, y, c)

    def shaded_ellipse(self, cx, cy, rx, ry, ramp, hl=None):
        """ramp: list light->dark; lit from top-left."""
        for y in range(self.h):
            for x in range(self.w):
                nx, ny = (x - cx) / rx, (y - cy) / ry
                d = nx * nx + ny * ny
                if d <= 1.0:
                    t = (nx + ny) * 0.5 + 0.5 * d  # -1 top-left .. 1+ bottom-right
                    t = (t + 0.9) / 2.2
                    k = min(len(ramp) - 1, max(0, int(t * len(ramp))))
                    self.px(x, y, ramp[k])

    def poly(self, pts, c):
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        for y in range(max(0, int(min(ys))), min(self.h, int(max(ys)) + 1)):
            for x in range(max(0, int(min(xs))), min(self.w, int(max(xs)) + 1)):
                if inside(x + 0.5, y + 0.5, pts):
                    self.px(x, y, c)

    def outline(self, c="k", diag=False, ignore=("k",)):
        src = [col(i) for i in ignore]
        out = []
        nb = [(1, 0), (-1, 0), (0, 1), (0, -1)]
        if diag:
            nb += [(1, 1), (-1, -1), (1, -1), (-1, 1)]
        for y in range(self.h):
            for x in range(self.w):
                if self.p[y][x] is not None:
                    continue
                for dx, dy in nb:
                    v = self.get(x + dx, y + dy)
                    if v is not None and v not in src:
                        out.append((x, y))
                        break
        for x, y in out:
            self.p[y][x] = col(c)
        return self

    def over(self, other, ox=0, oy=0):
        for y in range(other.h):
            for x in range(other.w):
                v = other.p[y][x]
                if v is not None:
                    self.px(ox + x, oy + y, v)
        return self

    def flip(self):
        s = Spr(self.w, self.h)
        for y in range(self.h):
            s.p[y] = list(reversed(self.p[y]))
        return s

    def recolor(self, mapping):
        m = {col(a): col(b) for a, b in mapping.items()}
        for y in range(self.h):
            for x in range(self.w):
                v = self.p[y][x]
                if v in m:
                    self.p[y][x] = m[v]
        return self

    def image(self):
        im = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        for y in range(self.h):
            for x in range(self.w):
                v = self.p[y][x]
                if v is not None:
                    im.putpixel((x, y), v + (255,))
        return im


def inside(x, y, pts):
    n = len(pts)
    ins = False
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            ins = not ins
        j = i
    return ins


# ---------------------------------------------------------------- tiny 3x5 font
FONT3 = {
    "A": ["###", "#.#", "###", "#.#", "#.#"],
    "C": ["###", "#..", "#..", "#..", "###"],
    "D": ["##.", "#.#", "#.#", "#.#", "##."],
    "E": ["###", "#..", "##.", "#..", "###"],
    "I": ["#", "#", "#", "#", "#"],
    "N": ["#.#", "###", "###", "#.#", "#.#"],
    "O": ["###", "#.#", "#.#", "#.#", "###"],
    "P": ["###", "#.#", "###", "#..", "#.."],
    "S": ["###", "#..", "###", "..#", "###"],
    "T": ["###", ".#.", ".#.", ".#.", ".#."],
}


def text3(s, txt, x, y, colors, gap=1):
    for n, ch in enumerate(txt):
        g = FONT3[ch]
        c = colors[n % len(colors)]
        for j, row in enumerate(g):
            for i, v in enumerate(row):
                if v == "#":
                    s.px(x + i, y + j, c)
        x += len(g[0]) + gap
    return x


def text3_width(txt, gap=1):
    return sum(len(FONT3[c][0]) for c in txt) + gap * (len(txt) - 1)


# ================================================================ HERO
# The hero is built from parts so every pose can squash and stretch:
# a pepper body with a width profile, a stalk with a flapping leaf, a face,
# and limbs.  Parts sit on separate layers (back arm, legs, body, front arm),
# each with its own outline, so overlapping limbs stay readable.

def rnd_half(v):
    return int(math.floor(v + 0.5))


EYES = {
    "normal": [".ww.", "wwww", "wwkw", "wkkw", ".ww."],
    "up":     [".ww.", "wkkw", "wkkw", "wwww", ".ww."],
    "wide":   [".ww.", "wwww", "wkkw", "wkkw", ".ww."],
    "fierce": ["kkk.", "wwkw", "wkkw", ".ww.", "...."],
    "fierceR": [".kkk", "wwkw", "wkkw", ".ww.", "...."],
    "squint": ["....", ".kk.", "k..k", "....", "...."],
    "x":      ["k...", ".kk.", "k...", "....", "...."],
    "xR":     ["...k", ".kk.", "...k", "....", "...."],
}

MOUTHS = {  # (grid, left column)
    "open": (["kmmmmk", ".kPPk.", "..kk.."], 5),
    "grin": (["k....k", ".kkkk."], 5),
    "o":    ([".kk.", "kmmk", ".kk."], 6),
    "wavy": (["k.k.k.", ".k.k.k"], 5),
    "grit": (["kwwwwk", ".kkkk."], 5),
}

LEAF = {  # rows from T-4 down to T-1, columns 3..7; joins the stalk at (7..8, T-2)
    "up":   ["ll...", ".lGG.", ".....", "....."],
    "mid":  [".....", "ll...", ".lGGG", "....."],
    "down": [".....", ".....", "...GG", ".lGG."],
    "back": [".....", ".....", "llGGG", "....."],
}


def chili_body(s, T, H, W, cx=8.0, curl=3.0):
    """Glossy pepper pod: round shoulders, long taper, tip hooking to the right.
    Returns {row: (left, right)}."""
    rows = {}
    for i in range(H):
        v = i / (H - 1)
        if v < 0.05:
            f = 0.6
        elif v < 0.1:
            f = 0.85
        elif v < 0.16:
            f = 0.96
        elif v <= 0.36:
            f = 1.0
        else:
            f = 1 - 0.96 * ((v - 0.36) / 0.64) ** 1.15
        off = curl * ((v - 0.5) / 0.5) ** 2.2 if v > 0.5 else 0.0
        half = W / 2 * f
        l = rnd_half(cx - half + off)
        r = rnd_half(cx + half + off) - 1
        if r < l:
            r = l
        rows[T + i] = (l, r)
        for x in range(l, r + 1):
            rel = (x - l) / max(1, r - l)
            if v > 0.86:
                c = "r" if (x < r and v < 0.95) else "d"
            elif x == l and v < 0.82:
                c = "h"
            elif rel > 0.72 or (v > 0.6 and rel > 0.55):
                c = "r"
            else:
                c = "R"
            if x == r and 0.15 < v < 0.8 and r - l >= 5:
                c = "d"
            s.px(x, T + i, c)
    # calyx cap and lobes
    l, r = rows[T]
    for x in range(l, r + 1):
        s.px(x, T, "l" if x == l else ("g" if x == r else "G"))
    l, r = rows[T + 1]
    for x in range(l, r + 1):
        if x in (l, l + 1, 7, 8) or x == r:
            s.px(x, T + 1, "g" if x in (l, r) else "G")
    # long gloss streak down the lit side
    for i in range(H):
        v = i / (H - 1)
        l, r = rows[T + i]
        if 0.2 <= v <= 0.62 and r - l >= 3:
            s.px(l + 1, T + i, "W" if v < 0.45 else "h")
    return rows


def stalk(s, T, leaf):
    # thick stalk curling up and right: the hero's quiff
    s.px(7, T - 1, "G")
    s.px(8, T - 1, "g")
    s.px(8, T - 2, "G")
    s.px(9, T - 2, "g")
    s.px(9, T - 3, "l")
    s.px(10, T - 3, "G")
    if leaf:
        s.grid(LEAF[leaf], 3, T - 4)


def face(s, T, H, eyes, mouth, blush=True):
    ey = T + 4
    s.grid(EYES[eyes], 3, ey)
    s.grid(EYES.get(eyes + "R", EYES[eyes]), 9, ey)
    my = ey + (5 if eyes not in ("squint", "x", "fierce") else 4)
    mg, mx = MOUTHS[mouth]
    s.grid(mg, mx, my)
    if blush:
        s.px(3, my, "P")
        s.px(12, my, "P")


def bandana_band(s, rows, T, knot=True):
    """White bandana with red polka dots wrapped round the top of the pod."""
    for y in (T + 2, T + 3):
        l, r = rows[y]
        for x in range(l, r + 1):
            c = "w"
            if x >= r - 1 or (y == T + 3 and x > l + 1 and x >= r - 2):
                c = "e"
            if (x + y * 2) % 3 == 0 and l < x < r:
                c = "R"
            s.px(x, y, c)
    if knot:
        s.rect(7, T + 2, 2, 2, "e")
        s.px(7, T + 2, "w")


def tail(lay, x0, y0, ang, length, bend, thick=2):
    """A bandana tail: a fluttering strip leaving the knot at angle ang
    (degrees, 0 = right, 90 = up), bent sideways by `bend` pixels."""
    a = math.radians(ang)
    dx, dy = math.cos(a), -math.sin(a)
    px_, py_ = -dy, dx
    n = int(length * 3) + 2
    for k in range(n + 1):
        t = k / n
        w = math.sin(t * math.pi * 1.5) * bend
        x = x0 + dx * length * t + px_ * w
        y = y0 + dy * length * t + py_ * w
        c = "R" if t > 0.8 else "w"
        lay.px(int(math.floor(x)), int(math.floor(y)), c)
        if t < 0.3 and thick > 1:
            lay.px(int(math.floor(x)), int(math.floor(y)) + 1, "e")


def tails_for(phase, mode="run"):
    """Two tails; their angles and bends move with phase (radians)."""
    if mode == "run":
        return [(178 + 24 * math.sin(phase), 4.6, 1.3 * math.sin(phase * 2 + 1.0)),
                (2 + 24 * math.sin(phase + 1.9), 4.2, 1.3 * math.sin(phase * 2 + 2.6))]
    if mode == "rise":   # moving up: tails stream down
        return [(215 + 8 * math.sin(phase), 5.0, 1.0), (-35 + 8 * math.sin(phase), 4.5, -1.0)]
    if mode == "fall":   # falling: tails stream straight up
        return [(110, 5.5, 1.2 * math.sin(phase)), (70, 5.0, -1.2 * math.sin(phase))]
    if mode == "float":
        return [(170, 5.5, 1.5), (10, 5.0, -1.5)]
    if mode == "flop":
        return [(200, 4.0, -1.0), (-20, 4.0, 1.0)]
    raise ValueError(mode)


def shoe_back(s, ax, top):
    """Sneaker seen from behind, planted: teal heel, white heel tab, sole edge."""
    for x in (ax - 1, ax, ax + 1):
        s.px(x, top, "T")
        s.px(x, top + 1, "e")
    s.px(ax, top, "w")
    s.px(ax + 1, top, "t")
    s.px(ax + 1, top + 1, "x")


def shoe_sole(s, ax, top):
    """Foot kicked up behind: the bright sole flashes at the camera."""
    s.px(ax, top - 1, "T")
    for x in (ax - 1, ax, ax + 1):
        s.px(x, top, "w")
        s.px(x, top + 1, "w")
    s.px(ax, top + 1, "x")
    s.px(ax + 1, top + 1, "e")


def hand(s, x, y):
    s.rect(x, y, 2, 2, "w")
    s.px(x + 1, y + 1, "e")


def hero_back(T, H, W, legs, hands, phase=0.0, tails="run", leaf=None, curl=3.0,
              dust=None, extra=None):
    """Runner seen from behind.
    legs:  [(ankle x, lift, sole)] left/right; sole=True draws the kicked-up sole
           in front of the pod.
    hands: [(x, y, front)]; a hand swinging back comes towards us (front)."""
    B = T + H - 1
    back, lg, body, front = Spr(16, 24), Spr(16, 24), Spr(16, 24), Spr(16, 24)
    rows = chili_body(body, T, H, W, curl=curl)
    stalk(body, T, leaf)
    bandana_band(body, rows, T)
    for i, (hx, hy, fr) in enumerate(hands):
        l, r = rows[T + 6]
        sx = l + 1 if i == 0 else r - 1
        lay = front if fr else back
        lay.line(sx, T + 6, hx + (1 if i == 0 else 0), hy, "G")
        hand(lay, hx, hy)
    for i, ((ax, lift, sole), hip) in enumerate(zip(legs, (6, 9))):
        top = 21 - lift
        lg.line(hip, B - 2, ax, top - 1, "G")
        if sole:
            sl = Spr(16, 24)
            sl.line(hip, B - 1, ax, top - 1, "G")
            shoe_sole(sl, ax, top)
            front.over(sl)
        else:
            shoe_back(lg, ax, top)
    tl = Spr(16, 24)
    for (ang, ln, bend) in tails_for(phase, tails):
        tail(tl, 7 if math.cos(math.radians(ang)) < 0 else 8, T + 3, ang, ln, bend)
    out = Spr(16, 24)
    if dust:
        d = Spr(16, 24)
        for (x, y, r, c) in dust:
            d.ellipse(x, y, r, r, c)
        d.outline()
        out.over(d)
    for lay in (back, lg, body, tl, front):
        lay.outline()
        out.over(lay)
    if extra:
        extra(out)
    return out


def mirror_spec(spec):
    sp = dict(spec)
    (la, ll, ls), (ra, rl, rs) = spec["legs"]
    sp["legs"] = [(15 - ra, rl, rs), (15 - la, ll, ls)]
    (lx, ly, lf), (rx, ry, rf) = spec["hands"]
    sp["hands"] = [(14 - rx, ry, rf), (14 - lx, ly, lf)]
    return sp


# Half a stride with the left foot planted; the second half mirrors the limbs
# only (the pod, its light and its hooked tip stay put).
RUN_HALF = [
    # contact: left heel strikes, right sole kicked up at us, left arm swung back
    dict(T=4, H=16, W=10, legs=[(6, 0, False), (10, 4, True)],
         hands=[(1, 15, True), (13, 9, False)]),
    # down: weight lands, pod squashes wide and short
    dict(T=6, H=14, W=12, legs=[(6, 0, False), (10, 5, True)],
         hands=[(1, 16, True), (13, 11, False)]),
    # passing: right foot swings through under the body, arms at the hips
    dict(T=4, H=16, W=10, legs=[(6, 0, False), (9, 2, False)],
         hands=[(2, 14, False), (12, 14, False)]),
    # up: push off, pod stretches tall and thin, arms start to swap
    dict(T=3, H=17, W=8, legs=[(6, 0, False), (10, 1, False)],
         hands=[(3, 10, False), (11, 16, True)]),
]


def run_specs():
    specs = RUN_HALF + [mirror_spec(s) for s in RUN_HALF]
    out = []
    for i, s in enumerate(specs):
        s = dict(s)
        s["phase"] = i / 8 * 2 * math.pi
        out.append(s)
    return out


def run_frames():
    return [hero_back(**s) for s in run_specs()]


def jump_up_frame():
    return hero_back(T=3, H=17, W=8, legs=[(6, 0, False), (9, 0, False)],
                     hands=[(1, 3, False), (13, 3, False)], tails="rise", phase=0.5,
                     dust=[(2.5, 21.5, 1.6, "e"), (13, 21.5, 1.6, "e")])


def jump_frame():
    # apex: knees tucked, both soles showing, arms out, tails floating
    return hero_back(T=5, H=15, W=10, legs=[(4, 0, True), (11, 0, True)],
                     hands=[(0, 5, False), (14, 5, False)], tails="float")


def jump_down_frame():
    return hero_back(T=4, H=16, W=8, legs=[(6, 0, False), (9, 0, False)],
                     hands=[(0, 4, False), (14, 6, False)], tails="fall", phase=1.0)


def land_frame():
    return hero_back(T=9, H=12, W=12, legs=[(3, 0, False), (12, 0, False)],
                     hands=[(0, 15, True), (14, 15, True)], tails="flop", curl=1.5,
                     dust=[(1.5, 22, 1.5, "e"), (14, 22, 1.5, "e")])


def slide_frame(alt=0):
    """Seen from behind, lying low on his side and sliding feet first into the
    screen: the pod lies across the lane, bandana tails streaming back at us."""
    pod = Spr(24, 16)
    rows = chili_body(pod, 2, 14, 8, cx=6.0, curl=-2.0)
    bandana_band(pod, rows, 2, knot=False)
    body = Spr(16, 24)
    for y in range(pod.h):
        for x in range(pod.w):
            v = pod.p[y][x]
            if v is not None:
                body.px(16 - y, 14 + x - 2, v)
    # relight: top edge lit, belly dark, keep the greens and bandana
    keep = {col(c) for c in ("G", "g", "l", "w", "e")}
    for y in range(24):
        for x in range(16):
            v = body.get(x, y)
            if v is None or v in keep:
                continue
            if body.get(x, y - 1) is None:
                c = "h"
            elif body.get(x, y + 1) is None or body.get(x, y + 2) is None:
                c = "r"
            else:
                c = "R"
            if v == col("R") and c == "R" and body.get(x, y - 2) is None:
                c = "W" if 4 <= x <= 9 else "R"
            if v == col("d"):
                c = "d"
            body.px(x, y, c)
    # stalk quiff at the head end, leaning back with the speed
    for (x, y, c) in [(15, 16, "G"), (15, 15, "l"), (14, 15, "G")]:
        body.px(x, y, c)
    # knot on top and tails streaming up/back
    body.px(12, 14, "e")
    body.px(13, 14, "e")
    tl = Spr(16, 24)
    angs = [(140, 3.5, 1.0), (60, 3.2, -0.8)] if alt == 0 else [(165, 3.8, -1.0), (85, 3.4, 0.8)]
    for (a, ln, b) in angs:
        tail(tl, 12, 14, a, ln, b)
    # legs and soles at the tip end, skidding away from us
    lg = Spr(16, 24)
    for (ax, top) in ((3, 20), (6, 21)):
        lg.line(7, 20, ax, top, "G")
        shoe_sole(lg, ax, top)
    dust = Spr(16, 24)
    puffs = ([(1.4, 21.0, 1.6, "e"), (14.4, 21.8, 1.3, "w"), (11, 22.6, 1.0, "e")]
             if alt == 0 else
             [(0.9, 19.8, 1.2, "w"), (2.2, 21.8, 1.8, "e"), (14.0, 21.0, 1.6, "e"),
              (10.2, 22.5, 1.2, "w")])
    for (x, y, r, c) in puffs:
        dust.ellipse(x, y, r, r, c)
    s = Spr(16, 24)
    for lay in (dust, lg, body, tl):
        lay.outline()
        s.over(lay)
    for (x, y) in ([(0, 15), (1, 18)] if alt == 0 else [(0, 17), (1, 14)]):
        if s.get(x, y) is None and s.get(x + 1, y) is None:
            s.px(x, y, "w")
            s.px(x + 1, y, "e")
    return s


def flame_layer(hero, frame):
    """Flame tongues licking up behind the hero plus a hot rim hugging the
    body.  The two frames swap tall and short tongues and sway the other way."""
    s = Spr(16, 24)
    order = [None, col("R"), col("O"), col("Y"), col("F")]

    def put(x, y, c):
        cur = s.get(x, y)
        if 0 <= x < 16 and 0 <= y < 24 and order.index(cur) < order.index(col(c)):
            s.px(x, y, c)

    tall = [(1.5, 11, 2.0), (4.5, 15, 2.6), (8.0, 18, 3.2), (11.5, 13, 2.6), (14.5, 10, 2.0)]
    short = [(1.5, 14, 2.2), (4.5, 11, 2.4), (8.0, 15, 3.0), (11.5, 18, 2.8), (14.5, 12, 2.0)]
    tongues = tall if frame == 0 else short
    base = 17
    for n, (bx, h, w) in enumerate(tongues):
        ph = n * 1.9 + frame * math.pi
        for y in range(24):
            t = (base - y) / h
            if t > 1 or t < 0:
                continue
            hw = w * (1 - t) ** 0.45
            cxp = bx + 1.8 * math.sin(t * 3.0 + ph) * t
            for x in range(16):
                dx = abs(x + 0.5 - cxp) / max(0.01, hw)
                if dx <= 1:
                    k = dx * 0.55 + t * 0.55
                    put(x, y, "F" if k < 0.3 else ("Y" if k < 0.55 else ("O" if k < 0.8 else "R")))
    for (x0, side) in ((0, 1), (15, -1)):
        for y in range(17, 22):
            reach = 2 if (y + frame) % 3 else 1
            for d in range(reach):
                put(x0 + side * d, y, "O" if d == 0 else "R")
    s.outline("k", ignore=("k",))
    return s


def dash_frames():
    out = []
    crouch = dict(T=8, H=13, W=10, legs=[(5, 0, False), (10, 3, True)],
                  hands=[(1, 16, True), (13, 12, False)])
    for i, base in enumerate([crouch, mirror_spec(crouch)]):
        hero = hero_back(**base, phase=i * math.pi + 0.7)
        fl = flame_layer(hero, i)
        fl.over(hero)
        for (x, y) in ([(0, 3), (14, 0), (15, 8)] if i == 0 else [(1, 0), (15, 3), (0, 9)]):
            if fl.get(x, y) is None:
                fl.px(x, y, "Y")
        out.append(fl)
    return out


def star4(s, cx, cy, c1="Y", c2="F"):
    for dx, dy in [(0, -1), (0, 1), (-1, 0), (1, 0)]:
        s.px(cx + dx, cy + dy, c1)
    s.px(cx, cy, c2)


def hero_front(T, H, W, legs, hands, eyes="normal", mouth="open", leaf="mid",
               curl=3.0, extra=None):
    """Facing the camera (hurt frame and the splash)."""
    B = T + H - 1
    back, lg, body = Spr(16, 24), Spr(16, 24), Spr(16, 24)
    rows = chili_body(body, T, H, W, curl=curl)
    stalk(body, T, leaf)
    bandana_band(body, rows, T, knot=False)
    face(body, T, H, eyes, mouth)
    for i, (hx, hy) in enumerate(hands):
        l, r = rows[T + 6]
        back.line(l + 1 if i == 0 else r - 1, T + 6, hx + (1 if i == 0 else 0), hy, "G")
        hand(back, hx, hy)
    for i, ((ax, lift), hip) in enumerate(zip(legs, (6, 9))):
        top = 21 - lift
        lg.line(hip, B - 2, ax, top - 1, "G")
        xs = [ax - 1, ax, ax + 1]
        for x in xs:
            lg.px(x, top, "T")
            lg.px(x, top + 1, "w")
        lg.px(ax, top, "w")
    tl = Spr(16, 24)
    for (ang, ln, bend) in tails_for(0.0, "flop"):
        tail(tl, 8, T + 2, ang, ln, bend)
    out = Spr(16, 24)
    for lay in (tl, back, lg, body):
        lay.outline()
        out.over(lay)
    if extra:
        extra(out)
    return out


def hurt_frame():
    # spun round by the crash: facing us, dizzy
    def extra(s):
        st = Spr(16, 24)
        star4(st, 2, 2)
        star4(st, 13, 1)
        st.px(8, 0, "Y")
        st.outline()
        s.over(st)
    return hero_front(T=6, H=15, W=10, legs=[(4, 0), (11, 0)],
                      hands=[(0, 17), (14, 17)], eyes="x", mouth="wavy",
                      leaf="down", curl=-2.0, extra=extra)


def splash_hero():
    return hero_front(T=4, H=16, W=12, legs=[(5, 0), (10, 3)],
                      hands=[(0, 9), (14, 14)], eyes="normal", mouth="open", leaf="up")


# ================================================================ OBSTACLES
def fence():
    s = Spr(32, 16)
    posts = [2, 14, 27]
    # planks behind posts
    for py in (4, 9):
        s.rect(1, py, 30, 1, "n")
        s.rect(1, py + 1, 30, 2, "B")
        s.rect(1, py + 3, 30, 1, "b")
        for gx in range(3, 30, 5):
            s.px(gx, py + 2, "b")
            s.px(gx + 1, py + 2, "b")
    # red/white warning plank paint on the top rail
    for x in range(1, 31):
        if (x // 3) % 2 == 0:
            s.px(x, 5, "R")
            s.px(x, 6, "r")
    for px_ in posts:
        s.rect(px_, 2, 3, 13, "B")
        s.rect(px_, 2, 1, 13, "n")
        s.rect(px_ + 2, 2, 1, 13, "b")
        s.px(px_ + 1, 1, "n")
        s.px(px_ + 1, 6, "j")  # nail
        s.px(px_ + 1, 11, "j")
    # tufts of dry grass at the feet
    for gx in (6, 11, 20, 24):
        s.px(gx, 14, "y")
        s.px(gx + 1, 13, "Y")
    s.outline()
    return s


def banner():
    s = Spr(32, 32)
    # poles
    for x in (2, 28):
        s.rect(x, 2, 2, 28, "b")
        s.rect(x, 2, 1, 28, "B")
        s.rect(x - 1, 0, 4, 2, "Y")
        s.px(x - 1, 0, "F")
        s.rect(x - 1, 29, 4, 2, "z")
        s.rect(x - 1, 29, 1, 2, "x")
    # string
    for x in range(4, 28):
        y = 2 + (1 if 8 <= x <= 23 else 0)
        s.px(x, y, "e")
    colors = [("i", "I"), ("Y", "y"), ("c", "C"), ("G", "g"), ("O", "o")]
    for n in range(5):
        x0 = 4 + n * 5
        top = 3 + (1 if 1 <= n <= 3 else 0)
        c1, c2 = colors[n]
        for y in range(top, top + 7):
            for x in range(x0, x0 + 4):
                s.px(x, y, c1 if x < x0 + 3 else c2)
        # zig-zag bottom
        s.px(x0 + 1, top + 7, c1)
        s.px(x0 + 2, top + 7, c1)
        # papel picado cut-outs
        for (dx, dy) in [(1, 1), (2, 1), (1, 3), (2, 3), (0, 5), (3, 5)]:
            s.px(x0 + dx, top + dy, None) if False else None
        s.px(x0 + 1, top + 2, "w")
        s.px(x0 + 2, top + 2, "w")
        s.px(x0 + 1, top + 4, "_k" and K)
        s.px(x0 + 2, top + 4, K)
    s.outline()
    return s


def cactus():
    s = Spr(24, 40)
    ramp = ["l", "G", "G", "g", "q"]

    def column(x0, x1, y0, y1):
        w = x1 - x0 + 1
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                t = (x - x0) / max(1, w - 1)
                c = "l" if t < 0.2 else ("G" if t < 0.6 else "g")
                if (x - x0) % 3 == 2 and t < 0.9:
                    c = "g" if c != "g" else "q"
                s.px(x, y, c)
        # rounded cap
        s.px(x0, y0, None)
        s.px(x1, y0, None)

    column(8, 15, 3, 39)          # trunk
    column(2, 6, 9, 22)           # left arm upright
    s.rect(4, 21, 5, 4, "G")      # left elbow
    s.rect(4, 24, 5, 1, "g")
    s.rect(4, 21, 5, 1, "l") if False else None
    column(18, 21, 5, 17)         # right arm upright
    s.rect(15, 15, 5, 4, "G")
    s.rect(15, 18, 5, 1, "g")
    # elbows lit edge
    s.rect(4, 21, 4, 1, "G")
    # spines
    for (x, y) in [(9, 8), (13, 12), (10, 18), (14, 24), (9, 29), (12, 34),
                   (3, 13), (5, 18), (19, 9), (20, 14), (11, 6), (14, 31)]:
        s.px(x, y, "w")
    # flower crown
    s.px(11, 2, "P")
    s.px(12, 2, "i")
    s.px(11, 1, "i")
    s.px(12, 1, "P")
    s.px(4, 8, "Y")
    # sand mound at the base
    for x in range(6, 18):
        s.px(x, 39, "a" if x > 11 else "s")
    s.px(7, 38, "s")
    s.px(16, 38, "a")
    s.outline()
    return s


def weed(frame):
    """Tumbleweed: an airy 4-fold spiral of twigs.  Frame 1 is frame 0 turned
    45 degrees, so playing 0,1,0,1 reads as continuous rolling."""
    s = Spr(16, 16)
    cx, cy = 8.0, 8.0
    rot = frame * math.pi / 4

    def tw(x, y, c):
        if math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= 6.9:
            s.px(int(x), int(y), c)

    for k in range(80):  # rim
        a = 2 * math.pi * k / 80
        r = 6.0 + 0.6 * math.sin(a * 4 - rot * 4)
        lit = math.cos(a + rot + math.pi * 0.75) > 0.0
        tw(cx + r * math.cos(a + rot), cy + r * math.sin(a + rot), "B" if lit else "b")
    for arm in range(4):  # spiral branches
        for k in range(40):
            t = k / 39
            a = rot + arm * math.pi / 2 + t * 2.6
            r = 0.6 + t * 5.4
            lit = math.cos(a + math.pi * 0.75) > -0.2
            tw(cx + r * math.cos(a), cy + r * math.sin(a), "n" if lit else "B")
    for arm in range(4):  # inner loops between branches
        for k in range(16):
            a = rot + arm * math.pi / 2 + math.pi / 4 + k / 15 * 1.2
            tw(cx + 3.4 * math.cos(a), cy + 3.4 * math.sin(a), "b")
    # dark brown tangle fills the gaps next to twigs, the rest stays see-through
    s.outline("j", ignore=("j",))
    for y in range(16):
        for x in range(16):
            if s.get(x, y) == col("j") and math.hypot(x + 0.5 - cx, y + 0.5 - cy) < 5.2:
                if (x * 3 + y * 5 + frame) % 4 == 0:
                    s.px(x, y, None)
    s.outline()
    if all(s.get(x, 15) is None for x in range(16)):
        sh = Spr(16, 16)
        sh.over(s, 0, 1)
        s = sh
    return s


def cart():
    s = Spr(32, 16)
    # awning
    for x in range(1, 31):
        c = "R" if (x // 3) % 2 == 0 else "w"
        c2 = "r" if c == "R" else "e"
        s.px(x, 1, c)
        s.px(x, 2, c)
        s.px(x, 3, c2)
        if x % 3 == 1:
            s.px(x, 4, c2)
    s.rect(2, 0, 28, 1, "Y")
    # poles
    for x in (3, 28):
        s.rect(x, 4, 1, 4, "x")
    # steam from pot
    s.px(12, 5, "e")
    s.px(13, 4, "w") if False else None
    # pot
    s.rect(10, 6, 5, 2, "z")
    s.rect(10, 6, 5, 1, "x")
    # counter top
    s.rect(1, 8, 30, 1, "n")
    # body
    s.rect(1, 9, 30, 4, "T")
    s.rect(1, 9, 30, 1, "c")
    s.rect(1, 12, 30, 1, "t")
    # chili logo on the side
    s.grid(["..g", "hRR", "RRr", ".r."], 15, 9)
    # yellow trim stripe
    for x in range(2, 30, 2):
        s.px(x, 11, "Y") if not (14 <= x <= 18) else None
    # wheels
    for wx in (6, 24):
        s.ellipse(wx, 13.5, 2.4, 2.4, "z")
        s.px(wx, 13, "e")
        s.px(wx - 1, 12, "x")
    # handle
    s.line(30, 9, 31, 7, "x")
    s.outline()
    return s


def barrier():
    s = Spr(32, 16)
    pts = [(1, 15.9), (2, 9), (5, 6), (9, 5), (12, 3), (17, 4), (21, 3), (25, 6),
           (29, 8), (31, 12), (31, 15.9)]
    s.poly(pts, "3")
    # lit facets
    for y in range(16):
        for x in range(32):
            v = s.get(x, y)
            if v is not None:
                above = s.get(x, y - 1)
                left = s.get(x - 1, y)
                if above is None:
                    s.px(x, y, "1")
                elif left is None or s.get(x - 1, y - 1) is None:
                    s.px(x, y, "2")
                elif y > 12:
                    s.px(x, y, "4")
    # glowing cracks
    cracks = [[(7, 7), (8, 9), (7, 11), (9, 13), (9, 15)],
              [(16, 5), (15, 8), (17, 10), (16, 13)],
              [(24, 7), (25, 9), (23, 11), (24, 14)]]
    for cr in cracks:
        for a, b in zip(cr, cr[1:]):
            s.line(a[0], a[1], b[0], b[1], "O")
        for (x, y) in cr[1:-1]:
            s.px(x, y, "Y")
    # molten top drips
    for x in (11, 12, 19, 20):
        s.px(x, 4 if x < 15 else 4, "O")
    s.px(12, 3, "Y")
    s.px(20, 3, "Y")
    s.px(11, 2, "O") if False else None
    # base glow
    for x in range(3, 30, 4):
        s.px(x, 15, "o")
    s.outline()
    return s


def vending():
    s = Spr(24, 40)
    # cabinet
    s.rect(2, 2, 20, 37, "I")
    s.rect(2, 2, 1, 37, "i")
    s.rect(21, 2, 1, 37, "p")
    # header sign
    s.rect(3, 3, 18, 5, "M")
    w = text3_width("SODA")
    text3(s, "SODA", 3 + (18 - w) // 2, 3, ["c", "Y", "c", "Y"])
    # glass front
    s.rect(4, 9, 12, 20, "N")
    s.rect(4, 9, 12, 1, "c")
    s.rect(4, 9, 1, 20, "C")
    items = ["R", "Y", "G", "T", "O", "P"]
    for row in range(4):
        y = 11 + row * 5
        for c in range(3):
            x = 5 + c * 4
            ic = items[(row * 3 + c) % len(items)]
            s.rect(x, y, 2, 3, ic)
            s.px(x, y, "w")
            s.rect(x - 1, y + 3, 4, 1, "e")
    # glass glint
    s.line(14, 10, 10, 18, "c")
    # side panel buttons and coin slot
    s.rect(17, 9, 4, 20, "M")
    for y in (11, 14, 17):
        s.rect(18, y, 2, 2, "c" if y != 14 else "i")
    s.rect(19, 21, 1, 3, "Y")
    # dispenser tray
    s.rect(5, 31, 14, 4, "M")
    s.rect(5, 31, 14, 1, "4")
    # feet
    s.rect(3, 39, 3, 1, "z")
    s.rect(18, 39, 3, 1, "z")
    # neon cap on top
    s.rect(3, 1, 18, 1, "c")
    s.outline()
    return s


def spire():
    s = Spr(24, 40)
    pts = [(1, 40), (5, 28), (7, 18), (9, 9), (11.5, 0.5), (14, 8), (15, 15),
           (18, 24), (20, 31), (23, 40)]
    s.poly(pts, "3")
    # facets: left lit, centre ridge, right dark
    for y in range(40):
        row = [x for x in range(24) if s.get(x, y) is not None]
        if not row:
            continue
        x0, x1 = row[0], row[-1]
        ridge = x0 + (x1 - x0) * 0.42
        for x in row:
            if x < ridge - 1:
                c = "2"
            elif x < ridge + 1:
                c = "1" if y % 7 else "2"
            else:
                c = "4" if x > x1 - 2 else "3"
            s.px(x, y, c)
        s.px(x0, y, "1")
    # lava cracks zig-zagging down
    crack = [(11, 6), (12, 11), (10, 16), (13, 22), (11, 27), (14, 33), (12, 39)]
    for a, b in zip(crack, crack[1:]):
        s.line(a[0], a[1], b[0], b[1], "O")
    for (x, y) in crack[1:-1]:
        s.px(x, y, "Y")
    side = [(6, 30), (8, 34), (7, 39)]
    for a, b in zip(side, side[1:]):
        s.line(a[0], a[1], b[0], b[1], "o")
    side = [(17, 26), (16, 31), (18, 36)]
    for a, b in zip(side, side[1:]):
        s.line(a[0], a[1], b[0], b[1], "o")
    s.px(12, 1, "w")
    # glowing base
    for x in range(2, 23):
        s.px(x, 39, "O" if x % 3 else "Y")
    s.outline()
    return s


def banner_city():
    s = Spr(32, 32)
    for x in (2, 28):
        s.rect(x, 1, 2, 29, "z")
        s.rect(x, 1, 1, 29, "x")
        s.rect(x - 1, 29, 4, 2, "4")
        s.rect(x - 1, 29, 1, 2, "z")
    # sign board
    s.rect(1, 1, 30, 9, "M")
    # neon tube border
    for x in range(2, 30):
        s.px(x, 2, "i")
        s.px(x, 8, "i")
    for y in range(2, 9):
        s.px(2, y, "i")
        s.px(29, y, "i")
    w = text3_width("TACOS")
    text3(s, "TACOS", 2 + (28 - w) // 2, 3, ["c"])
    # chili glyphs either side
    for x in (5, 25):
        s.px(x, 4, "G")
        s.px(x, 5, "R")
        s.px(x + 1, 5, "R")
        s.px(x, 6, "R")
    # hanging chains
    s.px(1, 0, "x")
    s.px(30, 0, "x")
    s.outline()
    return s


def banner_lava():
    s = Spr(32, 32)
    # pillars of stacked stones
    for x0 in (1, 25):
        for n, y in enumerate(range(10, 32, 4)):
            off = (n % 2)
            s.rect(x0, y, 6, 4, "2")
            s.rect(x0, y, 6, 1, "1")
            s.rect(x0 + 5, y, 1, 4, "3")
            s.px(x0 + (2 if off else 4), y + 1, "3")
    # lintel blocks
    s.rect(0, 1, 32, 9, "2")
    s.rect(0, 1, 32, 1, "1")
    s.rect(0, 9, 32, 1, "3")
    for x in (8, 16, 24):
        s.rect(x, 2, 1, 7, "3")
    # keystone with a glowing chili rune
    s.rect(13, 0, 6, 10, "1")
    s.rect(14, 1, 4, 8, "2")
    s.px(16, 3, "G")
    s.px(15, 4, "O")
    s.px(16, 4, "Y")
    s.px(15, 5, "Y")
    s.px(16, 5, "O")
    s.px(15, 6, "O")
    # lava seeping from the joints
    for (x, y) in [(5, 9), (5, 10), (27, 9), (27, 10), (27, 11), (9, 9), (22, 9)]:
        s.px(x, y, "O")
    s.px(5, 11, "Y")
    s.px(27, 12, "Y")
    # cracks on the lintel
    s.line(3, 3, 5, 6, "o")
    s.line(26, 3, 28, 6, "o")
    s.outline()
    return s


# ================================================================ PICKUPS
CHILI = [
    "........",
    ".....g..",
    "....gG..",
    "..gGGGg.",
    ".hRRRRr.",
    ".hRRRRr.",
    ".hRRRrr.",
    "..RRRr..",
    "..RRrr..",
    "..Rrr...",
    ".Rr.....",
    "........",
]


def chili(frame, gold=False):
    s = Spr(8, 12)
    s.grid(CHILI, strict_w=8)
    if frame == 0:
        s.px(2, 4, "W")
        s.px(2, 5, "W")
        s.px(2, 6, "h")
    else:
        # the glint slides down the pod and a star pops on the tip of the stem
        s.px(2, 4, "h")
        s.px(3, 6, "W")
        s.px(3, 7, "W")
        s.px(2, 8, "W")
    if gold:
        s.recolor({"h": "6", "R": "7", "r": "8", "W": "w", "G": "G"})
    s.outline()
    if frame == 1 or gold:
        s.px(7, 0, "F")
    return s


def milk():
    s = Spr(12, 12)
    s.grid([
        "............",
        "....eee.....",
        "...wwwee....",
        "..wwwwwee...",
        "..LLLLLLD...",
        "..wwwwwwe...",
        "..wwLwwwe...",
        "..wLLLwwe...",
        "..wwLwwwe...",
        "..wwwwwwe...",
        "..DDDDDDD...",
        "............",
    ], strict_w=12)
    s.rect(4, 0, 2, 1, "e") if False else None
    s.px(4, 1, "w")
    # move it to centre (rect is 12 wide, carton is 7 wide)
    t = Spr(12, 12)
    t.over(s, 1, 0)
    t.outline()
    return t


def sombrero():
    s = Spr(16, 12)
    s.grid([
        "................",
        "......YYYy......",
        ".....YFYYYy.....",
        ".....YYYYYy.....",
        ".....YYYYyy.....",
        ".....RGRGRG.....",
        "..yYYYYYYYYYYy..",
        ".YFYYYYYYYYYYyy.",
        ".RYGYRYGYRYGYy..".replace("..", "y."),
        "..yyyyyyyyyyy...",
        "................",
        "................",
    ][:11] + ["................"], strict_w=16)
    s.outline()
    return s


def sparkle(frame):
    s = Spr(8, 8)
    if frame == 0:
        s.grid([
            "...F....",
            "...Y....",
            "..YFY...",
            "FYFwFYF.",
            "..YFY...",
            "...Y....",
            "...F....",
            "........",
        ])
    else:
        s.grid([
            "........",
            "........",
            "...Y....",
            "..YwY...",
            "...Y....",
            "........",
            ".....F..",
            "........",
        ])
    return s


# ================================================================ DECOR
def agave():
    s = Spr(16, 24)
    bx, by = 8.0, 23.0
    leaves = [  # angle from vertical (deg), length, half width: back to front
        (-8, 21, 1.6), (12, 19, 1.6), (-34, 15, 1.6), (38, 14, 1.6),
        (-66, 9, 1.5), (68, 9, 1.5), (-2, 11, 1.9)]
    for (deg, L, w) in leaves:
        a = math.radians(deg)
        tx, ty = bx + L * math.sin(a), by - L * math.cos(a)
        nx, ny = math.cos(a), math.sin(a)
        left = (bx - nx * w, by - ny * w)
        right = (bx + nx * w, by + ny * w)
        leaf = Spr(16, 24)
        leaf.poly([left, (tx, ty), (bx, by)], "U")
        leaf.poly([(bx, by), (tx, ty), right], "u")
        leaf.line(bx, by - 1, tx, ty, "u")
        leaf.px(int(tx), int(ty), "U")
        leaf.outline("v")
        s.over(leaf)
    for x in range(3, 13):
        s.px(x, 23, "a" if x > 8 else "s")
    s.outline()
    return s


def rock():
    s = Spr(16, 12)
    s.grid([
        "................",
        "................",
        ".......ssA......",
        "......saaAA.....",
        "....ssaaaAAj....",
        "...saaaaaAAjj...",
        "..saaaaaaaAjjj..",
        "..saaaaaAaAAjj..",
        ".saaaaaAaaaAjjj.",
        ".saaaaAaaaaaAjj.",
        "saaaaaAaaaaaAjjj",
        "saaaaAaaaaaaAjjj",
    ], strict_w=16)
    s.px(7, 2, "w")
    s.outline()
    return s


def skull():
    s = Spr(16, 12)
    s.grid([
        "x..............x",
        "ex............ex",
        ".ewx........xwe.",
        "..ewwweeeeewwe..",
        "....wwwwwwwee...",
        "....wkkwwkkwe...",
        "....wkkwwkkee...",
        ".....wwwwwee....",
        "......wwwe......",
        "......wwwe......",
        "......wkke......",
        "......wwee......",
    ], strict_w=16)
    t = Spr(16, 12)
    t.over(s)
    t.outline()
    return t


def sign():
    s = Spr(24, 24)
    # post
    s.rect(10, 13, 4, 11, "B")
    s.rect(10, 13, 1, 11, "n")
    s.rect(13, 13, 1, 11, "b")
    # board
    s.rect(1, 1, 22, 13, "Y")
    s.rect(1, 1, 22, 1, "F")
    s.rect(1, 13, 22, 1, "y")
    s.rect(22, 1, 1, 13, "y")
    w = text3_width("PICANTE", gap=0)
    text3(s, "PICANTE", 1 + (22 - w) // 2 + 1, 3, ["r", "R"], gap=0)
    # three chili "heat" glyphs
    for x in (5, 11, 17):
        s.px(x + 1, 9, "g")
        s.px(x, 10, "R")
        s.px(x + 1, 10, "R")
        s.px(x + 1, 11, "r")
        s.px(x + 2, 11, "r") if False else None
    # bolts
    s.px(2, 2, "y")
    s.px(21, 2, "y")
    s.outline()
    return s


def lamp():
    s = Spr(8, 32)
    s.rect(3, 4, 2, 26, "z")
    s.rect(3, 4, 1, 26, "x")
    # neon band
    s.rect(3, 14, 2, 2, "i")
    # base
    s.rect(2, 29, 4, 3, "4")
    s.rect(2, 29, 1, 3, "z")
    # head with glowing tube
    s.rect(0, 1, 8, 3, "M")
    s.rect(1, 3, 6, 1, "c")
    s.rect(1, 2, 6, 1, "C")
    s.px(1, 3, "w")
    s.rect(3, 0, 2, 1, "z")
    s.outline()
    return s


def tower():
    s = Spr(24, 32)
    s.rect(2, 6, 20, 26, "N")
    s.rect(2, 6, 2, 26, "p")
    s.rect(20, 6, 2, 26, "M")
    s.rect(1, 5, 22, 1, "p")
    # roof box and antenna
    s.rect(7, 3, 8, 2, "N")
    s.rect(11, 0, 1, 3, "x")
    s.px(11, 0, "R")
    rnd = random.Random(5)
    for wy in range(8, 29, 3):
        for wx in range(5, 19, 3):
            lit = rnd.random()
            c = "Y" if lit < 0.45 else ("c" if lit < 0.6 else ("i" if lit < 0.67 else "M"))
            s.rect(wx, wy, 2, 2, c)
            if c == "Y":
                s.px(wx, wy, "F")
    # door
    s.rect(10, 28, 4, 4, "M")
    s.rect(10, 28, 4, 1, "c")
    # vertical neon sign on the left
    s.rect(0, 10, 2, 10, "i")
    s.rect(0, 10, 1, 10, "P")
    s.outline()
    return s


def pillar():
    s = Spr(16, 32)
    cols = [(1, 5, 9), (6, 10, 2), (11, 14, 12)]  # x0, x1, top
    order = [0, 2, 1]
    for i in order:
        x0, x1, top = cols[i]
        for y in range(top, 32):
            for x in range(x0, x1 + 1):
                c = "3"
                if x == x0:
                    c = "2"
                if x == x1:
                    c = "4"
                s.px(x, y, c)
        # hex top face
        s.rect(x0, top, x1 - x0 + 1, 2, "1")
        s.px(x0, top, "2")
        s.px(x1, top, "2")
        s.rect(x0, top + 2, x1 - x0 + 1, 1, "4")
        # cracks
        for y in range(top + 6, 32, 7):
            s.px(x0 + 1 + (y % 2), y, "4")
    # lava glow at the base
    for x in range(1, 15):
        s.px(x, 31, "O" if x % 2 else "o")
        if x % 3 == 0:
            s.px(x, 30, "o")
    s.outline()
    return s


def vent():
    s = Spr(16, 16)
    # smoke column: three puffs, lit from the top-left
    for (x, y, r) in [(10.5, 2.6, 2.6), (5.5, 4.6, 2.3), (8.5, 7.6, 2.0)]:
        puff = Spr(16, 16)
        puff.shaded_ellipse(x, y, r, r, ["w", "w", "e", "x"])
        puff.outline()
        s.over(puff)
    # cone of dark rock
    cone = Spr(16, 16)
    cone.poly([(0.5, 16), (4.5, 9.5), (11.5, 9.5), (15.5, 16)], "3")
    for y in range(16):
        for x in range(16):
            if cone.get(x, y) is None:
                continue
            row = [xx for xx in range(16) if cone.get(xx, y) is not None]
            if x <= row[0] + 1:
                cone.px(x, y, "2")
            elif x >= row[-1] - 1:
                cone.px(x, y, "4")
    cone.rect(5, 10, 6, 1, "F")
    cone.rect(4, 11, 8, 1, "Y")
    cone.rect(5, 12, 6, 1, "O")
    for (x, y) in [(4, 13), (11, 14), (7, 15), (2, 15), (13, 15)]:
        cone.px(x, y, "o")
    cone.outline()
    s.over(cone)
    return s


# ================================================================ HUD
def hud(name):
    s = Spr(8, 8)
    g = {
        "chili": [
            "........",
            ".....g..",
            "...gGg..",
            "..hRRr..",
            "..RRRr..",
            "..RRr...",
            ".Rr.....",
            "........",
        ],
        "shield": [
            "...ee...",
            "..wwwe..",
            ".LLLLD..",
            ".wwLwe..",
            ".wLLLe..",
            ".wwLwe..",
            ".DDDDD..",
            "........",
        ],
        "flame": [
            "...O....",
            "..OO.O..",
            "..OYOO..",
            ".OYYYO..",
            ".OYFYO..",
            ".RYFYR..",
            "..RRR...",
            "........",
        ],
        "magnet": [
            "........",
            "...YY...",
            "..YFYy..",
            "..RGRG..",
            "YYYYYYy.",
            ".yyyyy..",
            "........",
            "........",
        ],
        "star": [
            "...Y....",
            "...Y....",
            "YYYFYYY.",
            ".YFFFY..",
            "..YYY...",
            ".YY.YY..",
            ".Y...Y..",
            "........",
        ],
    }[name]
    s.grid(g, strict_w=8)
    # shift right by half a pixel isn't possible; centre the 7-wide icons
    t = Spr(8, 8)
    t.over(s, 0, 0)
    t.outline()
    return t


# ================================================================ SHEET
def parse_rects(path):
    rects = {}
    txt = open(path).read()
    m = re.search(r"\((\d+)x(\d+) RGBA", txt)
    size = (int(m.group(1)), int(m.group(2))) if m else (128, 128)
    txt = re.sub(r"//[^\n]*", "", txt)
    for mm in re.finditer(r"const Rect\s+(\w+)(\[\d+\])?\s*=\s*([^;]*);", txt):
        rs = [tuple(int(v) for v in r) for r in
              re.findall(r"Rect\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", mm.group(3))]
        rects[mm.group(1)] = rs if mm.group(2) else rs[0]
    return size, rects


BOTTOM_ANCHORED = {"run", "jump", "jump_up", "jump_down", "land", "slide2", "slide", "dash", "hurt", "fence", "banner",
                   "cactus", "weed", "cart", "barrier", "vending", "spire",
                   "banner_city", "banner_lava", "agave", "rock", "skull",
                   "sign", "lamp", "tower", "pillar", "vent"}


def build_sprites():
    return {
        "run": run_frames(),
        "jump": jump_frame(),
        "jump_up": jump_up_frame(),
        "jump_down": jump_down_frame(),
        "land": land_frame(),
        "slide": slide_frame(0),
        "slide2": slide_frame(1),
        "dash": dash_frames(),
        "hurt": hurt_frame(),
        "fence": fence(),
        "banner": banner(),
        "cactus": cactus(),
        "weed": [weed(0), weed(1)],
        "cart": cart(),
        "barrier": barrier(),
        "vending": vending(),
        "spire": spire(),
        "banner_city": banner_city(),
        "banner_lava": banner_lava(),
        "chili": [chili(0), chili(1)],
        "milk": milk(),
        "sombrero": sombrero(),
        "golden": chili(0, gold=True),
        "sparkle": [sparkle(0), sparkle(1)],
        "agave": agave(),
        "rock": rock(),
        "skull": skull(),
        "sign": sign(),
        "lamp": lamp(),
        "tower": tower(),
        "pillar": pillar(),
        "vent": vent(),
        "hud_chili": hud("chili"),
        "hud_shield": hud("shield"),
        "hud_flame": hud("flame"),
        "hud_magnet": hud("magnet"),
        "hud_star": hud("star"),
    }


def make_sheet():
    size, rects = parse_rects(os.path.join(ROOT, "sprites.hpp"))
    sprites = build_sprites()
    sheet = Image.new("RGBA", size, (0, 0, 0, 0))
    placed = []
    for name, r in rects.items():
        if name not in sprites:
            sys.exit("no art for sprite '%s'" % name)
        arts = sprites[name] if isinstance(sprites[name], list) else [sprites[name]]
        rs = r if isinstance(r, list) else [r]
        assert len(arts) == len(rs), name
        for art, (x, y, w, h) in zip(arts, rs):
            assert (art.w, art.h) == (w, h), (name, art.w, art.h, w, h)
            sheet.paste(art.image(), (x, y))
            placed.append((name, (x, y, w, h)))
    missing = set(sprites) - set(rects)
    if missing:
        print("warning: art without a rect:", sorted(missing))
    verify(sheet, placed)
    return sheet


def verify(sheet, placed):
    W, H = sheet.size
    px = sheet.load()
    inside_any = [[False] * W for _ in range(H)]
    for name, (x, y, w, h) in placed:
        for j in range(y, y + h):
            for i in range(x, x + w):
                inside_any[j][i] = True
    problems = []
    for j in range(H):
        for i in range(W):
            a = px[i, j][3]
            if a not in (0, 255):
                problems.append("alpha %d at %d,%d" % (a, i, j))
            if a and not inside_any[j][i]:
                problems.append("pixel outside rects at %d,%d" % (i, j))
    for name, (x, y, w, h) in placed:
        if name not in BOTTOM_ANCHORED:
            continue
        xs = [i for j in range(y, y + h) for i in range(x, x + w) if px[i, j][3]]
        bottom = [i for i in range(x, x + w) if px[i, y + h - 1][3]]
        if not bottom:
            problems.append("%s: bottom row empty" % name)
        if xs:
            c = (min(xs) + max(xs) + 1) / 2 - x
            if abs(c - w / 2) > 1.0:
                problems.append("%s: not centred (%.1f vs %.1f)" % (name, c, w / 2))
    if problems:
        print("\n".join(problems[:40]))
        sys.exit("sheet verification failed (%d problems)" % len(problems))
    print("sheet ok: %d sprites, nothing outside rects, bottom rows touched" % len(placed))


def make_preview(sheet, scale=4):
    W, H = sheet.size
    bg = Image.new("RGBA", (W * scale, H * scale))
    a, b = (200, 200, 210, 255), (160, 160, 175, 255)
    for y in range(0, H * scale, 8):
        for x in range(0, W * scale, 8):
            c = a if ((x // 8) + (y // 8)) % 2 == 0 else b
            bg.paste(c, (x, y, x + 8, y + 8))
    big = sheet.resize((W * scale, H * scale), Image.NEAREST)
    bg.alpha_composite(big)
    return bg.convert("RGB")


# ================================================================ SPLASH
BIG = {
    "C": [".####", "##..#", "##...", "##...", "##...", "##..#", ".####"],
    "H": ["##.##", "##.##", "##.##", "#####", "##.##", "##.##", "##.##"],
    "I": ["####", ".##.", ".##.", ".##.", ".##.", ".##.", "####"],
    "L": ["##...", "##...", "##...", "##...", "##...", "##...", "#####"],
    "R": ["####.", "##.##", "##.##", "####.", "##.#.", "##.##", "##.##"],
    "U": ["##.##", "##.##", "##.##", "##.##", "##.##", "##.##", ".###."],
    "S": [".####", "##...", "##...", ".###.", "...##", "...##", "####."],
}


def draw_logo(canvas, text, cy, scale, ramp, shadow):
    """canvas: Spr. Draws text centred horizontally with top at cy."""
    widths = [len(BIG[c][0]) for c in text]
    total = (sum(widths) + len(text) - 1) * scale
    x = (canvas.w - total) // 2
    layer = Spr(canvas.w, canvas.h)
    for ch in text:
        g = BIG[ch]
        for j, row in enumerate(g):
            for i, v in enumerate(row):
                if v != "#":
                    continue
                for yy in range(scale):
                    for xx in range(scale):
                        t = (j * scale + yy) / (7 * scale)
                        c = ramp[min(len(ramp) - 1, int(t * len(ramp)))]
                        layer.px(x + i * scale + xx, cy + j * scale + yy, c)
        # top-left highlight on each glyph pixel row 0
        x += (len(g[0]) + 1) * scale
    # highlight: pixels whose upper neighbour is empty
    hl = []
    for y in range(layer.h):
        for xx in range(layer.w):
            if layer.p[y][xx] is not None and layer.get(xx, y - 1) is None:
                hl.append((xx, y))
    for (xx, y) in hl:
        layer.p[y][xx] = col("F")
    layer.outline("k", diag=True)
    # drop shadow
    sh = Spr(canvas.w, canvas.h)
    for y in range(layer.h):
        for xx in range(layer.w):
            if layer.p[y][xx] is not None:
                sh.px(xx + 2, y + 2, shadow)
    canvas.over(sh)
    canvas.over(layer)


def make_logo():
    """112x34 RGBA title logo for the in-game title screen (drawn over the road):
    tall chunky letters, yellow-to-red gradient, thick dark outline, drop shadow,
    and a flame licking up from each I."""
    W, H = 112, 34
    sx, sy = 2, 3
    text = "CHILI RUSH"
    ramp = [hexc("ffe14a"), hexc("ffc23a"), hexc("ff9a2a"), hexc("ff7020"),
            hexc("f04a28"), hexc("e8302a"), hexc("c41e2a")]
    glyphs = dict(BIG)
    widths = [1 if c == " " else len(glyphs[c][0]) for c in text]
    total = (sum(widths) + len(text) - 1) * sx
    x = (W - 1 - total) // 2
    top = 9
    letters = Spr(W, H)
    flames_at = []
    for ch, w in zip(text, widths):
        if ch != " ":
            for j, row in enumerate(glyphs[ch]):
                for i, v in enumerate(row):
                    if v != "#":
                        continue
                    for yy in range(sy):
                        for xx in range(sx):
                            t = (j * sy + yy) / (7 * sy)
                            letters.px(x + i * sx + xx, top + j * sy + yy,
                                       ramp[min(len(ramp) - 1, int(t * len(ramp)))])
            if ch == "I":
                flames_at.append(x + 2 * sx)
        x += (w + 1) * sx
    for y in range(H):
        for xx in range(W):
            if letters.p[y][xx] is not None and letters.get(xx, y - 1) is None:
                letters.p[y][xx] = col("F")
    # flames on the I's
    for fx in flames_at:
        fl = Spr(W, H)
        fl.grid([".R...",
                 ".OR..",
                 "ROO.R",
                 "OYOOR",
                 "OYFYO",
                 ".OYO."], fx - 2, top - 7)
        letters.over(fl)
    letters.outline("k", diag=True)
    shadow = Spr(W, H)
    for y in range(H):
        for xx in range(W):
            if letters.p[y][xx] is not None:
                shadow.px(xx + 1, y + 2, "m")
    out = Spr(W, H)
    out.over(shadow)
    out.over(letters)
    out.outline("k", ignore=())
    return out.image()


def make_splash(hero_frame, cactus_spr, agave_spr):
    W, H = 128, 96
    s = Spr(W, H)
    horizon = 54
    sky = [hexc("2a1240"), hexc("4a1a5a"), hexc("8a2468"), hexc("d03a5a"),
           hexc("f0643a"), hexc("ff9a3a"), hexc("ffc850")]
    bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
    for y in range(horizon):
        t = y / (horizon - 1) * (len(sky) - 1)
        i = int(t)
        f = t - i
        for x in range(W):
            c = sky[i] if f * 16 <= bayer[y % 4][x % 4] or i + 1 >= len(sky) else sky[i + 1]
            s.px(x, y, c)
    # stars in the dark top
    rnd = random.Random(3)
    for _ in range(18):
        x, y = rnd.randrange(W), rnd.randrange(14)
        s.px(x, y, hexc("ffe0f0"))
    # sun with retro stripes
    cx, cy, R = 64, horizon - 2, 26
    for y in range(cy - R, horizon):
        for x in range(cx - R, cx + R + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= R * R:
                t = (y - (cy - R)) / R
                if y > cy - 12 and (y - (cy - 12)) % 5 in (0,) and y > cy - 10:
                    continue
                if y > cy - 4 and (y - cy) % 3 == 0:
                    continue
                c = hexc("fff2a0") if t < 0.35 else (hexc("ffd23c") if t < 0.7 else hexc("ff8a1e"))
                s.px(x, y, c)
    # mesas
    mesa = hexc("5a1a4a")
    mesa2 = hexc("3a1238")
    for (x0, x1, top, c) in [(0, 30, 40, mesa), (10, 22, 36, mesa), (96, 128, 42, mesa),
                             (104, 116, 38, mesa), (26, 40, 47, mesa2), (86, 102, 46, mesa2)]:
        for x in range(x0, x1):
            slope = min(x - x0, x1 - 1 - x)
            t = top if slope > 2 else top + (3 - slope) * 2
            for y in range(t, horizon):
                s.px(x, y, c)
        for x in range(x0 + 3, x1 - 3):
            s.px(x, top, hexc("c84a5a"))
    # desert ground
    ground = [hexc("e8884a"), hexc("d06a3a"), hexc("a84a32"), hexc("7a3030")]
    for y in range(horizon, H):
        t = (y - horizon) / (H - horizon) * (len(ground) - 1)
        i = int(t)
        f = t - i
        for x in range(W):
            c = ground[i] if f * 16 <= bayer[y % 4][x % 4] or i + 1 >= len(ground) else ground[i + 1]
            s.px(x, y, c)
    # road
    vx = 64
    for y in range(horizon, H):
        d = (y - horizon + 1) / (H - horizon)
        half = 3 + d * 62
        z = 1.0 / d
        for x in range(W):
            dx = x - vx
            if abs(dx) <= half:
                u = dx / half
                c = hexc("3a2c48")
                if abs(u) > 0.9:
                    c = hexc("fffaf0") if int(z * 3) % 2 == 0 else hexc("e8302a")
                elif abs(abs(u) - 0.3) < 0.035 + 0.01 / d and int(z * 2.2) % 2 == 0:
                    c = hexc("ffd23c")
                elif (x + y) % 7 == 0:
                    c = hexc("4a3a58")
                s.px(x, y, c)
    # roadside decor sprites (scaled by distance)
    def stamp(spr, bx, by, scale):
        im = spr.image()
        w, h = max(1, int(spr.w * scale)), max(1, int(spr.h * scale))
        im = im.resize((w, h), Image.NEAREST)
        for y in range(h):
            for x in range(w):
                p = im.getpixel((x, y))
                if p[3]:
                    s.px(bx - w // 2 + x, by - h + y, p[:3])
    stamp(cactus_spr, 104, horizon + 9, 0.5)
    stamp(agave_spr, 22, horizon + 7, 0.5)
    stamp(cactus_spr, 14, 92, 1.0)
    stamp(agave_spr, 116, 94, 1.0)
    # hero 2x with speed lines and dust
    for (x, y, l) in [(30, 70, 10), (34, 78, 8), (88, 72, 10), (86, 82, 8)]:
        for i in range(l):
            s.px(x + i, y, hexc("fff2a0"))
    stamp(hero_frame, 64, 94, 2.0)
    for (x, y, r) in [(50, 92, 2.5), (46, 90, 1.8), (78, 93, 2.0)]:
        for yy in range(int(y - r), int(y + r) + 1):
            for xx in range(int(x - r), int(x + r) + 1):
                if (xx - x) ** 2 + (yy - y) ** 2 <= r * r:
                    s.px(xx, yy, hexc("f4c48a"))
    # logo
    ramp = [hexc("ffe14a"), hexc("ffc23a"), hexc("ff9a2a"), hexc("ff7020"),
            hexc("f04a28"), hexc("e8302a"), hexc("c41e2a")]
    draw_logo(s, "CHILI", 3, 3, ramp, hexc("5a0a1e"))
    draw_logo(s, "RUSH", 29, 2, ramp, hexc("5a0a1e"))
    im = Image.new("RGB", (W, H))
    for y in range(H):
        for x in range(W):
            im.putpixel((x, y), s.p[y][x] or (0, 0, 0))
    return im


def make_run_gif(frames, path, scale=4, ms=83):
    """Run cycle on a little road so the bounce can be judged against a fixed line."""
    imgs = []
    W, H = 40, 30
    for n, f in enumerate(frames):
        bg = Image.new("RGBA", (W, H), hexc("3a2c48") + (255,))
        for y in range(0, 26):
            for x in range(W):
                bg.putpixel((x, y), hexc("f0643a") + (255,) if y > 16 else hexc("8a2468") + (255,))
        # scrolling lane dashes give a sense of speed
        for x in range(W):
            if ((x + n * 3) // 4) % 2 == 0:
                bg.putpixel((x, 28), hexc("ffd23c") + (255,))
        bg.alpha_composite(f.image(), ((W - 16) // 2, 26 - 24 + 1))
        imgs.append(bg.convert("RGB").resize((W * scale, H * scale), Image.NEAREST))
    imgs[0].save(path, save_all=True, append_images=imgs[1:], duration=ms, loop=0)


def make_icon():
    g = [
        "uuuuuulu",
        "uuuukGgu",
        "uuukhRRk",
        "uukhWRrk",
        "ukhRRrku",
        "khRRrkuu",
        "kRrkkuuu",
        "ukkuuuuu",
    ]
    pal = dict(PAL)
    pal["u"] = hexc("3a1d4a")
    im = Image.new("RGB", (8, 8))
    for y, row in enumerate(g):
        for x, ch in enumerate(row):
            im.putpixel((x, y), pal[ch])
    return im


def main():
    os.makedirs(ASSETS, exist_ok=True)
    sheet = make_sheet()
    sheet.save(os.path.join(ASSETS, "sprites.png"))
    make_preview(sheet).save(os.path.join(ASSETS, "preview.png"))
    sp = build_sprites()
    make_splash(sp["run"][0], sp["cactus"], sp["agave"]).save(os.path.join(ASSETS, "splash.png"))
    make_logo().save(os.path.join(ASSETS, "logo.png"))
    make_run_gif(sp["run"], os.path.join(ASSETS, "run-preview.gif"))
    ref = os.path.join(os.path.dirname(ROOT), "chili-city", "assets", "icon.png")
    icon = make_icon()
    if os.path.exists(ref):
        assert Image.open(ref).size == icon.size
    icon.save(os.path.join(ASSETS, "icon.png"))
    print("wrote sprites.png, preview.png, splash.png, icon.png, logo.png, run-preview.gif")


if __name__ == "__main__":
    main()
