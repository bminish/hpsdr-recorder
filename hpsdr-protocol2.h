#ifndef HPSDR_PROTOCOL2_H
#define HPSDR_PROTOCOL2_H

#include <stdint.h>
#include <stdbool.h>

#define DISCOVERY_PORT 1024
#define GENERAL_REGISTERS_PORT 1024
#define RECEIVER_SPECIFIC_REGISTERS_PORT 1025
#define TRANSMITTER_SPECIFIC_REGISTERS_PORT 1026
#define HIGH_PRIORITY_PORT 1027
#define RX_IQ_PORT 1035

#define P2_BUFFER_SIZE 1500
#define P2_BATCH_MAX 64

// One received datagram. Batching with recvmmsg() matters here: at 1536 kHz
// diversity the radio sends ~12900 packets/s, and one syscall per packet is
// the difference between keeping up and filling the socket buffer.
typedef struct {
    uint8_t data[P2_BUFFER_SIZE];
    int len;
    uint16_t src_port;
    uint32_t src_ip;      // network byte order
} p2_packet_t;

typedef struct {
    char name[32];
    char ip_addr[32];
    uint8_t mac[6];
    uint32_t device_id;
    uint32_t software_version;
} hpsdr_device_t;

int hpsdr_discover(const char *target_ip, hpsdr_device_t *device_out);
int hpsdr_connect(const char *ip_addr);
int hpsdr_configure_general(bool enable_pa, bool enable_alex);
int hpsdr_configure_receiver(uint32_t sample_rate, bool diversity_enabled, uint8_t atten_rx1, uint8_t atten_rx2);
int hpsdr_configure_transmitter_disabled(void);
int hpsdr_configure_high_priority(int freq1_hz, int freq2_hz);
void hpsdr_start_keepalive(uint32_t sample_rate, bool diversity_enabled, int freq1_hz, int freq2_hz);
void hpsdr_close(void);

// Data polling
int hpsdr_read_iq_batch(p2_packet_t *pkts, int max_pkts);
int hpsdr_socket_rcvbuf(void);
uint32_t hpsdr_radio_ip(void);
unsigned long long hpsdr_socket_drops(void);

#endif // HPSDR_PROTOCOL2_H
