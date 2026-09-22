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

int main(int argc, char *argv[]) {
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

    // Explicitly disable PA, Alex based on config
    if (hpsdr_configure_general(false, false) < 0) {
        fprintf(stderr, "Failed to send general config\n");
        main_exit(EXIT_FAILURE);
    }

    if (hpsdr_configure_transmitter_disabled() < 0) {
        fprintf(stderr, "Failed to send tx config\n");
        main_exit(EXIT_FAILURE);
    }

    if (hpsdr_configure_receiver(sample_rate, diversity, 0, 0) < 0) {
        fprintf(stderr, "Failed to send rx config\n");
        main_exit(EXIT_FAILURE);
    }
    
    // NOW start keepalive thread to trigger radio streaming after RX thread is ready
    hpsdr_start_keepalive(sample_rate, diversity, rx1_freq, rx2_freq);
    
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
    
    main_exit(EXIT_SUCCESS);
    return 0;
}
