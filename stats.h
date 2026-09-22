#ifndef STATS_H
#define STATS_H

#include <stdint.h>

void stats_add_samples(int num_samples);
void stats_add_levels(int clipped, int32_t peak24);
void stats_add_lost(int samples);
void stats_add_write(double elapsed, int partial, long long failed_bytes);
int print_stats(void);

#endif // STATS_H
