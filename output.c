#define _GNU_SOURCE
#include "output.h"
#include "buffers.h"
#include "config.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>

#define LINRAD_REMEMBER_UNKNOWN -1
#define LINRAD_TWO_CHANNELS 2
#define LINRAD_IQ_DATA 4
#define LINRAD_DIGITAL_IQ 32

typedef struct __attribute__((packed)) {
    char linrad_id[14]; // Not in the original header? The original header we saw was different.
} LinradHeader_dummy; // Just keeping it for reference, actually the header from rsp-recorder is below:

typedef struct __attribute__((packed)) {
    int remember_proprietary_chunk;
    double timestamp;
    double passband_center;
    int passband_direction;
    int rx_input_mode;
    int rx_rf_channels;
    int rx_ad_channels;
    int rx_ad_speed;
    unsigned char save_init_flag;
} LinradHeader;

static int outputfd = -1;
static pthread_t output_thread;
static bool output_thread_started = false;
static long long write_error_events = 0;
static off_t written_bytes = 0;
static off_t chunk_start = 0;
static off_t sync_off = 0;
static off_t sync_len = 0;

// Coalesce up to ~1 MB per write. Consecutive blocks are contiguous in the
// ring (one RX thread appends samples and descriptors in order), so N ready
// blocks are a single span. Writing them one at a time cost one syscall per
// received packet - ~12900/s of under 1 KB each.
#define COALESCE_MAX_VALUES (512 * 1024)
#define DRAIN_INTERVAL_MS   50
#define DRAIN_TARGET_BLOCKS 1024

// Every byte here is write-once and never read back, but it all lands in the
// page cache: an hour-long capture evicts 44 GB of other applications' data.
// Start writeback on the chunk just finished, then release the one before it,
// which by then is clean. Only the older chunk is waited on, so this does not
// stall the writer.
#define CACHE_CHUNK_BYTES (8 * 1024 * 1024)

static void release_page_cache(void) {
    if (written_bytes - chunk_start < CACHE_CHUNK_BYTES) {
        return;
    }
    off_t off = chunk_start;
    off_t len = written_bytes - chunk_start;
    sync_file_range(outputfd, off, len, SYNC_FILE_RANGE_WRITE);
    if (sync_len > 0) {
        sync_file_range(outputfd, sync_off, sync_len,
                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
                        SYNC_FILE_RANGE_WAIT_AFTER);
        posix_fadvise(outputfd, sync_off, sync_len, POSIX_FADV_DONTNEED);
    }
    sync_off = off;
    sync_len = len;
    chunk_start = written_bytes;
}
static volatile bool output_active = true;

static int write_linrad_header() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double timestamp = (double) ts.tv_sec + 1e-9 * ts.tv_nsec;

    int rx_input_mode = LINRAD_IQ_DATA | LINRAD_DIGITAL_IQ;
    int rx_rf_channels = 1;
    int rx_ad_channels = 2;
    if (diversity) {
        rx_input_mode |= LINRAD_TWO_CHANNELS;
        rx_rf_channels = 2;
        rx_ad_channels = 4;
    }
    
    LinradHeader linrad_header = {
        .remember_proprietary_chunk = LINRAD_REMEMBER_UNKNOWN,
        .timestamp = timestamp,
        .passband_center = rx1_freq / 1e6,
        .passband_direction = 1,
        .rx_input_mode = rx_input_mode,
        .rx_rf_channels = rx_rf_channels,
        .rx_ad_channels = rx_ad_channels,
        .rx_ad_speed = sample_rate,
        .save_init_flag = 0
    };
    
    if (write(outputfd, &linrad_header, sizeof(linrad_header)) == -1) {
        perror("write linrad header");
        return -1;
    }

    return 0;
}

