#!/usr/bin/env python3
"""Report a Linrad .raw recording's tuning error against the broadcast carrier grid.

Usage: check_grid.py <file.raw> [grid_khz] [skip_seconds]

  grid_khz       channel spacing, 9 (Europe, default) or 10 (North America)
  skip_seconds   how far into the file to analyse (default 30, clamped to fit)

Strong carriers should land within a few Hz of the grid. A constant offset
across every carrier means spectral inversion, not mistuning - see README.md.
The peak at exactly 0 Hz offset is the DC/LO spur, not a station.
"""
import sys, struct
import numpy as np

if len(sys.argv) < 2:
    print(__doc__.strip(), file=sys.stderr)
    sys.exit(1)

path = sys.argv[1]
grid = int(sys.argv[2]) * 1000 if len(sys.argv) > 2 else 9000

hdr = struct.unpack('<idd5iB', open(path, 'rb').read(41))
center, fs, ad = hdr[2] * 1e6, hdr[7], hdr[6]
nch = 4 if ad == 4 else 2
nfft, nseg = fs, 8                       # 1 Hz bins

import os
frame = nch * 2
total = (os.path.getsize(path) - 41) // frame          # samples available
need = nfft * nseg
skip = int(sys.argv[3]) * fs if len(sys.argv) > 3 else fs * 30
skip = max(0, min(skip, total - need))                 # clamp to what the file holds
if total < need:
    nseg = max(1, total // nfft); need = nfft * nseg; skip = 0
print(f"(analysing {nseg} x 1 s from t={skip/fs:.1f}s; file holds {total/fs:.1f}s)")
with open(path, 'rb') as fh:
    fh.seek(41 + frame * skip)                         # keep sample alignment
    raw = np.frombuffer(fh.read(need * frame), dtype='<i2')

x = raw.reshape(-1, nch)
ch = x[:, 0].astype(np.float32) + 1j * x[:, 1].astype(np.float32)
nseg = min(nseg, len(ch) // nfft)
w = np.hanning(nfft)
acc = sum(np.abs(np.fft.fft(ch[k*nfft:(k+1)*nfft] * w))**2 for k in range(nseg))
psd = 10 * np.log10(acc / nseg + 1e-9)
fr = np.fft.fftfreq(nfft, 1 / fs)
m = np.abs(fr) <= 60000
fr, pw = fr[m], psd[m]
o = np.argsort(fr); fr, pw = fr[o], pw[o]

print(f"{path}: header centre {center/1000:.3f} kHz, rate {fs}, {nch} AD channels")
peaks, pw2 = [], pw.copy()
for _ in range(14):
    i = int(np.argmax(pw2)); peaks.append((fr[i], pw[i])); pw2[max(0, i-400):i+400] = -300

errs = []
for off, db in sorted(peaks):
    absf = center + off
    g = round(absf / grid) * grid
    err = absf - g
    tag = ''
    if abs(err) < grid / 6:
        errs.append(err); tag = ' *'
    print(f"  {off:+9.1f} Hz -> {absf/1000:9.3f} kHz  {db:6.1f} dB   grid {g/1000:6.0f}  err {err:+8.1f} Hz{tag}")

if errs:
    med = float(np.median(errs))
    print(f"\n  median tuning error: {med:+.1f} Hz   (actual NCO = {center - med:.0f} Hz "
          f"for a commanded {center:.0f} Hz)")
    print("  VERDICT: tuning correct" if abs(med) < 50 else
          f"  VERDICT: off by {med:+.0f} Hz - adjust freq_correction by {-med:+.0f}")
