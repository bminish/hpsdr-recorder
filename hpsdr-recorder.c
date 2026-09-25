#define _GNU_SOURCE
#include <string.h>
#include "hpsdr-recorder.h"
#include "config.h"
#include "hpsdr-protocol2.h"
#include "buffers.h"
#include "streaming.h"
#include "output.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static volatile bool keep_running = true;

void int_handler(int dummy) {
    (void)dummy;
    keep_running = false;
}

void main_exit(int exit_status) {
    streaming_stop();
    hpsdr_close();
    output_close();
    buffers_free();
    exit(exit_status);
}

// Sends the full receiver setup. Verbose on purpose: when the radio does not
// stream, knowing exactly which packet failed is the first thing you want.
static int send_radio_config(int attempt, int attempts) {
    printf("Configuring radio (attempt %d/%d)...\n", attempt, attempts);

    if (hpsdr_configure_general(false, false) < 0) {
        fprintf(stderr, "  general packet     -> port %d  FAILED: %s\n",
                GENERAL_REGISTERS_PORT, strerror(errno));
        return -1;
    }
    printf("  general packet     -> port %d  ok\n", GENERAL_REGISTERS_PORT);

    if (hpsdr_configure_transmitter_disabled() < 0) {
        fprintf(stderr, "  tx-disable packet  -> port %d  FAILED: %s\n",
                TRANSMITTER_SPECIFIC_REGISTERS_PORT, strerror(errno));
        return -1;
    }
    printf("  tx-disable packet  -> port %d  ok\n", TRANSMITTER_SPECIFIC_REGISTERS_PORT);

    if (hpsdr_configure_receiver(sample_rate, diversity, 0, 0) < 0) {
        fprintf(stderr, "  rx-specific packet -> port %d  FAILED: %s\n",
                RECEIVER_SPECIFIC_REGISTERS_PORT, strerror(errno));
        return -1;
    }
    printf("  rx-specific packet -> port %d  ok  (%d ADCs, %d Hz, 24 bit)\n",
           RECEIVER_SPECIFIC_REGISTERS_PORT, diversity ? 2 : 1, sample_rate);

    if (hpsdr_configure_high_priority(rx1_freq, rx2_freq) < 0) {
        fprintf(stderr, "  high-priority      -> port %d  FAILED: %s\n",
                HIGH_PRIORITY_PORT, strerror(errno));
        return -1;
    }
    printf("  high-priority      -> port %d  ok\n", HIGH_PRIORITY_PORT);
    return 0;
}

// Waits for the IQ stream to actually start. Returns 0 once samples arrive.
static int wait_for_stream(double seconds) {
    long long start_samples = stats_total_samples();
    long long start_iq = streaming_iq_packets();
    int ticks = (int)(seconds * 10);
    for (int i = 0; i < ticks && keep_running; i++) {
        usleep(100000);
        if (stats_total_samples() > start_samples) {
            printf("  IQ stream running (%lld packets in %.1f s)\n",
                   streaming_iq_packets() - start_iq, (i + 1) / 10.0);
            return 0;
        }
    }
    return -1;
}

int main(int argc, char *argv[]) {
    // Line-buffer stdout. Redirected to a file (as cron does), stdout is
    // block-buffered and only flushes at exit, while stderr is unbuffered -
    // so a failing run's diagnostics landed in the log ABOVE the lines
    // describing the run they belonged to, which made them look missing.
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (get_config_from_cli(argc, argv) == -1) {
        main_exit(EXIT_FAILURE);
    }

    signal(SIGINT, int_handler);
    // Without this a closed pipe/socket output kills the process outright
    // instead of letting the write path report the failure.
    signal(SIGPIPE, SIG_IGN);

    hpsdr_device_t device;
    if (!ip_address) {
        printf("Discovering HPSDR Protocol 2 device...\n");
        if (hpsdr_discover(NULL, &device) == 0) {
            printf("Found device at %s\n", device.ip_addr);
            ip_address = strdup(device.ip_addr);
        } else {
            fprintf(stderr, "Could not discover any HPSDR Protocol 2 devices.\n");
            main_exit(EXIT_FAILURE);
        }
    }
    
    printf("Connecting to %s...\n", ip_address);
    if (hpsdr_connect(ip_address) < 0) {
        main_exit(EXIT_FAILURE);
    }
    
    if (buffers_create() < 0) {
        fprintf(stderr, "Failed to create buffers\n");
        main_exit(EXIT_FAILURE);
    }

    if (output_open() < 0) {
        fprintf(stderr, "Failed to open output\n");
        main_exit(EXIT_FAILURE);
    }
    
    if (stream() < 0) {
        fprintf(stderr, "Failed to start streaming\n");
        main_exit(EXIT_FAILURE);
    }

    // Configure, then confirm the radio actually streams. A recording that
    // silently captures nothing is worse than one that fails loudly, so this
    // retries and then exits non-zero rather than producing an empty file.
    const int attempts = 3;
    int streaming_ok = 0;
    for (int attempt = 1; attempt <= attempts && keep_running; attempt++) {
        if (send_radio_config(attempt, attempts) < 0) {
            main_exit(EXIT_FAILURE);
        }
        if (attempt == 1) {
            // Keepalive must run for the radio to keep streaming
            hpsdr_start_keepalive(sample_rate, diversity, rx1_freq, rx2_freq);
        }
        printf("Waiting for IQ stream...\n");
        if (wait_for_stream(5.0) == 0) {
            streaming_ok = 1;
            break;
        }
        fprintf(stderr, "  NO IQ SAMPLES after 5.0 s\n");
        streaming_port_report();
        if (attempt < attempts) {
            fprintf(stderr, "  retrying configuration...\n");
        }
    }

    if (!streaming_ok) {
        if (!keep_running) {
            fprintf(stderr, "Interrupted before the stream started.\n");
        } else {
            fprintf(stderr,
                    "ERROR: radio at %s never started streaming after %d attempts.\n"
                    "       Nothing was recorded; exiting non-zero so a scheduled run\n"
                    "       is not mistaken for a successful capture.\n",
                    ip_address, attempts);
        }
        main_exit(EXIT_FAILURE);
    }
    
    printf("Tuning RX1 %d Hz, RX2 %d Hz (DDC correction %+d Hz)\n",
           rx1_freq, rx2_freq, freq_correction);
    printf("Antenna %s, attenuation %d dB, sample shift %d bits%s\n",
           rx_antenna_name(rx_antenna), attenuation, sample_shift,
           sample_shift < 8 ? " (reduced headroom)" : "");
    if (streaming_time > 0) {
        printf("Recording for %d seconds (Ctrl+C to stop early)...\n", streaming_time);
    } else {
        printf("Recording... Press Ctrl+C to stop.\n");
    }

    // Wall-clock rather than counting sleeps, so a signal-interrupted sleep
    // does not stretch the recording.
    time_t started = time(NULL);
    while (keep_running) {
        sleep(1);
        if (streaming_time > 0 && difftime(time(NULL), started) >= streaming_time) {
            printf("Reached the %d second limit.\n", streaming_time);
            break;
        }
    }
    
    printf("\nStopping recording...\n");
    
    print_stats();

    if (stats_total_samples() == 0) {
        fprintf(stderr, "ERROR: the stream stopped and no samples were recorded.\n");
        main_exit(EXIT_FAILURE);
    }

    main_exit(EXIT_SUCCESS);
    return 0;
}
