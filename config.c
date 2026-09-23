#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

char *ip_address = NULL;
int sample_rate = 1536000;
bool diversity = true;
int rx1_freq = 990000;
int rx2_freq = 990000;
int attenuation = 0;          // dB, 0-31, applied to both ADCs
int rx_antenna = RX_ANT_ANT1; // which jack feeds the receivers
int sample_shift = 8;         // 24-bit sample >> this many bits to make 16-bit
int alex_new_pa_board = 1;    // Rev.24 PA board (matches this radio's piHPSDR setting)
int streaming_time = 0;       // seconds; 0 = record until Ctrl+C
int socket_buffer_mb = 64;    // UDP receive buffer; ~3.6 s of slack at 1536 kHz
int freq_correction = 0; // Hz added to the DDC tuning word; calibration only, see hpsdr-protocol2.c
int output_type = OUTPUT_TYPE_LINRAD;
char *output_file = "hpsdr_recording.raw";
int blocks_buffer_capacity = 32768;
int samples_buffer_capacity = 64 * 1024 * 1024; // 64M shorts (~11s of RAM buffer)

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [options]\n", prog_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c <file>       Config file (e.g. MW_dual.conf)\n");
    fprintf(stderr, "  -i <IP>         Radio IP Address (default auto-discover)\n");
    fprintf(stderr, "  -r <rate>       Sample rate (default 1536000)\n");
    fprintf(stderr, "  -f <freq>       RX1 Frequency in Hz (default 990000)\n");
    fprintf(stderr, "  -F <freq>       RX2 Frequency in Hz (default 990000)\n");
    fprintf(stderr, "  -d              Enable diversity (default true)\n");
    fprintf(stderr, "  -o <file>       Output file (default hpsdr_recording.raw)\n");
    fprintf(stderr, "  -k <hz>         DDC tuning correction in Hz (default 0, calibration only)\n");
    fprintf(stderr, "  -x <secs>       Streaming time in seconds (default 0 = until Ctrl+C)\n");
    fprintf(stderr, "  -B <mb>         UDP receive buffer in MB (default 64; needs rmem_max)\n");
    fprintf(stderr, "  -a <db>         ADC attenuation 0-31 dB (default 0)\n");
    fprintf(stderr, "  -A <ant>        RX antenna: ant1|ant2|ant3|ext1|ext2|xvtr (default ant1)\n");
    fprintf(stderr, "  -s <bits>       Sample shift 0-8; 24-bit -> 16-bit (default 8).\n");
    fprintf(stderr, "                  Lower keeps weak signals; too low clips. 4 gains 24 dB.\n");
}

static const struct { const char *name; int value; } rx_antenna_names[] = {
    { "ant1", RX_ANT_ANT1 }, { "ant2", RX_ANT_ANT2 }, { "ant3", RX_ANT_ANT3 },
    { "ext1", RX_ANT_EXT1 }, { "ext2", RX_ANT_EXT2 }, { "xvtr", RX_ANT_XVTR },
};

const char *rx_antenna_name(int ant) {
    for (size_t i = 0; i < sizeof(rx_antenna_names) / sizeof(rx_antenna_names[0]); i++) {
        if (rx_antenna_names[i].value == ant) return rx_antenna_names[i].name;
    }
    return "?";
}

static int parse_rx_antenna(const char *val) {
    for (size_t i = 0; i < sizeof(rx_antenna_names) / sizeof(rx_antenna_names[0]); i++) {
        if (strcasecmp(val, rx_antenna_names[i].name) == 0) return rx_antenna_names[i].value;
    }
    fprintf(stderr, "config: unknown antenna '%s' (want ant1|ant2|ant3|ext1|ext2|xvtr), keeping %s\n",
            val, rx_antenna_name(rx_antenna));
    return rx_antenna;
}

// Clamp with a warning rather than silently accepting a value the radio would
// reject or that would corrupt the sample scaling.
static int clamp_int(const char *what, int v, int lo, int hi) {
    if (v < lo || v > hi) {
        fprintf(stderr, "config: %s %d out of range %d-%d, clamping\n", what, v, lo, hi);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    }
    return v;
}

