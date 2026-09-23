#ifndef STATS_H
#define STATS_H

#include <stdint.h>

// Coarse amplitude histogram of the raw 24-bit samples, filled inline by the
// RX thread (a function call per sample would be too costly at 6 M values/s).
#define STATS_HIST_BINS  1024
#define STATS_HIST_SHIFT 13      // 24-bit amplitude >> 13 -> 0..1023
extern long long stats_amp_hist[STATS_HIST_BINS];

void stats_add_samples(int num_samples);
void stats_add_levels(int clipped, int32_t peak24);
void stats_add_lost(int samples);
void stats_add_gap(int packets, int samples_padded);
void stats_add_reordered(void);
void stats_add_slip(long long samples);
void stats_add_write(double elapsed, int partial, long long failed_bytes);
int print_stats(void);

#endif // STATS_H
