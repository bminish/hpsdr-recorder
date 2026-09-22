#include "streaming.h"
#include "hpsdr-protocol2.h"
#include "buffers.h"
#include "config.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

// The radio gives 24-bit samples and the Linrad file holds 16-bit, so
// sample_shift bits are dropped. The default 8 keeps the ADC's top 16 bits and
// cannot clip; a smaller shift trades headroom for weak-signal resolution (each
// bit is 6 dB), which is the point at night. Clipping is counted rather than
// left silent - see print_stats().
//
// HPSDR delivers IQ whose spectrum is inverted relative to the convention
// Linrad/wavviewdx assume for passband_direction = 1, so Q is negated on the
// way out (a conjugation). Both diversity channels get the same treatment, so
// their relative phase - what the diversity/phasing analysis depends on - is
// preserved. Without this, carriers land 2*(centre mod 9000) Hz off the MW grid.
static inline int16_t pack_sample(int32_t v24, int *clipped) {
    int32_t s = v24 >> sample_shift;
    if (s > 32767) {
        (*clipped)++;
        return 32767;
    }
    if (s < -32768) {
        (*clipped)++;
        return -32768;
    }
    return (int16_t)s;
}

static inline int16_t pack_i(int32_t v24, int *clipped) {
    return pack_sample(v24, clipped);
}

static inline int16_t pack_q(int32_t v24, int *clipped) {
    return pack_sample(-v24, clipped);
}

// Peak of the raw 24-bit samples, so the report is independent of the shift.
static inline void track_peak(int32_t v24, int32_t *peak) {
    int32_t a = v24 < 0 ? -v24 : v24;
    if (a > *peak) *peak = a;
}

static long long overrun_events = 0;

// The ring holds ~11 s. If the output thread has not drained it in that time we
// drop the incoming packet: that keeps the ring self-consistent and, unlike
// silently overwriting, leaves a record that the file has a gap.
//
// This is NOT a throughput problem and the message must not claim it is. The
// stream needs ~12 MB/s; the NVMe this records to measures 2 GB/s, and
// rsp-recorder logs an 8 us average / 20 ms worst-case write at a higher rate.
// Filling an 11 s ring means the writer stopped, not that it was slow - a full
// volume, a hung mount, or a write that is blocking indefinitely.
static void note_overrun(int lost_samples) {
    overrun_events++;
    if (overrun_events == 1 || overrun_events % 100 == 0) {
        fprintf(stderr, "[RING FULL] output thread is not draining the buffer, "
                        "dropped %d samples (event %lld)\n",
                lost_samples, overrun_events);
    }
    stats_add_lost(lost_samples);
}

static pthread_t rx_thread;
static bool rx_thread_started = false;
static volatile bool streaming_active = true;

