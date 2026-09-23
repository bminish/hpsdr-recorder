#!/usr/bin/env python3
"""Compare the two diversity arms of a recording: gain match, noise floor, phase.

Usage: arm_compare.py <file.raw> [options]

  --windows N       time windows spread through the file (default 60)
  --quiet LO-HI     clean band for the noise-floor comparison, kHz (default 1400-1460)
  --carriers LO-HI  broadcast carrier range, kHz (default 531-1602)
  --min-snr DB      carrier SNR needed to be counted (default 10)

ch1 is ADC0, fed from whichever jack the recorder's `antenna` setting selected;
ch2 is ADC1, which has its own feed and is unaffected by that setting. Check the
recorder log for which jack was in use, and relabel the arms accordingly.

Two independent estimates of the gain difference are produced, because each has
a different weakness:

  Noise floor  - reflects what each arm actually delivers, but is contaminated
                 by local man-made noise, which is why the reference band is
                 chosen to be clean and why every band is reported separately.
  Carriers     - immune to a raised noise floor, but individual carriers fade
                 independently on spatially separated antennas, so only the
                 median over many carriers is meaningful.

Agreement between the two is the evidence that either can be trusted.
"""

import os
import sys
import struct

import numpy as np

NFFT = 1 << 17                 # ~11.7 Hz bins at 1536 kHz
SEGS_PER_WINDOW = 8


def read_header(path):
    with open(path, 'rb') as fh:
        h = struct.unpack('<idd5iB', fh.read(41))
    return {'centre': h[2] * 1e6, 'fs': h[7], 'nch': 4 if h[6] == 4 else 2,
            'timestamp': h[1]}


def averaged_spectra(path, hdr, windows):
    """Averaged auto- and cross-spectra of both arms, from identical windows.

    Using the same windows for both is what makes the comparison valid: any
    fading common to the two arms cancels in the ratio.
    """
    fs, nch, frame = hdr['fs'], hdr['nch'], hdr['nch'] * 2
    total = (os.path.getsize(path) - 41) // frame
    need = NFFT * SEGS_PER_WINDOW
    if total < need:
        raise SystemExit("recording too short")

    p11 = np.zeros(NFFT)
    p22 = np.zeros(NFFT)
    p12 = np.zeros(NFFT, dtype=complex)
    win = np.hanning(NFFT)
    used = 0

    for w in range(windows):
        start = int((total - need) * w / max(windows - 1, 1))
        with open(path, 'rb') as fh:
            fh.seek(41 + start * frame)
            raw = np.frombuffer(fh.read(need * frame), dtype='<i2')
        if raw.size < need * nch:
            continue
        x = raw.reshape(-1, nch)
        a = x[:, 0].astype(np.float64) + 1j * x[:, 1].astype(np.float64)
        b = x[:, 2].astype(np.float64) + 1j * x[:, 3].astype(np.float64)
        for k in range(SEGS_PER_WINDOW):
            sl = slice(k * NFFT, (k + 1) * NFFT)
            A = np.fft.fft(a[sl] * win)
            B = np.fft.fft(b[sl] * win)
            p11 += np.abs(A) ** 2
            p22 += np.abs(B) ** 2
            p12 += A * np.conj(B)
        used += 1

    n = max(used * SEGS_PER_WINDOW, 1)
    freqs = hdr['centre'] + np.fft.fftfreq(NFFT, 1 / fs)
    order = np.argsort(freqs)
    return (freqs[order], p11[order] / n, p22[order] / n, p12[order] / n, used)


def db(x):
    return 10 * np.log10(np.maximum(x, 1e-30))


def band_floor(freqs, psd, lo, hi, pct=10):
    """Noise floor as a low percentile, so carriers in the band do not count."""
    m = (freqs >= lo) & (freqs <= hi)
    if not np.any(m):
        return None
    return float(np.percentile(psd[m], pct))


