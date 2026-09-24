#!/usr/bin/env python3
"""Compare several antennas across recordings that share a common reference arm.

Usage: antenna_survey.py LABEL=file.raw [LABEL=file.raw ...]

Each recording contributes ch1 (the switchable antenna, named by LABEL) and ch2
(the fixed reference antenna, the same one in every recording).

Why it is done this way: two antennas on two ADCs can be compared directly, but
a third cannot — you can only ever hear two at once. Comparing ch1 across two
recordings taken at different times would be confounded by propagation, which
changes between them. Referring each switchable antenna to the *common* arm and
then differencing cancels most of that, because the reference heard the same
band in each run.

The reference arm is also the experiment's control: its own noise floor should
not change between recordings, since nothing about it changed. However much it
does move bounds the confidence of every other number here.
"""

import sys

import numpy as np

import arm_compare as ac

BANDS = [(500e3, 700e3), (700e3, 900e3), (900e3, 1100e3),
         (1100e3, 1300e3), (1300e3, 1500e3), (1500e3, 1700e3)]
WINDOWS = 60


def analyse(path):
    hdr = ac.read_header(path)
    freqs, p11, p22, p12, used = ac.averaged_spectra(path, hdr, WINDOWS)
    out = {'floors': [], 'gains': [], 'nd': []}
    rows = []
    f = 531000.0
    while f <= 1602000.0:
        st = ac.carrier_stats(freqs, p11, p22, p12, f, hdr['fs'] / ac.NFFT)
        if st and st['snr1'] >= 10 and st['snr2'] >= 10:
            rows.append(st)
        f += 9000
    for lo, hi in BANDS:
        f1 = ac.band_floor(freqs, p11, lo, hi)
        f2 = ac.band_floor(freqs, p22, lo, hi)
        inb = [r for r in rows if lo <= r['f'] < hi]
        g = float(np.median([r['ratio'] for r in inb])) if inb else float('nan')
        out['floors'].append((ac.db(f1), ac.db(f2)))
        out['gains'].append(g)
        out['nd'].append(ac.db(f1) - ac.db(f2))
    out['median_gain'] = float(np.median([r['ratio'] for r in rows])) if rows else float('nan')
    out['n'] = len(rows)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(1)

    runs = []
    for arg in sys.argv[1:]:
        label, _, path = arg.partition('=')
        if not path:
            print(f"expected LABEL=file.raw, got {arg}", file=sys.stderr)
            sys.exit(1)
        runs.append((label, path, analyse(path)))

    # ---- the control: the reference arm should read the same every time ----
    print("\n=== reference arm (ch2) as control ===")
    print("  Its noise floor should not change between runs. Whatever it does")
    print("  move is the uncertainty floor for everything below.\n")
    print(f"  {'band kHz':>14} " + " ".join(f"{lab:>9}" for lab, _, _ in runs) + f"{'spread':>9}")
    ref_spreads = []
    for i, (lo, hi) in enumerate(BANDS):
        vals = [r[2]['floors'][i][1] for r in runs]
        spread = max(vals) - min(vals)
        ref_spreads.append(spread)
        print(f"  {lo/1000:6.0f}-{hi/1000:<7.0f} " +
              " ".join(f"{v:9.1f}" for v in vals) + f"{spread:9.1f}")
    print(f"\n  worst reference drift: {max(ref_spreads):.1f} dB")

    # ---- each antenna's own noise floor ----
    print("\n=== absolute noise floor of each antenna (same scale throughout) ===")
    print(f"  {'band kHz':>14} " + " ".join(f"{lab:>9}" for lab, _, _ in runs) + f"{'  reference':>11}")
    for i, (lo, hi) in enumerate(BANDS):
        ref = np.mean([r[2]['floors'][i][1] for r in runs])
        print(f"  {lo/1000:6.0f}-{hi/1000:<7.0f} " +
              " ".join(f"{r[2]['floors'][i][0]:9.1f}" for r in runs) + f"{ref:11.1f}")

    # ---- gain relative to the common reference ----
    print("\n=== gain relative to the reference arm (median over carriers) ===")
    for lab, _, res in runs:
        print(f"  {lab:>10}: {res['median_gain']:+7.2f} dB   ({res['n']} carriers)")

    # ---- is each antenna hearing itself, or the receiver? ----
    print("\n=== what limits each antenna, band by band ===")
    print("  gap = (noise floor vs reference) - (gain vs reference).")
    print("  Near zero: both arms are limited by the same external noise.")
    print("  Negative : the REFERENCE arm carries that much EXCESS noise for its")
    print("             signal -- equivalently, this antenna has that much better")
    print("             signal-to-noise ratio.")
    print("  Positive : this antenna carries that much excess noise instead.")
    print("  The gap does not say whether excess noise is the receiver's or the")
    print("  antenna's. Receiver noise is invariant to band conditions; antenna")
    print("  noise is not. Compare day against night to tell them apart.\n")
    for lab, _, res in runs:
        print(f"  {lab}")
        for i, (lo, hi) in enumerate(BANDS):
            g, nd = res['gains'][i], res['nd'][i]
            if np.isnan(g):
                continue
            gap = nd - g
            if abs(gap) < 1.5:
                note = "matched: same external noise limit"
            elif gap > 0:
                note = f"{lab} noisier by {gap:.1f} dB for its signal"
            else:
                note = f"{lab} has {-gap:.1f} dB BETTER SNR than reference"
            print(f"     {lo/1000:6.0f}-{hi/1000:<6.0f} gain {g:+7.2f}  noise {nd:+7.2f}  "
                  f"gap {gap:+6.2f}   {note}")

    # ---- derived comparison between the switchable antennas ----
    if len(runs) >= 2:
        print("\n=== derived: switchable antennas against each other ===")
        print("  Each is referred to the common arm, so the difference is largely")
        print("  free of the propagation change between recordings.\n")
        base = runs[0]
        for lab, _, res in runs[1:]:
            d = res['median_gain'] - base[2]['median_gain']
            print(f"  {lab} - {base[0]}: {d:+.2f} dB")
        same = {}
        for lab, _, res in runs:
            same.setdefault(lab, []).append(res['median_gain'])
        for lab, vals in same.items():
            if len(vals) > 1:
                print(f"  repeatability of {lab}: {max(vals)-min(vals):.2f} dB "
                      f"between its own runs -- treat smaller differences as noise")


if __name__ == '__main__':
    main()
