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
int hpsdr_read_iq(uint8_t *buffer, int *bytes_read, uint16_t *src_port);

#endif // HPSDR_PROTOCOL2_H
