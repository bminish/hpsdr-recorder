# Tests

## test_zerofill.c

Verifies the guarantee that matters for a DX archive: when UDP packets are
lost, the gap is padded with exactly the right number of zeroed samples, so
every later sample still lands at its true instant and carrier phase continues
coherently across the gap.

It includes `streaming.c` directly to reach the static `process_packet()`, then
feeds it synthetic Protocol 2 packets carrying a known tone, deliberately
dropping some and replaying others out of order.

```sh
gcc -g -O1 -I.. -o test_zerofill test_zerofill.c ../config.c ../buffers.c \
    ../stats.c ../hpsdr-protocol2.c -lpthread -lm
./test_zerofill
```

A single sample of timing slip would show as a 23.4 degree phase error, so the
check has roughly 2000x margin over the failure it is looking for.

## test_adc_setup.c

Dumps the bytes that actually configure ADC1 (ch2) — step attenuator, dither,
random, DDC/ADC assignment and sync — by wrapping `sendto()` and decoding by
destination port, so it reports what goes on the wire rather than what the
code appears to say.

```sh
gcc -O1 -I.. -o test_adc_setup test_adc_setup.c ../config.c ../hpsdr-protocol2.c \
    -lpthread -lm -Wl,--wrap=sendto
./test_adc_setup
```

On Angelia, ADC1 has exactly three settable parameters in Protocol 2: the step
attenuator (high-priority byte 1442), and its dither and random bits (receive-
specific bytes 5 and 6, bit 1 each). There is no positive gain control — the
attenuator only subtracts — so 0 dB is already maximum sensitivity and any
further improvement to that arm has to be external to the radio.
