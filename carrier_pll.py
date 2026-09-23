#!/usr/bin/env python3
"""Measure the average frequency of broadcast carriers in a Linrad .raw recording.

Usage: carrier_pll.py [--ppm P] <file.raw> [freq_kHz ...]     (default: 909 693)

  --ppm P   correct a known receiver clock error of P ppm. A clock error is a
            scale error, so it must be applied as one: true = measured*(1-P/1e6).
            Do NOT try to correct this with the recorder's freq_correction,
            which is a constant offset in whole Hz and cannot cancel a scale.

For each carrier it downconverts, decimates to 1 kHz, and runs a second-order
PLL. The average frequency is taken from the PLL's total phase advance over the
recording, which is exact to well under a millihertz for any usable duration:
an average over T seconds needs only 0.001*T cycles of phase accuracy.

Zero-filled samples (inserted where UDP packets were lost) carry no phase, so
the loop coasts through them rather than being dragged toward zero.

Measuring two carriers at once separates transmitter error from receiver clock
error: a receiver reference that is off by N ppm shifts every carrier by the
same *fractional* amount, so a common ppm offset is the radio, not the stations.
"""

import os
import sys
import struct

import numpy as np
from scipy import signal

DEC_STAGES = (32, 6, 8)          # 1536000 -> 1000 Hz
LOOP_BW_HZ = 0.5                 # PLL noise bandwidth
DAMPING = 0.707
SETTLE_S = 20.0                  # let the loop acquire before measuring
SEGMENT_S = 300.0                # report stability over 5-minute segments
ZERO_RUN_MIN = 64                # consecutive zeros that indicate real zero-fill


def read_header(path):
    with open(path, 'rb') as fh:
        h = struct.unpack('<idd5iB', fh.read(41))
    return {'centre': h[2] * 1e6, 'fs': h[7],
            'nch': 4 if h[6] == 4 else 2, 'ts': h[1]}


def make_decimators(fs):
    """Stage 1 is a boxcar, stages 2-3 are FIRs carrying state between blocks.

    A boxcar of length D has exact nulls at every multiple of the output rate -
    precisely the frequencies that would alias onto DC - so it is the right
    cheap filter for the first, highest-rate stage. It is ~30x faster than a
    101-tap FIR there, which matters on a multi-gigabyte file. The narrow
    filtering that rejects the adjacent 9 kHz channels is done by the later
    stages, while the rate is still high enough that nothing has folded in.
    """
    stages = []
    rate = fs
    for i, d in enumerate(DEC_STAGES):
        if i == 0:
            stages.append({'d': d, 'boxcar': True})
        else:
            cutoff = 0.40 * (rate / d)
            taps = signal.firwin(101, cutoff / (rate / 2))
            stages.append({'d': d, 'boxcar': False, 'b': taps,
                           'zi': np.zeros(len(taps) - 1, dtype=complex)})
        rate //= d
    return stages, rate


