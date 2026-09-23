#define _GNU_SOURCE
#include "hpsdr-protocol2.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>

static int data_socket = -1;
static struct sockaddr_in radio_addr_general;
static struct sockaddr_in radio_addr_rx;
static struct sockaddr_in radio_addr_tx;
static struct sockaddr_in radio_addr_hp;

static int rcvbuf_granted = 0;
static uint32_t last_rxq_ovfl = 0;
static unsigned long long sock_drops_total = 0;

static uint32_t general_sequence = 0;
static uint32_t rx_sequence = 0;
static uint32_t tx_sequence = 0;
static uint32_t hp_sequence = 0;

static pthread_t keepalive_thread;
static volatile bool keepalive_active = false;
static uint32_t current_sample_rate = 1536000;
static bool current_diversity = true;
static int current_freq1 = 990000;
static int current_freq2 = 990000;

int hpsdr_discover(const char *target_ip, hpsdr_device_t *device_out) {
    int sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    if (target_ip == NULL || strcmp(target_ip, "255.255.255.255") == 0) {
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));
    }

    struct sockaddr_in to_addr;
    memset(&to_addr, 0, sizeof(to_addr));
    to_addr.sin_family = AF_INET;
    to_addr.sin_port = htons(DISCOVERY_PORT);
    
    if (target_ip) {
        inet_aton(target_ip, &to_addr.sin_addr);
    } else {
        to_addr.sin_addr.s_addr = INADDR_BROADCAST;
    }

    uint8_t buffer[60];
    memset(buffer, 0, sizeof(buffer));
    buffer[4] = 0x02; // protocol 2 discovery

    if (sendto(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&to_addr, sizeof(to_addr)) < 0) {
        perror("sendto discovery");
        close(sock);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int bytes_read = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
    
    close(sock);

    if (bytes_read < 0) {
        return -1; // Timeout or error
    }

    if (buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0 && (buffer[4] == 2 || buffer[4] == 3)) {
        if (device_out) {
            strncpy(device_out->ip_addr, inet_ntoa(from_addr.sin_addr), sizeof(device_out->ip_addr) - 1);
            device_out->ip_addr[sizeof(device_out->ip_addr) - 1] = '\0';
            memcpy(device_out->mac, &buffer[5], 6);
            device_out->device_id = buffer[11];
            device_out->software_version = buffer[13];
        }
        return 0;
    }

    return -1;
}

