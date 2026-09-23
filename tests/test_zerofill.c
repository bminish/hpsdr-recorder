// Verifies that a sequence gap is padded with exactly the right number of
// zeroed samples, so that samples after the gap land at their true instant and
// the carrier phase continues coherently.
//
// Includes streaming.c directly to reach the static process_packet().
#include <string.h>
#include "streaming.c"

#include <math.h>
#include <complex.h>

#define SPF     238            // samples per frame, as the radio sends
#define QUADS   (SPF / 2)      // sample instants per packet
#define TONE    100000.0       // Hz offset; 1 sample of slip = 23.4 deg

static void put24(uint8_t *p, int32_t v) {
    p[0] = (v >> 16) & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = v & 0xFF;
}

// Build the packet the radio would send for sample instants starting at `quad`
static void make_packet(uint8_t *buf, uint32_t seq, long long quad) {
    memset(buf, 0, P2_BUFFER_SIZE);
    buf[0] = seq >> 24; buf[1] = seq >> 16; buf[2] = seq >> 8; buf[3] = seq;
    buf[14] = SPF >> 8; buf[15] = SPF & 0xFF;
    int b = 16;
    for (int i = 0; i < QUADS; i++) {
        double ph = 2.0 * M_PI * TONE * (double)(quad + i) / 1536000.0;
        int32_t iv = (int32_t)(2000000.0 * cos(ph));
        int32_t qv = (int32_t)(2000000.0 * sin(ph));
        put24(&buf[b], iv); b += 3;   // ADC0 I
        put24(&buf[b], qv); b += 3;   // ADC0 Q
        put24(&buf[b], iv); b += 3;   // ADC1 I
        put24(&buf[b], qv); b += 3;   // ADC1 Q
    }
}

int main(void) {
    diversity = true; sample_rate = 1536000; sample_shift = 8;
    samples_buffer_capacity = 8 * 1024 * 1024;
    blocks_buffer_capacity = 4096;
    if (buffers_create() < 0) { printf("buffers failed\n"); return 1; }

    const int npkt = 120;
    int skip[] = {10, 11, 12, 40, 70, 71, 72, 73, 74};   // deliberate losses
    int nskip = sizeof(skip)/sizeof(skip[0]);
    uint8_t buf[P2_BUFFER_SIZE];

    for (int k = 0; k < npkt; k++) {
        bool dropped = false;
        for (int j = 0; j < nskip; j++) if (skip[j] == k) dropped = true;
        if (dropped) continue;                  // simulate the packet never arriving
        make_packet(buf, (uint32_t)k, (long long)k * QUADS);
        process_packet(buf, 16 + QUADS * 12);
    }

    long long expected_quads = (long long)npkt * QUADS;
    long long got_quads = samples_resource.nused / 4;
    printf("packets sent      : %d of %d (%d deliberately lost)\n", npkt - nskip, npkt, nskip);
    printf("sample instants   : %lld  (expected %lld)  %s\n",
           got_quads, expected_quads,
           got_quads == expected_quads ? "NO TIME SLIPPAGE" : "*** SLIPPED ***");

    // Phase coherency: compare each recorded sample against a free-running NCO
    // at the same frequency. Q is negated on record, so the reference is
    // conjugated. Zero-filled samples are skipped (they carry no phase).
    double worst = 0.0; long long checked = 0, zeros = 0;
    for (long long n = 0; n < got_quads; n++) {
        double complex rec = insamples[n*4] + I * insamples[n*4 + 1];
        if (cabs(rec) < 1.0) { zeros++; continue; }
        double ph = 2.0 * M_PI * TONE * (double)n / 1536000.0;
        double complex ref = cos(ph) - I * sin(ph);          // conjugated
        double err = carg(rec * conj(ref)) * 180.0 / M_PI;
        if (fabs(err) > worst) worst = fabs(err);
        checked++;
    }
    printf("zero-filled       : %lld sample instants\n", zeros);
    printf("phase checked     : %lld sample instants\n", checked);
    printf("worst phase error : %.3f deg   (1 sample of slip would be 23.4 deg)\n", worst);
    // --- scenario 2: a late/duplicate datagram must not shift the timeline ---
    first_pkt = true; expected_seq = 0;
    samples_resource.nused = 0; samples_resource.write_index = 0;
    blocks_resource.nused = 0; blocks_resource.nready = 0; blocks_resource.write_index = 0;

    printf("\n--- reordering ---\n");
    for (int k = 0; k < 40; k++) {
        make_packet(buf, (uint32_t)k, (long long)k * QUADS);
        process_packet(buf, 16 + QUADS * 12);
        if (k == 25) {                       // replay an old packet
            make_packet(buf, 20u, 20LL * QUADS);
            process_packet(buf, 16 + QUADS * 12);
        }
    }
    long long q2 = samples_resource.nused / 4;
    printf("duplicate of packet 20 replayed after packet 25\n");
    printf("sample instants   : %lld  (expected %d)  %s\n",
           q2, 40 * QUADS, q2 == 40 * QUADS ? "DISCARDED, no slippage" : "*** SLIPPED ***");

    printf("\nRESULT: %s\n",
           (got_quads == expected_quads && worst < 1.0 && q2 == 40 * QUADS)
               ? "timing exact, phase coherent across gaps, reordering rejected"
               : "*** FAILED ***");
    return 0;
}