// Thread to read UDP packets and put them in the buffer
static void *rx_thread_func(void *arg) {
    (void)arg;
    uint8_t buffer[P2_BUFFER_SIZE];
    int bytes_read;
    uint16_t src_port = 0;
    while (streaming_active) {
        if (hpsdr_read_iq(buffer, &bytes_read, &src_port) == 0) {
            // ONLY process IQ data packets from RX_IQ_PORT (1035).
            // Ignore status/register response packets from ports 1024, 1025, 1026, 1027.
            if (src_port == RX_IQ_PORT && bytes_read >= 16) {
                int samplesperframe = (buffer[14] << 8) | buffer[15];
                uint32_t seq = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) | ((uint32_t)buffer[2] << 8) | (uint32_t)buffer[3];
                
                // Validate before samplesperframe is used for anything: the
                // sequence-drop handler below sizes a zero-fill from it and the
                // sample loop indexes the packet with it. In diversity the
                // samples arrive as ADC0/ADC1 pairs, so an odd count is
                // malformed. Requiring the packet to actually hold the bytes -
                // and to fit the receive buffer - is what stops the loop
                // reading off the end of it.
                int step = diversity ? 2 : 1;
                int steps = samplesperframe / step;
                int bytes_needed = 16 + steps * (diversity ? 12 : 6);
                if (samplesperframe <= 0 || samplesperframe > 1024
                        || (diversity && (samplesperframe % 2) != 0)
                        || bytes_needed > bytes_read
                        || bytes_needed > (int)sizeof(buffer)) {
                    fprintf(stderr, "[UDP IQ] malformed packet dropped: samples-per-frame %d, %d bytes\n",
                            samplesperframe, bytes_read);
                    continue;
                }

                static uint32_t expected_seq = 0;
                static bool first_pkt = true;
                if (first_pkt) {
                    expected_seq = seq;
                    first_pkt = false;
                } else if (seq != expected_seq) {
                    int32_t gap = (int32_t)(seq - expected_seq);
                    fprintf(stderr, "[UDP IQ SEQ DROP] Expected %u, got %u (gap: %d packets)\n", expected_seq, seq, gap);
                    
                    // Zero-fill missing packets to maintain sample rate and phase continuity
                    if (gap > 0 && gap < 10000) {
                        int samples_per_pkt = samplesperframe / (diversity ? 2 : 1);
                        int total_missing_quads = gap * samples_per_pkt;
                        int n_vals = total_missing_quads * (diversity ? 4 : 2);
                        
                        pthread_mutex_lock(samples_resource.lock);
                        bool have_room =
                            (samples_resource.nused + (unsigned int)n_vals <= samples_resource.size);
                        int start_idx = samples_resource.write_index;
                        if (have_room) {
                            for (int k = 0; k < n_vals; k++) {
                                insamples[samples_resource.write_index++] = 0;
                                if (samples_resource.write_index >= samples_resource.size) {
                                    samples_resource.write_index = 0;
                                }
                            }
                            samples_resource.nused += n_vals;
                        }
                        pthread_mutex_unlock(samples_resource.lock);

                        if (!have_room) {
                            note_overrun(total_missing_quads);
                            expected_seq = seq;
                            goto seq_resynced;
                        }

                        pthread_mutex_lock(blocks_resource.lock);
                        BlockDescriptor *block = &((BlockDescriptor*)blocks_resource.resource)[blocks_resource.write_index];
                        block->num_samples = total_missing_quads;
                        block->samples_index = start_idx;
                        
                        blocks_resource.write_index = (blocks_resource.write_index + 1) % blocks_resource.size;
                        blocks_resource.nused++;
                        blocks_resource.nready++;
                        pthread_cond_signal(blocks_resource.is_ready);
                        pthread_mutex_unlock(blocks_resource.lock);
                    }
                    expected_seq = seq;
                seq_resynced: ;
                }
                expected_seq++;

                // Packet was validated above.
                {
                    int b = 16;
                    int clipped = 0;
                    int32_t peak = 0;
                    unsigned int needed = (unsigned int)steps * (diversity ? 4u : 2u);

                    pthread_mutex_lock(samples_resource.lock);
                    if (samples_resource.nused + needed > samples_resource.size) {
                        pthread_mutex_unlock(samples_resource.lock);
                        note_overrun(steps);
                        continue;
                    }
                    int start_index = samples_resource.write_index;
                    
                    for (int i = 0; i < samplesperframe; i += (diversity ? 2 : 1)) {
                        // Extract RX1 24-bit IQ and shift right 8 bits to 16-bit short
                        int32_t i_val = (int8_t)buffer[b++] << 16;
                        i_val |= buffer[b++] << 8;
                        i_val |= buffer[b++];
                        int32_t q_val = (int8_t)buffer[b++] << 16;
                        q_val |= buffer[b++] << 8;
                        q_val |= buffer[b++];
                        
                        track_peak(i_val, &peak);
                        track_peak(q_val, &peak);
                        insamples[samples_resource.write_index++] = pack_i(i_val, &clipped);
                        insamples[samples_resource.write_index++] = pack_q(q_val, &clipped);
                        if (samples_resource.write_index >= samples_resource.size) {
                            samples_resource.write_index = 0;
                        }
                        
                        if (diversity) {
                            int32_t i_val2 = (int8_t)buffer[b++] << 16;
                            i_val2 |= buffer[b++] << 8;
                            i_val2 |= buffer[b++];
                            int32_t q_val2 = (int8_t)buffer[b++] << 16;
                            q_val2 |= buffer[b++] << 8;
                            q_val2 |= buffer[b++];
                            
                            track_peak(i_val2, &peak);
                            track_peak(q_val2, &peak);
                            insamples[samples_resource.write_index++] = pack_i(i_val2, &clipped);
                            insamples[samples_resource.write_index++] = pack_q(q_val2, &clipped);
                            if (samples_resource.write_index >= samples_resource.size) {
                                samples_resource.write_index = 0;
                            }
                        }
                        
                        samples_resource.nused += (diversity ? 4 : 2);
                    }

                    if (samples_resource.nused > samples_resource.nused_max) {
                        samples_resource.nused_max = samples_resource.nused;
                    }
                    
                    pthread_mutex_unlock(samples_resource.lock);

                    // Add block
                    pthread_mutex_lock(blocks_resource.lock);
                    BlockDescriptor *block = &((BlockDescriptor*)blocks_resource.resource)[blocks_resource.write_index];
                    block->num_samples = samplesperframe / (diversity ? 2 : 1);
                    block->samples_index = start_index;
                    
                    blocks_resource.write_index = (blocks_resource.write_index + 1) % blocks_resource.size;
                    blocks_resource.nused++;
                    blocks_resource.nready++;
                    pthread_cond_signal(blocks_resource.is_ready);
                    pthread_mutex_unlock(blocks_resource.lock);
                    
                    stats_add_samples(samplesperframe / (diversity ? 2 : 1));
                    stats_add_levels(clipped, peak);
                }
            }
        }
    }
    return NULL;
}

int stream(void) {
    if (pthread_create(&rx_thread, NULL, rx_thread_func, NULL) != 0) {
        perror("pthread_create");
        return -1;
    }
    rx_thread_started = true;
    return 0;
}

// Must run before buffers_free(): the RX thread writes straight into insamples,
// so freeing the buffer under it is a use-after-free. The receive socket has a
// 1 s timeout, so the join returns promptly.
void streaming_stop(void) {
    if (!rx_thread_started) {
        return;
    }
    streaming_active = false;
    pthread_join(rx_thread, NULL);
    rx_thread_started = false;
}