def decimate(stages, x):
    for st in stages:
        d = st['d']
        if st['boxcar']:
            n = (len(x) // d) * d          # blocks are aligned, so nothing is lost
            x = x[:n].reshape(-1, d).mean(axis=1)
        else:
            x, st['zi'] = signal.lfilter(st['b'], [1.0], x, zi=st['zi'])
            x = x[::d]
    return x


class PLL:
    """Second-order (type II) loop. Phase is accumulated unwrapped."""

    def __init__(self, fs):
        wn = 2 * np.pi * LOOP_BW_HZ
        t = 1.0 / fs
        self.kp = 2 * DAMPING * wn * t
        self.ki = (wn * t) ** 2
        self.phase = 0.0        # NCO phase, radians, unwrapped
        self.freq = 0.0         # radians per sample
        self.locked = 0
        self.coasted = 0
        self.total = 0
        # The average must not include the acquisition transient, so phase is
        # measured from a mark set once the loop has settled.
        self.settle_at = int(SETTLE_S * fs)
        self.mark_phase = None
        self.mark_n = 0
        self.seg_n = int(SEGMENT_S * fs)
        self.marks = []          # (sample_index, phase) at each segment edge

    def run(self, x, gate):
        """gate[i] False -> zero-filled sample: hold the loop, keep advancing."""
        phase, freq, kp, ki = self.phase, self.freq, self.kp, self.ki
        for i in range(len(x)):
            phase += freq
            self.total += 1
            if self.mark_phase is None and self.total >= self.settle_at:
                self.mark_phase = phase
                self.mark_n = self.total
                self.marks.append((self.total, phase))
            elif self.mark_phase is not None and \
                    self.total - self.marks[-1][0] >= self.seg_n:
                self.marks.append((self.total, phase))
            if not gate[i]:
                self.coasted += 1
                continue
            err = np.angle(x[i] * np.exp(-1j * phase))
            phase += kp * err
            freq += ki * err
            if abs(err) < 0.3:
                self.locked += 1
        self.phase, self.freq = phase, freq


class Carrier:
    """Per-carrier state for a single shared pass over the file."""

    def __init__(self, target_hz, hdr, block):
        self.target = target_hz
        self.offset = target_hz - hdr['centre']
        # The mixer table is precomputed once for a whole block. Every block
        # starts at the same mixer phase (the offsets are multiples of 1 kHz and
        # the block is a multiple of 1536 samples, so a whole number of cycles
        # fits in each block), which removes all transcendental work from the
        # inner loop.
        n = np.arange(block, dtype=np.float64)
        self.mix = np.exp(-2j * np.pi * self.offset * n / hdr['fs']).astype(np.complex64)
        self.stages, self.dec_fs = make_decimators(hdr['fs'])
        self.pll = PLL(self.dec_fs)


def measure_all(path, hdr, targets, max_seconds=None):
    """Measure every carrier in one pass. Reading a 44 GB file once instead of
    once per carrier is the difference between minutes and half an hour."""
    fs, nch, centre = hdr['fs'], hdr['nch'], hdr['centre']
    frame = nch * 2

    block = 1536 * 1365                       # multiple of the decimation product
    usable = [f for f in targets if abs(f - centre) <= 0.45 * fs]
    carriers = [Carrier(f, hdr, block) for f in usable]
    if not carriers:
        return []

    total_samples = (os.path.getsize(path) - 41) // frame
    if max_seconds:
        total_samples = min(total_samples, int(max_seconds * fs))

    n0 = 0
    zero_run = 0
    noise = []                                 # per-block wideband level
    while n0 < total_samples:
        with open(path, 'rb') as fh:
            fh.seek(41 + n0 * frame)
            want = int(min(block, total_samples - n0))
            raw = np.frombuffer(fh.read(want * frame), dtype='<i2')
        if raw.size < want * nch:
            want = raw.size // nch
            if want == 0:
                break
            raw = raw[:want * nch]
        x = raw.reshape(-1, nch)
        sig = x[:, 0].astype(np.float32) + 1j * x[:, 1].astype(np.float32)

        # Zero-fill shows up as runs; isolated (0,0) samples are just the
        # signal crossing zero and there are millions of those at low levels.
        zeros = (x[:, 0] == 0) & (x[:, 1] == 0)
        if zeros.any():
            edges = np.flatnonzero(np.diff(np.concatenate(([0], zeros.view(np.int8), [0]))))
            runs = edges[1::2] - edges[0::2]
            zero_run += int(runs[runs >= ZERO_RUN_MIN].sum())

        noise.append(float(np.sqrt(np.mean(np.abs(sig) ** 2))))

        for c in carriers:
            d = decimate(c.stages, sig * c.mix[:want])
            mag = np.abs(d)
            ref = np.median(mag[mag > 0]) if np.any(mag > 0) else 0.0
            gate = mag > 0.05 * ref if ref > 0 else np.zeros(len(d), bool)
            c.pll.run(d, gate)

        n0 += want

    results = []
    for c in carriers:
        pll = c.pll
        if pll.total == 0 or pll.locked == 0 or pll.mark_phase is None:
            results.append({'target': c.target, 'failed': True})
            continue
        span_n = pll.total - pll.mark_n
        if span_n <= 0:
            results.append({'target': c.target, 'failed': True})
            continue
        seconds = span_n / c.dec_fs
        delta_f = (pll.phase - pll.mark_phase) / (2 * np.pi * seconds)
        results.append({
            'target': c.target,
            'measured': c.target + delta_f,
            'delta': delta_f,
            'seconds': seconds,
            'lock_pct': 100.0 * pll.locked / pll.total,
            'coast_pct': 100.0 * pll.coasted / pll.total,
            'zero_samples': zero_run,
            'failed': False,
            'segments': [
                (pll.marks[i + 1][1] - pll.marks[i][1]) /
                (2 * np.pi * (pll.marks[i + 1][0] - pll.marks[i][0]) / c.dec_fs)
                for i in range(len(pll.marks) - 1)
            ],
        })
    return results


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(1)
    args = sys.argv[1:]
    ppm = 0.0
    if '--ppm' in args:
        i = args.index('--ppm')
        ppm = float(args[i + 1])
        del args[i:i + 2]
    path = args[0]
    freqs = [float(a) * 1000.0 for a in args[1:]] or [909000.0, 693000.0]

    hdr = read_header(path)
    print(f"{path}")
    print(f"  centre {hdr['centre']/1000:.3f} kHz, {hdr['fs']} Hz, "
          f"{hdr['nch']} AD channels\n")

    results = [r for r in measure_all(path, hdr, freqs) if not r.get('failed')]
    for r in results:
        f = r['target']
        print(f"  {f/1000:.0f} kHz")
        print(f"     average frequency : {r['measured']:.3f} Hz "
              f"({r['delta']:+.3f} Hz from nominal)")
        if ppm:
            corr = r['measured'] * (1.0 - ppm * 1e-6)
            print(f"     clock-corrected   : {corr:.3f} Hz "
                  f"({corr - r['target']:+.3f} Hz from nominal, {ppm:+.3f} ppm applied)")
        print(f"     over              : {r['seconds']:.1f} s")
        print(f"     PLL locked        : {r['lock_pct']:.1f} %  "
              f"(coasted {r['coast_pct']:.2f} % on zero-fill/fades)")
        print(f"     zero-filled input : {r['zero_samples']} samples "
              f"(runs of >= {ZERO_RUN_MIN})")
        segs = r['segments']
        if len(segs) >= 2:
            sd = float(np.std(segs))
            print(f"     stability         : {len(segs)} x {SEGMENT_S:.0f}s segments, "
                  f"sd {sd*1000:.1f} mHz")
            print("     segment averages  : " +
                  ", ".join(f"{r['target'] + d:.3f}" for d in segs[:6]) +
                  (" ..." if len(segs) > 6 else ""))
        print()

    if len(results) >= 2:
        print("  --- common-mode check ---")
        for r in results:
            print(f"     {r['target']/1000:.0f} kHz: "
                  f"{1e6 * r['delta'] / r['target']:+.3f} ppm")
        spread = [1e6 * r['delta'] / r['target'] for r in results]
        print(f"     spread: {max(spread) - min(spread):.3f} ppm")
        print("     A common offset is the receiver's reference; a differing one\n"
              "     is the transmitters themselves.")


if __name__ == '__main__':
    main()
