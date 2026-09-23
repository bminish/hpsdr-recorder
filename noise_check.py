#!/usr/bin/env python3
"""Assess a recording's noise floor, and whether the sample shift is costing anything.

Usage: noise_check.py <file.raw> [shift_used]        (shift defaults to 8)

The question a sample shift raises is not "how many bits are we using" but
"is the band noise still comfortably above the quantisation floor". If the
received noise is many LSBs, quantisation is well dithered and contributes
nothing; the discarded low bits held no information. If the band noise
approaches one LSB, quantisation is destroying weak-signal detail and a
smaller shift would genuinely buy something.

Both the band noise and the quantisation noise scale identically with any
later narrowband processing gain, so the wideband ratio is the right measure
and applies equally to a 1 Hz carrier analysis.
"""

import os
import sys
import struct

import numpy as np

FULL_SCALE = 32767.0
Q_RMS = 1.0 / np.sqrt(12.0)          # uniform quantisation noise, in LSBs


def read_header(path):
    with open(path, 'rb') as fh:
        h = struct.unpack('<idd5iB', fh.read(41))
    return {'centre': h[2] * 1e6, 'fs': h[7], 'nch': 4 if h[6] == 4 else 2}


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(1)
    path = sys.argv[1]
    shift = int(sys.argv[2]) if len(sys.argv) > 2 else 8

    hdr = read_header(path)
    fs, nch, centre = hdr['fs'], hdr['nch'], hdr['centre']
    frame = nch * 2
    total = (os.path.getsize(path) - 41) // frame
    dur = total / fs

    print(f"{path}")
    print(f"  centre {centre/1000:.0f} kHz, {fs} Hz, {dur/60:.1f} min, "
          f"sample shift {shift}\n")

    # Sample windows spread across the whole recording
    windows = 12
    secs = 2
    rms_ch, peak_ch = [[], []], [0, 0]
    psd_acc = None
    nfft = 1 << 16
    for w in range(windows):
        off = int(dur * w / windows)
        with open(path, 'rb') as fh:
            fh.seek(41 + frame * int(off * fs))
            raw = np.frombuffer(fh.read(int(secs * fs) * frame), dtype='<i2')
        if raw.size < nfft * nch:
            continue
        x = raw.reshape(-1, nch)
        for c, (i, q) in enumerate(((0, 1), (2, 3))[:nch // 2]):
            z = x[:, i].astype(np.float64) + 1j * x[:, q].astype(np.float64)
            rms_ch[c].append(np.sqrt(np.mean(np.abs(z) ** 2)))
            peak_ch[c] = max(peak_ch[c], float(np.max(np.abs(z))))
            if c == 0:
                segs = len(z) // nfft
                acc = np.zeros(nfft)
                w_win = np.hanning(nfft)
                for k in range(segs):
                    acc += np.abs(np.fft.fft(z[k*nfft:(k+1)*nfft] * w_win)) ** 2
                acc /= max(segs, 1)
                psd_acc = acc if psd_acc is None else psd_acc + acc

    for c in range(nch // 2):
        if not rms_ch[c]:
            continue
        rms = float(np.mean(rms_ch[c]))
        print(f"  channel {c+1}")
        print(f"     wideband RMS   : {rms:.1f} LSB  ({20*np.log10(rms/FULL_SCALE):.1f} dBFS)")
        print(f"     peak           : {20*np.log10(max(peak_ch[c],1)/FULL_SCALE):.1f} dBFS")
        # How far the quantisation floor sits below the received noise
        margin = 20 * np.log10(rms / Q_RMS)
        penalty = 10 * np.log10(1 + 10 ** (-margin / 10))
        print(f"     quantisation   : {margin:.1f} dB below the band noise")
        print(f"     noise added by quantisation: {penalty:.4f} dB")
        print()

    # Spectral floor, from the quietest bins (carriers excluded by using a low
    # percentile rather than the median)
    if psd_acc is not None:
        fr = np.fft.fftfreq(nfft, 1 / fs)
        psd = psd_acc / windows
        # normalise to dBFS/Hz: window gain and FFT length
        wsum = np.sum(np.hanning(nfft) ** 2)
        psd_hz = psd / (wsum * fs) / (FULL_SCALE ** 2)
        inband = np.abs(fr) < 0.45 * fs
        floor_db = 10 * np.log10(np.percentile(psd_hz[inband], 10))
        q_psd_db = 10 * np.log10((Q_RMS ** 2 / FULL_SCALE ** 2) / fs)
        print(f"  spectral noise floor (10th pct) : {floor_db:.1f} dBFS/Hz")
        print(f"  16-bit quantisation floor       : {q_psd_db:.1f} dBFS/Hz")
        print(f"  margin                          : {floor_db - q_psd_db:.1f} dB\n")

        print("  --- what the sample shift is worth ---")
        rms = float(np.mean(rms_ch[0]))
        for s in range(shift, max(shift - 5, -1), -1):
            scale = 2.0 ** (shift - s)
            m = 20 * np.log10(rms * scale / Q_RMS)
            head = 20 * np.log10(FULL_SCALE / (max(peak_ch[0], 1) * scale))
            note = ""
            if head < 0:
                note = "  CLIPS"
            elif head < 20:
                note = "  thin headroom for impulses"
            print(f"     shift {s}: quantisation {m:5.1f} dB below noise, "
                  f"headroom {head:5.1f} dB{note}")


if __name__ == '__main__':
    main()
