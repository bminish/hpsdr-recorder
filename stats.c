#include "stats.h"
#include "config.h"
#include "hpsdr-protocol2.h"
#include <stdio.h>
#include <math.h>

static long long total_samples = 0;
static long long total_clipped = 0;
static int32_t peak_sample = 0;
long long stats_amp_hist[STATS_HIST_BINS];
static long long total_lost = 0;
static long long gap_events = 0;
static long long gap_packets = 0;
static long long gap_samples_padded = 0;
static long long reordered_packets = 0;
static long long slipped_samples = 0;
static long long total_writes = 0;
static long long partial_writes = 0;
static long long failed_writes = 0;
static long long failed_bytes = 0;
static double write_elapsed_total = 0.0;
static double write_elapsed_max = 0.0;

void stats_add_samples(int num_samples) {
    total_samples += num_samples;
}

void stats_add_write(double elapsed, int partial, long long failed_bytes_in) {
    total_writes++;
    if (partial) partial_writes++;
    if (failed_bytes_in > 0) {
        failed_writes++;
        failed_bytes += failed_bytes_in;
    }
    write_elapsed_total += elapsed;
    if (elapsed > write_elapsed_max) write_elapsed_max = elapsed;
}

void stats_add_gap(int packets, int samples_padded) {
    gap_events++;
    gap_packets += packets;
    gap_samples_padded += samples_padded;
}

void stats_add_reordered(void) {
    reordered_packets++;
}

void stats_add_slip(long long samples) {
    slipped_samples += samples;
}

void stats_add_lost(int samples) {
    total_lost += samples;
}

void stats_add_levels(int clipped, int32_t peak24) {
    total_clipped += clipped;
    if (peak24 > peak_sample) peak_sample = peak24;
}

int print_stats(void) {
    printf("Total samples recorded: %lld\n", total_samples);

    if (peak_sample > 0) {
        // 24-bit full scale, independent of the shift that was applied
        double dbfs = 20.0 * log10((double)peak_sample / 8388607.0);
        printf("Peak level: %.1f dBFS (24-bit full scale)\n", dbfs);

        long long total = 0;
        for (int i = 0; i < STATS_HIST_BINS; i++) total += stats_amp_hist[i];
        if (total > 0) {
            // 99.9% level: the band's routine amplitude, with impulses excluded
            long long cum = 0, want = (long long)(0.999 * (double)total);
            int bin = 0;
            for (bin = 0; bin < STATS_HIST_BINS; bin++) {
                cum += stats_amp_hist[bin];
                if (cum >= want) break;
            }
            double routine = (double)((bin + 1) << STATS_HIST_SHIFT);
            if (routine < 1.0) routine = 1.0;
            printf("99.9%% of samples below %.1f dBFS -- impulse peaks run %.1f dB above that\n",
                   20.0 * log10(routine / 8388607.0),
                   20.0 * log10((double)peak_sample / routine));
        }

        // Headroom to clipping at the shift actually used, and the most
        // aggressive shift that still keeps a margin for impulse noise.
        // The margin is deliberately generous: a short run's peak says little
        // about the static crash that arrives at 3am, and a clipped impulse is
        // not merely distorted - it can no longer be removed by a noise
        // blanker downstream, which needs the impulse's true shape.
        const double margin_db = 20.0;
        double headroom = 20.0 * log10(32767.0 / ((double)peak_sample / (double)(1 << sample_shift)));
        printf("Sample shift used: %d -- %.1f dB of headroom above this run's peak\n",
               sample_shift, headroom);

        int suggested = sample_shift;
        while (suggested > 0) {
            double h = 20.0 * log10(32767.0 / ((double)peak_sample / (double)(1 << (suggested - 1))));
            if (h < margin_db) break;
            suggested--;
        }
        if (suggested < sample_shift) {
            printf("   shift %d would keep %.0f dB of margin (+%d dB weak-signal range);\n"
                   "   going lower risks clipping impulse noise, which cannot be undone\n",
                   suggested,
                   20.0 * log10(32767.0 / ((double)peak_sample / (double)(1 << suggested))),
                   6 * (sample_shift - suggested));
        }
    }

    if (total_lost > 0) {
        double mbps = sample_rate * (diversity ? 4.0 : 2.0) * 2.0 / 1e6;
        printf("LOST: %lld samples dropped -- the sample ring filled, so the recording\n"
               "      has a time discontinuity. The stream is only %.1f MB/s, so this\n"
               "      means the output stopped draining (full volume, hung mount or a\n"
               "      blocking write), not that the disk was too slow.\n",
               total_lost, mbps);
    }

    printf("\n--- link ---\n");
    printf("socket receive buffer = %d bytes (%.0f ms of stream)\n",
           hpsdr_socket_rcvbuf(),
           1000.0 * hpsdr_socket_rcvbuf() / (sample_rate * (diversity ? 2.0 : 1.0) * 6.0));
    printf("sequence gaps = %lld events, %lld packets missing\n", gap_events, gap_packets);
    printf("zero-filled = %lld samples (timing preserved)\n", gap_samples_padded);
    printf("reordered/duplicate packets discarded = %lld\n", reordered_packets);
    if (slipped_samples > 0) {
        printf("UNPADDED SLIP: %lld samples -- timing after this point has shifted\n",
               slipped_samples);
    }
    printf("socket overflow drops = %llu%s\n", hpsdr_socket_drops(),
           hpsdr_socket_drops() > 0 ? "  <- arrived but we were too slow (local, fixable)" : "");
    if (gap_packets > 0 && hpsdr_socket_drops() == 0) {
        printf("  (no socket overflow: the missing packets never arrived -- network loss)\n");
    }

    if (total_writes > 0) {
        printf("\n--- disk ---\n");
        printf("total writes = %lld\n", total_writes);
        printf("full writes = %lld\n", total_writes - partial_writes);
        printf("partial writes = %lld\n", partial_writes);
        printf("average write elapsed = %.9f\n", write_elapsed_total / (double)total_writes);
        printf("max write elapsed = %.9f\n", write_elapsed_max);
    }
    if (failed_writes > 0) {
        printf("FAILED WRITES: %lld (%lld bytes lost) -- the recording is incomplete\n",
               failed_writes, failed_bytes);
    }

    long long total_values = total_samples * (diversity ? 4 : 2);
    if (total_clipped > 0) {
        printf("CLIPPED: %lld of %lld values (%.4f%%) -- raise sample shift (-s) or add attenuation (-a)\n",
               total_clipped, total_values,
               total_values ? 100.0 * (double)total_clipped / (double)total_values : 0.0);
    } else {
        printf("Clipped values: none\n");
    }
    return 0;
}
