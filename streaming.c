#include "streaming.h"
#include "hpsdr-protocol2.h"
#include "buffers.h"
#include "config.h"
#include "stats.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>

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

// Peak and amplitude distribution of the raw 24-bit samples, so the report is
// independent of the shift. The distribution is what tells impulse noise apart
// from steady signal: a single peak cannot.
static inline void track_peak(int32_t v24, int32_t *peak) {
    int32_t a = v24 < 0 ? -v24 : v24;
    if (a > 8388607) a = 8388607;
    if (a > *peak) *peak = a;
    stats_amp_hist[a >> STATS_HIST_SHIFT]++;
}

static long long overrun_events = 0;
static int batch_max_seen = 0;

// Packets seen per source port. The radio answers on 1024-1027 even when it is
// not streaming IQ on 1035, so this distinguishes "radio is not talking to us
// at all" from "radio is talking but will not stream".
static volatile long long pkt_by_port[6];   // 1024,1025,1026,1027,1035,other
static volatile long long pkt_total;
static uint8_t first_other[16];
static uint32_t seen_ip[4];
static long long seen_ip_count[4];
static int seen_ips = 0;
static volatile long long foreign_iq;
static int first_other_port = -1;
static int first_other_len;

static int port_slot(uint16_t port) {
    switch (port) {
        case 1024: return 0;
        case 1025: return 1;
        case 1026: return 2;
        case 1027: return 3;
        case RX_IQ_PORT: return 4;
        default: return 5;
    }
}

long long streaming_iq_packets(void) {
    return pkt_by_port[4];
}

long long streaming_foreign_iq(void) {
    return foreign_iq;
}

void streaming_port_report(void) {
    static const char *names[6] = {
        "1024 general/status", "1025 rx-specific", "1026 tx-specific",
        "1027 high-priority", "1035 IQ DATA", "other"
    };
    fprintf(stderr, "  packets received: %lld total\n", pkt_total);
    for (int i = 0; i < 6; i++) {
        if (pkt_by_port[i] > 0) {
            fprintf(stderr, "    %-22s %lld\n", names[i], pkt_by_port[i]);
        }
    }
    for (int i = 0; i < seen_ips; i++) {
        struct in_addr a; a.s_addr = seen_ip[i];
        fprintf(stderr, "    from %-16s %lld packets\n", inet_ntoa(a), seen_ip_count[i]);
    }
    if (pkt_total == 0) {
        fprintf(stderr, "    nothing at all from the radio -- check it is powered and\n"
                        "    reachable, and that no other client (piHPSDR, Thetis) holds it\n");
    } else if (pkt_by_port[4] == 0) {
        fprintf(stderr, "    the radio IS responding but is not streaming IQ:\n"
                        "    it is likely streaming to a different client, or it rejected\n"
                        "    the receiver-specific setup\n");
        if (first_other_port >= 0) {
            fprintf(stderr, "    first reply from port %d, %d bytes:", first_other_port, first_other_len);
            for (int i = 0; i < first_other_len && i < 16; i++) {
                fprintf(stderr, " %02X", first_other[i]);
            }
            fprintf(stderr, "\n");
        }
    }
}

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
static uint32_t expected_seq = 0;
static bool first_pkt = true;

