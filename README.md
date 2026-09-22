# hpsdr-recorder

Records dual-ADC (diversity) IQ from an openHPSDR **Protocol 2** radio to
Linrad `.raw` files, for medium-wave DX analysis in wavviewdx.

Developed and verified against an **ANAN-100D (Angelia)**. Protocol details —
packet layouts, Alex filter bits, DDC sync — follow piHPSDR's implementation.

## Build

```sh
cmake . && make
```

Needs pthreads and libm. No other dependencies.

## Quick start

```sh
./hpsdr-recorder -c MW_dual.conf                 # record until Ctrl+C
./hpsdr-recorder -c MW_dual.conf -x 3600         # record one hour
./hpsdr-recorder -f 950000 -x 60 -o test.raw     # no config file
```

With no `-i`, the radio is auto-discovered by broadcast.

## Command line

| Option | Meaning | Default |
|---|---|---|
| `-c <file>` | Config file | – |
| `-i <IP>` | Radio IP address | auto-discover |
| `-r <rate>` | Sample rate (Hz) | 1536000 |
| `-f <freq>` | RX1 frequency (Hz) | 990000 |
| `-F <freq>` | RX2 frequency (Hz) | 990000 |
| `-d` | Enable diversity | on |
| `-o <file>` | Output file (template, see below) | `hpsdr_recording.raw` |
| `-x <secs>` | Recording length; 0 runs until Ctrl+C | 0 |
| `-a <db>` | ADC attenuation, 0–31 dB | 0 |
| `-A <ant>` | RX antenna: `ant1 ant2 ant3 ext1 ext2 xvtr` | `ant1` |
| `-s <bits>` | Sample shift, 0–8 (see below) | 8 |
| `-k <hz>` | DDC tuning correction — calibration only | 0 |

## Config file

`key = value`, `#` starts a comment. Unknown keys are reported rather than
silently ignored. Both spellings of each key are accepted.

| Key | Meaning |
|---|---|
| `ip`, `ip_address` | Radio address |
| `sample rate`, `sample_rate` | Sample rate in Hz |
| `frequency`, `freq` | Sets both receivers |
| `rx1_freq`, `rx2_freq` | Per-receiver frequency |
| `diversity` | `true`/`1` for dual-ADC recording |
| `attenuation`, `atten` | 0–31 dB, applied to **both** ADCs |
| `antenna`, `rx_antenna` | `ant1`…`xvtr` |
| `sample shift`, `sample_shift` | 0–8 |
| `streaming time`, `streaming_time`, `duration` | Seconds; 0 = until Ctrl+C |
| `output file`, `output_file` | Filename template |
| `output type`, `output_type` | Only `Linrad` is supported |
| `freq_correction` | Calibration offset in Hz; normally 0 |
| `new_pa_board`, `alex_new_pa_board` | 1 for Rev.24 PA board |

The output filename expands `{TIMESTAMP}` (UTC, `20260922T143000Z`) and
`{FREQKHZ}` (e.g. `950kHz`).

## Output format

A 41-byte **packed** Linrad header followed by interleaved 16-bit samples:

```c
int    remember_proprietary_chunk;   // -1
double timestamp;                    // Unix seconds
double passband_center;              // MHz
int    passband_direction;           // 1
int    rx_input_mode;                // 38 for diversity IQ
int    rx_rf_channels;               // 2 diversity, 1 otherwise
int    rx_ad_channels;               // 4 diversity, 2 otherwise
int    rx_ad_speed;                  // sample rate
unsigned char save_init_flag;        // 0
```

Samples follow as `I1 Q1 I2 Q2` per sample instant in diversity mode (`I Q`
otherwise). At 1536000 Hz diversity that is 12.288 MB/s.

## Spectral inversion — read this before chasing a tuning error

