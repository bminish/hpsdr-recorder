#include "stats.h"
#include "config.h"
#include <stdio.h>
#include <math.h>

static long long total_samples = 0;
static long long total_clipped = 0;
static int32_t peak_sample = 0;
static long long total_lost = 0;
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

        // Smallest shift that would still have fitted, keeping 6 dB spare
        int suggested = 0;
        while (suggested < 8 && ((peak_sample * 2) >> suggested) > 32767) suggested++;

        printf("Peak level: %.1f dBFS (24-bit full scale)\n", dbfs);
        printf("Sample shift used: %d", sample_shift);
        if (suggested < sample_shift) {
            printf("  -- %d would have fitted with 6 dB spare (+%d dB of weak-signal range)\n",
                   suggested, 6 * (sample_shift - suggested));
        } else {
            printf("\n");
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

    if (total_writes > 0) {
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