int read_config_file(const char *filepath) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        perror("fopen config file");
        return -1;
    }
    
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        // Strip comment or newline
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        
        // Trim key
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        
        // Trim val
        while (*val == ' ' || *val == '\t') val++;
        end = val + strlen(val) - 1;
        while (end > val && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        
        if (strlen(key) == 0 || strlen(val) == 0) continue;
        
        if (strcasecmp(key, "ip") == 0 || strcasecmp(key, "ip_address") == 0) {
            ip_address = strdup(val);
        } else if (strcasecmp(key, "sample rate") == 0 || strcasecmp(key, "sample_rate") == 0) {
            sample_rate = atoi(val);
        } else if (strcasecmp(key, "frequency") == 0 || strcasecmp(key, "freq") == 0) {
            rx1_freq = atoi(val);
            rx2_freq = atoi(val);
        } else if (strcasecmp(key, "rx1_freq") == 0) {
            rx1_freq = atoi(val);
        } else if (strcasecmp(key, "rx2_freq") == 0) {
            rx2_freq = atoi(val);
        } else if (strcasecmp(key, "freq_correction") == 0 || strcasecmp(key, "frequency correction") == 0) {
            freq_correction = atoi(val);
        } else if (strcasecmp(key, "diversity") == 0) {
            diversity = (strcasecmp(val, "true") == 0 || atoi(val) == 1);
        } else if (strcasecmp(key, "output file") == 0 || strcasecmp(key, "output_file") == 0) {
            output_file = strdup(val);
        } else if (strcasecmp(key, "streaming time") == 0 || strcasecmp(key, "streaming_time") == 0
                   || strcasecmp(key, "duration") == 0) {
            streaming_time = clamp_int("streaming time", atoi(val), 0, 365 * 24 * 3600);
        } else if (strcasecmp(key, "socket buffer") == 0 || strcasecmp(key, "socket_buffer") == 0
                   || strcasecmp(key, "rcvbuf_mb") == 0) {
            socket_buffer_mb = clamp_int("socket buffer", atoi(val), 1, 1024);
        } else if (strcasecmp(key, "attenuation") == 0 || strcasecmp(key, "atten") == 0) {
            attenuation = clamp_int("attenuation", atoi(val), 0, 31);
        } else if (strcasecmp(key, "antenna") == 0 || strcasecmp(key, "rx_antenna") == 0) {
            rx_antenna = parse_rx_antenna(val);
        } else if (strcasecmp(key, "sample shift") == 0 || strcasecmp(key, "sample_shift") == 0) {
            sample_shift = clamp_int("sample shift", atoi(val), 0, 8);
        } else if (strcasecmp(key, "new_pa_board") == 0 || strcasecmp(key, "alex_new_pa_board") == 0) {
            alex_new_pa_board = atoi(val) ? 1 : 0;
        } else if (strcasecmp(key, "output type") == 0 || strcasecmp(key, "output_type") == 0) {
            if (strcasecmp(val, "linrad") != 0) {
                fprintf(stderr, "config: output type '%s' not supported, using Linrad\n", val);
            }
            output_type = OUTPUT_TYPE_LINRAD;
        } else {
            // Silently dropping keys is how "attenuation = 0" went unnoticed.
            fprintf(stderr, "config: ignoring unknown key '%s'\n", key);
        }
    }
    
    fclose(fp);
    return 0;
}

int get_config_from_cli(int argc, char *argv[]) {
    int opt;
    // Process -c first if present
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (read_config_file(argv[i+1]) < 0) {
                return -1;
            }
            break;
        }
    }
    
    optind = 1; // Reset getopt
    while ((opt = getopt(argc, argv, "c:i:r:f:F:do:k:a:A:s:x:B:")) != -1) {
        switch (opt) {
            case 'c':
                // Already read
                break;
            case 'i':
                ip_address = strdup(optarg);
                break;
            case 'r':
                sample_rate = atoi(optarg);
                break;
            case 'f':
                rx1_freq = atoi(optarg);
                break;
            case 'F':
                rx2_freq = atoi(optarg);
                break;
            case 'd':
                diversity = true;
                break;
            case 'o':
                output_file = strdup(optarg);
                break;
            case 'k':
                freq_correction = atoi(optarg);
                break;
            case 'x':
                streaming_time = clamp_int("streaming time", atoi(optarg), 0, 365 * 24 * 3600);
                break;
            case 'B':
                socket_buffer_mb = clamp_int("socket buffer", atoi(optarg), 1, 1024);
                break;
            case 'a':
                attenuation = clamp_int("attenuation", atoi(optarg), 0, 31);
                break;
            case 'A':
                rx_antenna = parse_rx_antenna(optarg);
                break;
            case 's':
                sample_shift = clamp_int("sample shift", atoi(optarg), 0, 8);
                break;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    return 0;
}
