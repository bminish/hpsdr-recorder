#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define OUTPUT_TYPE_LINRAD 1

// RX antenna selection. ANT1..ANT3 are the main jacks (the RX signal then also
// passes through the TX low-pass filters on this board); EXT1/EXT2/XVTR are the
// receive-only inputs, which bypass them.
#define RX_ANT_ANT1 0
#define RX_ANT_ANT2 1
#define RX_ANT_ANT3 2
#define RX_ANT_EXT1 3
#define RX_ANT_EXT2 4
#define RX_ANT_XVTR 5

extern char *ip_address;
extern int sample_rate;
extern bool diversity;
extern int rx1_freq;
extern int rx2_freq;
extern int freq_correction;
extern int attenuation;
extern int rx_antenna;
extern int sample_shift;
extern int alex_new_pa_board;
extern int streaming_time;
extern int socket_buffer_mb;

const char *rx_antenna_name(int ant);
extern int output_type;
extern char *output_file;
extern int blocks_buffer_capacity;
extern int samples_buffer_capacity;

int get_config_from_cli(int argc, char *argv[]);
int read_config_file(const char *filepath);

#endif // CONFIG_H
