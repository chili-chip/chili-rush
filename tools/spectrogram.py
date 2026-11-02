#!/usr/bin/env python3
"""spectrogram.py in.wav out.png [start_s] [len_s]: log-frequency spectrogram
(60 Hz .. 11 kHz) with the waveform envelope on top, written as a PNG with zlib only."""
import sys, wave, zlib, struct
import numpy as np
w = wave.open(sys.argv[1]); sr = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float) / 32768
a = float(sys.argv[3]) if len(sys.argv) > 3 else 0; n = float(sys.argv[4]) if len(sys.argv) > 4 else len(x) / sr
x = x[int(a * sr):int((a + n) * sr)]
hop, win = 110, 1024; H = 300; W = (len(x) - win) // hop
freqs = np.geomspace(60, 11000, H)[::-1]
bins = (freqs / sr * win).astype(int)
img = np.zeros((H + 60, W))
han = np.hanning(win)
for i in range(W):
    seg = x[i * hop:i * hop + win]
    sp = 20 * np.log10(np.abs(np.fft.rfft(seg * han)) + 1e-6)
    img[60:, i] = np.clip((sp[bins] + 20) / 60, 0, 1)
    pk = min(1.0, np.max(np.abs(seg[:hop])))
    h = int(pk * 58); img[59 - h:60, i] = 1
rgb = (np.stack([img ** 0.7, img ** 1.5, img * 0.4 + img ** 3 * 0.6], -1) * 255).astype(np.uint8)
raw = b"".join(b"\0" + row.tobytes() for row in rgb)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H + 60, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
