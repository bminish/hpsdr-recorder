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