HPSDR delivers IQ whose spectrum is **inverted** relative to the convention
Linrad and wavviewdx assume for `passband_direction = 1`. This recorder
conjugates on output (`pack_q()` in `streaming.c`) to correct it.

This matters because an inverted spectrum does not look inverted on a
regularly-spaced broadcast band — it looks like a **constant frequency
offset**. Recorded inverted at centre `C`, a station at `f` reads as `2C - f`,
an apparent shift of `2 * (C mod 9000)` Hz on the 9 kHz grid:

| Centre | Apparent error |
|---|---|
| 990 kHz | **0 Hz** — the fault is completely invisible |
| 950 kHz | 1 kHz |
| 951 kHz | 2 kHz |

So a constant offset that changes when you retune, and vanishes at a centre
that is itself on the grid, is inversion — not mistuning. **Do not "fix" it by
adding an offset to the DDC tuning word.** The NCO tunes exactly at the phase
word; `freq_correction` exists only for calibration against a known carrier and
should stay at 0.

Verify with `check_grid.py` rather than by eye.

## check_grid.py

Reports a recording's tuning error against the broadcast carrier grid:

```sh
./check_grid.py recording.raw            # 9 kHz grid (Europe)
./check_grid.py recording.raw 10         # 10 kHz grid (North America)
./check_grid.py recording.raw 9 300      # skip 300 s into the file
```

It prints the strongest carriers, their error against the nearest channel, and
a verdict. Correct output puts strong carriers within a few Hz of the grid.
The peak at exactly 0 Hz offset is the DC/LO spur, not a station.

## Choosing sample shift and attenuation

The radio sends 24-bit samples; the file holds 16-bit, so `sample_shift` bits
are discarded. Shift 8 keeps the top 16 bits and **cannot clip**, but with
typical MW levels only ~10 of the 16 bits are exercised — the DDC's decimation
processing gain, where weak DX lives, is in the bits being dropped. Each bit
less is 6 dB more weak-signal range, at the cost of headroom.

Every run reports what it saw, so choose from measurement, not guesswork:

```
Peak level: -25.6 dBFS (24-bit full scale)
Sample shift used: 8  -- 5 would have fitted with 6 dB spare (+18 dB of weak-signal range)
Clipped values: none
```

Night-time levels rise substantially, so a shift chosen by day may clip after
dark — clipping is counted and reported, so check the exit summary.

Attenuation is rarely needed: measured peaks sit ~25–30 dB below full scale on
a typical MW antenna, and attenuation only costs SNR. It applies to **both**
ADCs deliberately; unequal attenuation would break the amplitude match the
diversity phasing analysis depends on.

## Exit diagnostics

```
total writes = 154897
full writes = 154897
partial writes = 0
average write elapsed = 0.000005058
max write elapsed = 0.001358945
```

Two distinct failure signatures, worth telling apart:

- **`LOST: … samples dropped`** — the ~11 second sample ring filled because the
  output stopped draining. The stream is only ~12 MB/s, so this is not disk
  throughput; look for a full volume, a hung mount, or a blocking write. A
  large `max write elapsed` points at the stall.
- **`FAILED WRITES: …`** — writes returned an error (a full volume, typically).
  This produces **no** ring overrun, because failing writes return instantly
  and keep the ring draining. Without this check the recording would appear to
  succeed while containing nothing.

## Notes and limitations

- Alex HPF/LPF selection is automatic from the RX frequency, matching
  piHPSDR's ladders. On this board the RX signal from ANT1/2/3 also passes
  through the TX low-pass filters, so the LPF is chosen for the receive
  frequency; EXT1/EXT2/XVTR bypass them.
- Antenna selection affects the **ADC0 path only**. ADC1 (channel 2) has its
  own feed and is unaffected.
- There is a DC/LO spur at exactly the centre frequency. Avoid centring on a
  channel you care about.
- `blocks_buffer_capacity` and `samples_buffer_capacity` are compile-time only.
- Only Linrad output is implemented.