def carrier_stats(freqs, p11, p22, p12, f0, bin_hz):
    """Carrier power in each arm, plus local noise, coherence and phase."""
    m = (freqs >= f0 - 60) & (freqs <= f0 + 60)
    if not np.any(m):
        return None
    idx = np.flatnonzero(m)
    pk = idx[np.argmax(p11[m] + p22[m])]
    sl = slice(max(pk - 1, 0), pk + 2)            # carrier bin +/- 1
    c1, c2 = float(np.sum(p11[sl])), float(np.sum(p22[sl]))

    # Local noise sampled clear of the AM sidebands but inside the channel
    side = ((np.abs(freqs - f0) > 5000) & (np.abs(freqs - f0) < 8000))
    if not np.any(side):
        return None
    n1 = float(np.median(p11[side])) * 3
    n2 = float(np.median(p22[side])) * 3

    coh = (abs(np.sum(p12[sl])) ** 2) / max(c1 * c2, 1e-30)
    phase = np.angle(np.sum(p12[sl]), deg=True)
    return {'f': f0, 'c1': c1, 'c2': c2,
            'snr1': db(c1 / max(n1, 1e-30)), 'snr2': db(c2 / max(n2, 1e-30)),
            'ratio': db(c1) - db(c2), 'coh': coh, 'phase': phase}


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(1)

    def opt(name, default):
        if name in args:
            i = args.index(name)
            v = args[i + 1]
            del args[i:i + 2]
            return v
        return default

    windows = int(opt('--windows', 60))
    quiet = opt('--quiet', '1400-1460')
    crange = opt('--carriers', '531-1602')
    min_snr = float(opt('--min-snr', 10))
    path = args[0]

    qlo, qhi = [float(v) * 1000 for v in quiet.split('-')]
    clo, chi = [float(v) * 1000 for v in crange.split('-')]

    hdr = read_header(path)
    freqs, p11, p22, p12, used = averaged_spectra(path, hdr, windows)
    bin_hz = hdr['fs'] / NFFT
    dur = (os.path.getsize(path) - 41) / (hdr['nch'] * 2) / hdr['fs']

    print(f"{path}")
    print(f"  centre {hdr['centre']/1000:.0f} kHz, {dur/60:.1f} min, "
          f"{used} windows x {SEGS_PER_WINDOW} segments, {bin_hz:.1f} Hz bins")
    print("  ch1 = ADC0 (selected antenna jack), ch2 = ADC1 (its own feed)\n")

    # ---- noise floor, band by band ----
    print("  --- noise floor by band (10th percentile of bins) ---")
    print(f"  {'band kHz':>14} {'ch1 dB':>9} {'ch2 dB':>9} {'ch1-ch2':>9}")
    edges = [(500e3, 700e3), (700e3, 900e3), (900e3, 1100e3), (1100e3, 1300e3),
             (1300e3, 1500e3), (1500e3, 1700e3), (qlo, qhi)]
    quiet_delta = None
    for lo, hi in edges:
        f1 = band_floor(freqs, p11, lo, hi)
        f2 = band_floor(freqs, p22, lo, hi)
        if f1 is None or f2 is None:
            continue
        d = db(f1) - db(f2)
        tag = "   <- reference band" if (lo, hi) == (qlo, qhi) else ""
        if tag:
            quiet_delta = d
        print(f"  {lo/1000:6.0f}-{hi/1000:<7.0f} {db(f1):9.1f} {db(f2):9.1f} "
              f"{d:+9.2f}{tag}")

    # ---- broadcast carriers ----
    print(f"\n  --- broadcast carriers {clo/1000:.0f}-{chi/1000:.0f} kHz "
          f"(9 kHz grid, SNR >= {min_snr:.0f} dB both arms) ---")
    rows = []
    f = clo
    while f <= chi:
        st = carrier_stats(freqs, p11, p22, p12, f, bin_hz)
        if st and st['snr1'] >= min_snr and st['snr2'] >= min_snr:
            rows.append(st)
        f += 9000

    if not rows:
        print("     no carriers met the SNR threshold")
        return

    print(f"  {'kHz':>6} {'ch1 dB':>8} {'ch2 dB':>8} {'ch1-ch2':>9} "
          f"{'SNR1':>6} {'SNR2':>6} {'coh':>6} {'phase':>7}")
    for r in rows:
        print(f"  {r['f']/1000:6.0f} {db(r['c1']):8.1f} {db(r['c2']):8.1f} "
              f"{r['ratio']:+9.2f} {r['snr1']:6.1f} {r['snr2']:6.1f} "
              f"{r['coh']:6.2f} {r['phase']:+7.1f}")

    ratios = np.array([r['ratio'] for r in rows])
    fmhz = np.array([r['f'] for r in rows]) / 1e6
    med = float(np.median(ratios))
    q1, q3 = np.percentile(ratios, [25, 75])
    slope = float(np.polyfit(fmhz, ratios, 1)[0]) if len(rows) >= 3 else float('nan')

    print(f"\n  carriers used     : {len(rows)}")
    print(f"  median ch1 - ch2  : {med:+.2f} dB   (IQR {q1:+.2f} to {q3:+.2f})")
    print(f"  spread (sd)       : {float(np.std(ratios)):.2f} dB")
    print(f"  linear trend      : {slope:+.2f} dB per MHz "
          f"(only meaningful if the band table below is monotonic)")

    # Phase slope across frequency is a time delay between the arms. Only
    # carriers the two arms actually share (high coherence) can contribute.
    good = [r for r in rows if r['coh'] > 0.8]
    if len(good) >= 4:
        gf = np.array([r['f'] for r in good])
        gp = np.unwrap(np.radians([r['phase'] for r in good]))
        fit, res = np.polyfit(gf, gp, 1, full=True)[:2]
        delay_us = -fit[0] / (2 * np.pi) * 1e6
        resid = float(np.sqrt(res[0] / len(good))) if len(res) else float('nan')
        rdeg = np.degrees(resid)
        if rdeg < 30:
            print(f"  phase slope       : {delay_us:+.2f} us of delay between arms "
                  f"({len(good)} coherent carriers, residual {rdeg:.0f} deg)")
        else:
            print(f"  phase slope       : no consistent delay (residual {rdeg:.0f} deg over "
                  f"{len(good)} carriers) -- the arms see independently fading paths, "
                  f"which is what spatial diversity is for")

    print("\n  --- by band: gain (carriers) vs noise floor ---")
    print("  If an arm's antenna noise dominates its own receiver noise, its")
    print("  noise floor sits above the other arm by the SAME amount as its gain.")
    print("  A gap between the two says which effect is in play.\n")
    print(f"  {'band kHz':>14} {'n':>3} {'gain':>8} {'noise':>8} {'gap':>8}   diagnosis")
    for lo, hi in edges[:-1]:
        inb = [r for r in rows if lo <= r['f'] < hi]
        f1 = band_floor(freqs, p11, lo, hi)
        f2 = band_floor(freqs, p22, lo, hi)
        if not inb or f1 is None or f2 is None:
            continue
        g = float(np.median([r['ratio'] for r in inb]))
        nd = db(f1) - db(f2)
        gap = nd - g
        if abs(gap) < 1.5:
            note = "both arms antenna-noise-limited"
        elif gap > 0:
            note = "ch1 has %.1f dB excess noise (man-made?)" % gap
        else:
            note = "ch2 is receiver-limited, losing %.1f dB" % (-gap)
        print(f"  {lo/1000:6.0f}-{hi/1000:<7.0f} {len(inb):3d} {g:+8.2f} "
              f"{nd:+8.2f} {gap:+8.2f}   {note}")

    print("\n  --- gain match ---")
    if quiet_delta is not None:
        print(f"  from noise floor in {qlo/1000:.0f}-{qhi/1000:.0f} kHz : {quiet_delta:+.2f} dB")
    print(f"  from {len(rows)} carriers (median)            : {med:+.2f} dB")
    if quiet_delta is not None:
        agree = abs(quiet_delta - med)
        verdict = ("the two estimates agree, so the figure is trustworthy"
                   if agree < 1.5 else
                   "the two disagree: suspect noise contamination in one arm, "
                   "or genuinely different frequency responses")
        print(f"  difference between methods          : {agree:.2f} dB")
        print(f"  -> {verdict}")
    spread_db = float(np.std(ratios))
    if spread_db > 3.0:
        print(f"  -> carrier-to-carrier spread is {spread_db:.1f} dB: on spatially "
              f"separated\n     antennas this is mostly independent fading, so trust "
              f"the median, not\n     any single carrier")


if __name__ == '__main__':
    main()