int hpsdr_connect(const char *ip_addr) {
    data_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (data_socket < 0) {
        perror("socket");
        return -1;
    }

    int optval = 1;
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    setsockopt(data_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    
    // Maximize receive buffer for deep buffering. The kernel silently caps this
    // at net.core.rmem_max, so read back what we actually got: a request for
    // 16 MB commonly lands at 5 MB, which is only ~280 ms of this stream.
    int rcvbuf = 1024 * 1024 * 16; // 16MB
    setsockopt(data_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    socklen_t optlen = sizeof(rcvbuf_granted);
    if (getsockopt(data_socket, SOL_SOCKET, SO_RCVBUF, &rcvbuf_granted, &optlen) == 0) {
        rcvbuf_granted /= 2; // kernel reports double the usable size
    }

    // Ask the kernel to tell us how many datagrams it dropped for this socket,
    // which separates "we were too slow" from "it never arrived".
    int on = 1;
    setsockopt(data_socket, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(data_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(RX_IQ_PORT);

    if (bind(data_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind rx iq port");
        close(data_socket);
        return -1;
    }

    memset(&radio_addr_general, 0, sizeof(radio_addr_general));
    radio_addr_general.sin_family = AF_INET;
    inet_aton(ip_addr, &radio_addr_general.sin_addr);
    radio_addr_general.sin_port = htons(GENERAL_REGISTERS_PORT);

    memset(&radio_addr_rx, 0, sizeof(radio_addr_rx));
    radio_addr_rx.sin_family = AF_INET;
    inet_aton(ip_addr, &radio_addr_rx.sin_addr);
    radio_addr_rx.sin_port = htons(RECEIVER_SPECIFIC_REGISTERS_PORT);

    memset(&radio_addr_tx, 0, sizeof(radio_addr_tx));
    radio_addr_tx.sin_family = AF_INET;
    inet_aton(ip_addr, &radio_addr_tx.sin_addr);
    radio_addr_tx.sin_port = htons(TRANSMITTER_SPECIFIC_REGISTERS_PORT);

    memset(&radio_addr_hp, 0, sizeof(radio_addr_hp));
    radio_addr_hp.sin_family = AF_INET;
    inet_aton(ip_addr, &radio_addr_hp.sin_addr);
    radio_addr_hp.sin_port = htons(HIGH_PRIORITY_PORT);

    return 0;
}

int hpsdr_configure_general(bool enable_pa, bool enable_alex) {
    uint8_t buf[60];
    memset(buf, 0, sizeof(buf));
    
    buf[0] = (general_sequence >> 24) & 0xFF;
    buf[1] = (general_sequence >> 16) & 0xFF;
    buf[2] = (general_sequence >> 8) & 0xFF;
    buf[3] = (general_sequence) & 0xFF;
    general_sequence++;
    
    buf[37] = 0x08; // phase word
    buf[38] = 0x01; // enable hardware timer
    
    buf[58] = enable_pa ? 0x01 : 0x00; // PA Disabled
    buf[59] = 0x01; // Enable Alex Filter Board 0
    
    if (sendto(data_socket, buf, sizeof(buf), 0, (struct sockaddr *)&radio_addr_general, sizeof(radio_addr_general)) != sizeof(buf)) {
        return -1;
    }
    return 0;
}

int hpsdr_configure_receiver(uint32_t sample_rate, bool diversity_enabled, uint8_t atten_rx1, uint8_t atten_rx2) {
    (void)atten_rx1;
    (void)atten_rx2;
    uint8_t buf[1444];
    memset(buf, 0, sizeof(buf));
    
    buf[0] = (rx_sequence >> 24) & 0xFF;
    buf[1] = (rx_sequence >> 16) & 0xFF;
    buf[2] = (rx_sequence >> 8) & 0xFF;
    buf[3] = (rx_sequence) & 0xFF;
    rx_sequence++;
    
    uint8_t n_adc = diversity_enabled ? 2 : 1;
    buf[4] = n_adc;
    
    // In Protocol 2, for diversity or normal RX1, only enable DDC0 (bit 0)
    // When diversity is enabled (buf[1363] = 0x02), DDC0 streams both ADC0 and ADC1 interleaved on port 1035!
    buf[7] = 1; 

    // Receiver 0
    buf[17] = 0; // ADC0
    buf[18] = ((sample_rate / 1000) >> 8) & 0xFF;
    buf[19] = (sample_rate / 1000) & 0xFF;
    buf[22] = 24; // 24 bits
    
    if (diversity_enabled) {
        // Receiver 1
        buf[23] = 1; // ADC1
        buf[24] = ((sample_rate / 1000) >> 8) & 0xFF;
        buf[25] = (sample_rate / 1000) & 0xFF;
        buf[26] = 24; // 24 bits
        
        buf[1363] = 0x02; // Sync DDC1 to DDC0
    }
    
    if (sendto(data_socket, buf, sizeof(buf), 0, (struct sockaddr *)&radio_addr_rx, sizeof(radio_addr_rx)) != sizeof(buf)) {
        return -1;
    }
    return 0;
}

int hpsdr_configure_transmitter_disabled(void) {
    uint8_t buf[60];
    memset(buf, 0, sizeof(buf));
    
    buf[0] = (tx_sequence >> 24) & 0xFF;
    buf[1] = (tx_sequence >> 16) & 0xFF;
    buf[2] = (tx_sequence >> 8) & 0xFF;
    buf[3] = (tx_sequence) & 0xFF;
    tx_sequence++;
    
    buf[4] = 1; // 1 DAC
    buf[5] = 0; // No CW
    
    // Explicitly set TX drive and gains to 0, though memset already does it.
    
    if (sendto(data_socket, buf, sizeof(buf), 0, (struct sockaddr *)&radio_addr_tx, sizeof(radio_addr_tx)) != sizeof(buf)) {
        return -1;
    }
    return 0;
}

// DDC NCO phase word: freq * 2^32 / 122.88 MHz, rounded to nearest.
//
// The DDC tunes exactly at this frequency; freq_correction defaults to 0 and
// exists only for calibration. Verified against the 9 kHz MW carrier grid: with
// the phase word encoding 951000 Hz, 909 and 990 kHz land within 1 Hz of grid.
//
// Do NOT add an offset here to chase an apparent frequency error. A constant
// offset in a 9 kHz-grid view is the signature of spectral inversion, not
// mistuning - see the conjugation in streaming.c. An inverted spectrum recorded
// at centre C makes stations read (2C - f), i.e. an apparent shift of
// 2*(C mod 9000), which is invisible at 990 kHz but looks like 1 kHz at 950 kHz.
static uint32_t ddc_phase_word(int freq_hz) {
    double f = (double)freq_hz + (double)freq_correction;
    if (f < 0.0) f = 0.0;
    return (uint32_t)((f * 4294967296.0 / 122880000.0) + 0.5);
}

int hpsdr_configure_high_priority(int freq1_hz, int freq2_hz) {
    uint8_t buf[1444];
    memset(buf, 0, sizeof(buf));
    
    buf[0] = (hp_sequence >> 24) & 0xFF;
    buf[1] = (hp_sequence >> 16) & 0xFF;
    buf[2] = (hp_sequence >> 8) & 0xFF;
    buf[3] = (hp_sequence) & 0xFF;
    hp_sequence++;
    
    buf[4] = 1; // P2running = 1
    
    uint32_t phase0 = ddc_phase_word(freq1_hz);
    buf[9] = (phase0 >> 24) & 0xFF;
    buf[10] = (phase0 >> 16) & 0xFF;
    buf[11] = (phase0 >> 8) & 0xFF;
    buf[12] = (phase0) & 0xFF;
    
    uint32_t phase1 = ddc_phase_word(freq2_hz);
    buf[13] = (phase1 >> 24) & 0xFF;
    buf[14] = (phase1 >> 16) & 0xFF;
    buf[15] = (phase1 >> 8) & 0xFF;
    buf[16] = (phase1) & 0xFF;
    
    // Copy DDC0/1 to DDC2/3 for Angelia/Orion board compatibility
    buf[17] = buf[9];
    buf[18] = buf[10];
    buf[19] = buf[11];
    buf[20] = buf[12];
    buf[21] = buf[13];
    buf[22] = buf[14];
    buf[23] = buf[15];
    buf[24] = buf[16];

    // Compute automatic Alex HPF & LPF filter selection based on tuning frequency
    uint32_t alex0 = 0;
    uint32_t alex1 = 0;

    // ANT1/2/3 carry the RX signal through the TX low-pass filters on this
    // board (pre-Orion2), so the LPF must be chosen for the RX frequency.
    // EXT1/EXT2/XVTR are receive-only inputs that do not, so the LPF is
    // bypassed for them - same rule piHPSDR applies while receiving.
    bool rx_via_main_ant = (rx_antenna <= RX_ANT_ANT3);
    int lpf_freq_hz = rx_via_main_ant ? freq1_hz : 40000000;

    // HPF selection for RX (based on freq1_hz)
    if (freq1_hz < 1800000) {
        alex0 |= 0x00001000; // ALEX_BYPASS_HPF (For 990 kHz AM Broadcast)
    } else if (freq1_hz < 6500000) {
        alex0 |= 0x00000040; // ALEX_1_5MHZ_HPF
    } else if (freq1_hz < 9500000) {
        alex0 |= 0x00000020; // ALEX_6_5MHZ_HPF
    } else if (freq1_hz < 13000000) {
        alex0 |= 0x00000010; // ALEX_9_5MHZ_HPF
    } else if (freq1_hz < 20000000) {
        alex0 |= 0x00000002; // ALEX_13MHZ_HPF
    } else if (freq1_hz < 50000000) {
        alex0 |= 0x00000004; // ALEX_20MHZ_HPF
    } else {
        alex0 |= 0x00000008; // ALEX_6M_PREAMP
    }

    // LPF selection for RX (based on the antenna in use, see above)
    if (lpf_freq_hz > 35600000) {
        alex0 |= 0x20000000; // ALEX_6_BYPASS_LPF
        alex1 |= 0x20000000;
    } else if (lpf_freq_hz > 24000000) {
        alex0 |= 0x40000000; // ALEX_12_10_LPF
        alex1 |= 0x40000000;
    } else if (lpf_freq_hz > 16500000) {
        alex0 |= 0x80000000; // ALEX_17_15_LPF
        alex1 |= 0x80000000;
    } else if (lpf_freq_hz > 8000000) {
        alex0 |= 0x00100000; // ALEX_30_20_LPF
        alex1 |= 0x00100000;
    } else if (lpf_freq_hz > 5000000) {
        alex0 |= 0x00200000; // ALEX_60_40_LPF
        alex1 |= 0x00200000;
    } else if (lpf_freq_hz > 2500000) {
        alex0 |= 0x00400000; // ALEX_80_LPF
        alex1 |= 0x00400000;
    } else {
        alex0 |= 0x00800000; // ALEX_160_LPF (For 990 kHz AM Broadcast)
        alex1 |= 0x00800000;
    }

    // Antenna routing. For ANT1/2/3 the "TX antenna" bits in alex0 are what
    // selects the RX jack; for EXT1/EXT2/XVTR they stay on ANT1 and an extra
    // routing bit selects the receive-only input. The BYPASS bit is only needed
    // on the pre-Rev.24 PA board - this radio reports new_pa_board=1.
    switch (rx_via_main_ant ? rx_antenna : RX_ANT_ANT1) {
        case RX_ANT_ANT2: alex0 |= 0x02000000; break; // ALEX_TX_ANTENNA_2
        case RX_ANT_ANT3: alex0 |= 0x04000000; break; // ALEX_TX_ANTENNA_3
        default:          alex0 |= 0x01000000; break; // ALEX_TX_ANTENNA_1
    }
    if (!rx_via_main_ant) {
        switch (rx_antenna) {
            case RX_ANT_EXT1: alex0 |= 0x00000200; break; // ALEX_RX_ANTENNA_EXT1
            case RX_ANT_EXT2: alex0 |= 0x00000400; break; // ALEX_RX_ANTENNA_EXT2
            case RX_ANT_XVTR: alex0 |= 0x00000100; break; // ALEX_RX_ANTENNA_XVTR
        }
        if (!alex_new_pa_board) {
            alex0 |= 0x00000800; // ALEX_RX_ANTENNA_BYPASS
        }
    }
    alex1 |= 0x01000000; // ALEX_TX_ANTENNA_1 (TX register, inert while receiving)

    buf[1432] = (alex0 >> 24) & 0xFF;
    buf[1433] = (alex0 >> 16) & 0xFF;
    buf[1434] = (alex0 >> 8) & 0xFF;
    buf[1435] = alex0 & 0xFF;

    buf[1428] = (alex1 >> 24) & 0xFF;
    buf[1429] = (alex1 >> 16) & 0xFF;
    buf[1430] = (alex1 >> 8) & 0xFF;
    buf[1431] = alex1 & 0xFF;

    // Step attenuators: 1443 is ADC0, 1442 is ADC1. Both ADCs get the same value
    // in diversity, otherwise the two channels no longer match in amplitude and
    // the phasing analysis is meaningless (piHPSDR forces this too).
    buf[1443] = (uint8_t)attenuation;
    buf[1442] = (uint8_t)attenuation;
    
    if (sendto(data_socket, buf, sizeof(buf), 0, (struct sockaddr *)&radio_addr_hp, sizeof(radio_addr_hp)) != sizeof(buf)) {
        return -1;
    }
    return 0;
}

static void *keepalive_thread_func(void *arg) {
    (void)arg;
    int cycling = 0;
    while (keepalive_active) {
        cycling++;
        switch (cycling) {
        case 1:
        case 3:
        case 5:
        case 7:
            hpsdr_configure_transmitter_disabled();
            hpsdr_configure_high_priority(current_freq1, current_freq2);
            break;
        case 2:
        case 4:
        case 6:
            hpsdr_configure_receiver(current_sample_rate, current_diversity, 0, 0);
            hpsdr_configure_high_priority(current_freq1, current_freq2);
            break;
        case 8:
            hpsdr_configure_general(false, false);
            hpsdr_configure_receiver(current_sample_rate, current_diversity, 0, 0);
            hpsdr_configure_high_priority(current_freq1, current_freq2);
            cycling = 0;
            break;
        }
        usleep(100000); // 100 ms
    }
    return NULL;
}

void hpsdr_start_keepalive(uint32_t sample_rate, bool diversity_enabled, int freq1_hz, int freq2_hz) {
    current_sample_rate = sample_rate;
    current_diversity = diversity_enabled;
    current_freq1 = freq1_hz;
    current_freq2 = freq2_hz;
    if (!keepalive_active) {
        keepalive_active = true;
        pthread_create(&keepalive_thread, NULL, keepalive_thread_func, NULL);
    }
}

void hpsdr_close(void) {
    if (keepalive_active) {
        keepalive_active = false;
        pthread_join(keepalive_thread, NULL);
    }
    if (data_socket != -1) {
        close(data_socket);
        data_socket = -1;
    }
}

int hpsdr_socket_rcvbuf(void) {
    return rcvbuf_granted;
}

unsigned long long hpsdr_socket_drops(void) {
    return sock_drops_total;
}

// Receives up to max_pkts datagrams in one syscall. MSG_WAITFORONE returns as
// soon as at least one has arrived, so latency is unchanged when the link is
// quiet but syscalls collapse by ~60x when it is busy.
int hpsdr_read_iq_batch(p2_packet_t *pkts, int max_pkts) {
    if (data_socket < 0) return -1;
    if (max_pkts > P2_BATCH_MAX) max_pkts = P2_BATCH_MAX;

    struct mmsghdr msgs[P2_BATCH_MAX];
    struct iovec iovs[P2_BATCH_MAX];
    struct sockaddr_in addrs[P2_BATCH_MAX];
    char ctrl[P2_BATCH_MAX][CMSG_SPACE(sizeof(uint32_t))];

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < max_pkts; i++) {
        iovs[i].iov_base = pkts[i].data;
        iovs[i].iov_len = P2_BUFFER_SIZE;
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
        msgs[i].msg_hdr.msg_control = ctrl[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(ctrl[i]);
    }

    int n = recvmmsg(data_socket, msgs, max_pkts, MSG_WAITFORONE, NULL);
    if (n <= 0) {
        return n;
    }

    for (int i = 0; i < n; i++) {
        pkts[i].len = (int)msgs[i].msg_len;
        pkts[i].src_port = ntohs(addrs[i].sin_port);
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msgs[i].msg_hdr); c != NULL;
             c = CMSG_NXTHDR(&msgs[i].msg_hdr, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                uint32_t v;
                memcpy(&v, CMSG_DATA(c), sizeof(v));
                if (v != last_rxq_ovfl) {
                    sock_drops_total += (unsigned long long)(uint32_t)(v - last_rxq_ovfl);
                    last_rxq_ovfl = v;
                }
            }
        }
    }
    return n;
}