// Writes are timed and their results checked. Ignoring the result (as this did)
// means a full volume or an I/O error silently discards the samples while the
// recording appears to succeed - and it would not even show up as a ring
// overrun, because a failing write returns immediately and keeps the ring
// draining.
static void timed_write(const void *buf, size_t len) {
    struct timespec t0, t1;
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    bool partial = false;
    int err = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (done < len) {
        ssize_t n = write(outputfd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            break;
        }
        if (n == 0) {
            break;
        }
        done += (size_t)n;
        if (done < len) {
            partial = true;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    long long lost = (long long)(len - done);
    written_bytes += (off_t)done;

    if (lost > 0) {
        write_error_events++;
        if (write_error_events == 1 || write_error_events % 100 == 0) {
            fprintf(stderr, "[WRITE FAILED] %lld bytes not written (%s) -- the recording "
                            "is losing samples (event %lld)\n",
                    lost, err ? strerror(err) : "short write", write_error_events);
        }
    }
    stats_add_write(elapsed, partial, lost);
}

static void *output_thread_func(void *arg) {
    (void)arg;
    
    while (output_active) {
        // Drain on a timer, not per packet. The ring holds ~11 s, so letting
        // 50 ms accumulate costs nothing and turns ~12900 tiny writes per
        // second into ~20 large ones. The condvar is now only used to make
        // shutdown immediate.
        // Wait for a worthwhile batch OR the deadline, whichever comes first.
        // Waiting only when nothing was ready never accumulated anything: with
        // packets arriving at ~12900/s there is always one waiting, so the
        // thread spun writing a single block at a time. The ring holds ~11 s,
        // so spending 50 ms gathering costs nothing and turns those writes
        // into ~20 large ones per second.
        pthread_mutex_lock(blocks_resource.lock);
        {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += DRAIN_INTERVAL_MS * 1000L * 1000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            while (output_active && blocks_resource.nready < DRAIN_TARGET_BLOCKS) {
                if (pthread_cond_timedwait(blocks_resource.is_ready,
                                           blocks_resource.lock, &deadline) == ETIMEDOUT) {
                    break;
                }
            }
        }
        if (!output_active) {
            pthread_mutex_unlock(blocks_resource.lock);
            break;
        }
        if (blocks_resource.nready == 0) {
            pthread_mutex_unlock(blocks_resource.lock);
            continue;
        }
        
        // Take every ready block that fits in one coalesced write. They are
        // contiguous in the ring, so this is one span (two after a wrap).
        const BlockDescriptor *blocks = (const BlockDescriptor *)blocks_resource.resource;
        int n_channels = diversity ? 4 : 2;
        unsigned int start = blocks[blocks_resource.read_index].samples_index;
        unsigned int total = 0;
        unsigned int taken = 0;
        while (taken < blocks_resource.nready) {
            unsigned int i = (blocks_resource.read_index + taken) % blocks_resource.size;
            unsigned int vals = blocks[i].num_samples * (unsigned int)n_channels;
            if (total > 0 && total + vals > COALESCE_MAX_VALUES) {
                break;
            }
            total += vals;
            taken++;
        }
        blocks_resource.read_index = (blocks_resource.read_index + taken) % blocks_resource.size;
        blocks_resource.nready -= taken;
        blocks_resource.nused -= taken;
        pthread_mutex_unlock(blocks_resource.lock);

        unsigned int ring_sz = samples_resource.size;
        if (start + total <= ring_sz) {
            timed_write(&insamples[start], total * sizeof(int16_t));
        } else {
            unsigned int part1 = ring_sz - start;
            timed_write(&insamples[start], part1 * sizeof(int16_t));
            timed_write(&insamples[0], (total - part1) * sizeof(int16_t));
        }
        release_page_cache();

        // Release the space so the RX thread can tell how far behind we are.
        // Without this, nused only ever grows and overrun is undetectable.
        pthread_mutex_lock(samples_resource.lock);
        if (samples_resource.nused >= total) {
            samples_resource.nused -= total;
        } else {
            samples_resource.nused = 0;
        }
        pthread_mutex_unlock(samples_resource.lock);
    }
    return NULL;
}

static void expand_filename(const char *in, char *out, size_t out_max) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    char ts_str[32];
    strftime(ts_str, sizeof(ts_str), "%Y%m%dT%H%M%SZ", tm);

    char freq_str[32];
    snprintf(freq_str, sizeof(freq_str), "%dkHz", rx1_freq / 1000);

    const char *p = in;
    char *o = out;
    char *end = out + out_max - 1;

    while (*p && o < end) {
        if (strncmp(p, "{TIMESTAMP}", 11) == 0) {
            size_t len = strlen(ts_str);
            if (o + len < end) {
                strcpy(o, ts_str);
                o += len;
            }
            p += 11;
        } else if (strncmp(p, "{FREQKHZ}", 9) == 0) {
            size_t len = strlen(freq_str);
            if (o + len < end) {
                strcpy(o, freq_str);
                o += len;
            }
            p += 9;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

int output_open(void) {
    char expanded_filename[1024];
    expand_filename(output_file, expanded_filename, sizeof(expanded_filename));
    
    // Ensure parent directory exists or file path is valid
    outputfd = open(expanded_filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (outputfd < 0) {
        perror("open output file");
        fprintf(stderr, "Failed file path: %s\n", expanded_filename);
        return -1;
    }
    printf("Writing output to: %s\n", expanded_filename);
    
    if (output_type == OUTPUT_TYPE_LINRAD) {
        if (write_linrad_header() < 0) return -1;
    }
    
    if (pthread_create(&output_thread, NULL, output_thread_func, NULL) != 0) {
        perror("pthread_create output");
        return -1;
    }
    output_thread_started = true;
    
    return 0;
}

int output_process(void) {
    // We already do it in thread, so just wait
    return 0;
}

void output_close(void) {
    output_active = false;
    // Only join a thread that was actually created: output_open() returns early
    // if the output file cannot be opened, and joining garbage crashes.
    if (output_thread_started) {
        pthread_mutex_lock(blocks_resource.lock);
        pthread_cond_signal(blocks_resource.is_ready);
        pthread_mutex_unlock(blocks_resource.lock);
        pthread_join(output_thread, NULL);
        output_thread_started = false;
    }
    if (outputfd >= 0) {
        close(outputfd);
        outputfd = -1;
    }
}