static void process_packet(const uint8_t *buffer, int bytes_read) {
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
                    || bytes_needed > P2_BUFFER_SIZE) {
                fprintf(stderr, "[UDP IQ] malformed packet dropped: samples-per-frame %d, %d bytes\n",
                        samplesperframe, bytes_read);
                return;
            }

            if (first_pkt) {
                expected_seq = seq;
                first_pkt = false;
            } else if (seq != expected_seq) {
                int32_t gap = (int32_t)(seq - expected_seq);

                // A negative gap is a late or duplicated datagram, which a
                // routed link can produce and a switched LAN rarely does.
                // Appending its samples would push every later sample forward
                // in time, so discard it and leave expected_seq alone.
                if (gap < 0) {
                    stats_add_reordered();
                    return;
                }

                fprintf(stderr, "[UDP IQ SEQ DROP] Expected %u, got %u (gap: %d packets)\n", expected_seq, seq, gap);

                // Zero-fill the missing packets. The radio's NCO free-runs, so
                // inserting exactly the right number of zeroed samples keeps
                // every later sample at its true instant - which is what makes
                // the recording phase-coherent across a drop. An inexact count
                // would corrupt the phase of everything that follows.
                if (gap > 0) {
                    int samples_per_pkt = samplesperframe / (diversity ? 2 : 1);
                    long long missing_quads = (long long)gap * samples_per_pkt;
                    int vals_per_quad = diversity ? 4 : 2;
                    long long want_vals = missing_quads * vals_per_quad;

                    // Pad as much as the ring can take. Anything beyond it is
                    // time we cannot represent, and it is reported rather than
                    // quietly swallowed.
                    pthread_mutex_lock(samples_resource.lock);
                    long long room = (long long)samples_resource.size - samples_resource.nused;
                    long long n_vals = want_vals <= room
                                       ? want_vals
                                       : (room / vals_per_quad) * vals_per_quad;
                    pthread_mutex_unlock(samples_resource.lock);

                    if (n_vals < want_vals) {
                        stats_add_slip((want_vals - n_vals) / vals_per_quad);
                    }
                    int total_missing_quads = (int)(n_vals / vals_per_quad);
                    stats_add_gap(gap, total_missing_quads);
                    
                    if (n_vals <= 0) {
                        note_overrun((int)missing_quads);
                        expected_seq = seq;
                        goto seq_resynced;
                    }

                    pthread_mutex_lock(samples_resource.lock);
                    int start_idx = samples_resource.write_index;
                    for (long long k = 0; k < n_vals; k++) {
                        insamples[samples_resource.write_index++] = 0;
                        if (samples_resource.write_index >= samples_resource.size) {
                            samples_resource.write_index = 0;
                        }
                    }
                    samples_resource.nused += (unsigned int)n_vals;
                    pthread_mutex_unlock(samples_resource.lock);

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
                    return;
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

static void *rx_thread_func(void *arg) {
    (void)arg;
    // Static: 64 x 1.5 KB is too large for the thread stack.
    static p2_packet_t batch[P2_BATCH_MAX];

    while (streaming_active) {
        int n = hpsdr_read_iq_batch(batch, P2_BATCH_MAX);
        if (n <= 0) {
            continue;   // receive timeout, or the socket was closed at shutdown
        }
        if (n > batch_max_seen) {
            batch_max_seen = n;
        }
        for (int i = 0; i < n; i++) {
            pkt_total++;
            pkt_by_port[port_slot(batch[i].src_port)]++;
            {
                int k;
                for (k = 0; k < seen_ips; k++) {
                    if (seen_ip[k] == batch[i].src_ip) break;
                }
                if (k < 4) {
                    if (k == seen_ips) { seen_ip[k] = batch[i].src_ip; seen_ips++; }
                    seen_ip_count[k]++;
                }
            }
            if (batch[i].src_port != RX_IQ_PORT && first_other_port < 0) {
                first_other_port = batch[i].src_port;
                first_other_len = batch[i].len < 16 ? batch[i].len : 16;
                memcpy(first_other, batch[i].data, (size_t)first_other_len);
            }
            // ONLY process IQ from the radio we are actually talking to. The
            // port alone is not enough: another HPSDR device on the same LAN
            // streaming to 1035 would otherwise be interleaved into the
            // recording, silently corrupting it.
            if (batch[i].src_port == RX_IQ_PORT && batch[i].src_ip != hpsdr_radio_ip()) {
                foreign_iq++;
                continue;
            }
            // Ignore status/register responses from ports 1024-1027.
            if (batch[i].src_port == RX_IQ_PORT && batch[i].len >= 16) {
                process_packet(batch[i].data, batch[i].len);
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
