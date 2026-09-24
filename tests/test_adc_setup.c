// Dumps the bytes that actually configure ADC1 (ch2), by wrapping sendto()
// and decoding by destination port. Links the real hpsdr-protocol2.c.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "config.h"
#include "hpsdr-protocol2.h"

ssize_t __wrap_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *addr, socklen_t alen) {
    (void)fd; (void)flags; (void)alen;
    const uint8_t *b = buf;
    int port = ntohs(((const struct sockaddr_in *)addr)->sin_port);

    if (port == RECEIVER_SPECIFIC_REGISTERS_PORT) {
        printf("  RX-specific packet (port %d):\n", port);
        printf("    [4]    ADCs in use          = %u\n", b[4]);
        printf("    [5]    dither  bit0=ADC0 bit1=ADC1 = 0x%02X\n", b[5]);
        printf("    [6]    random  bit0=ADC0 bit1=ADC1 = 0x%02X\n", b[6]);
        printf("    [7]    DDC enable bitmap    = 0x%02X\n", b[7]);
        printf("    DDC0: ADC=%u rate=%u bits=%u\n",
               b[17], (unsigned)((b[18] << 8) | b[19]) * 1000u, b[22]);
        printf("    DDC1: ADC=%u rate=%u bits=%u   <- ch2\n",
               b[23], (unsigned)((b[24] << 8) | b[25]) * 1000u, b[26]);
        printf("    [1363] DDC sync              = 0x%02X\n", b[1363]);
    } else if (port == HIGH_PRIORITY_PORT) {
        printf("  High-priority packet (port %d):\n", port);
        printf("    [1443] ADC0 step attenuator  = %u dB\n", b[1443]);
        printf("    [1442] ADC1 step attenuator  = %u dB   <- ch2\n", b[1442]);
    }
    return (ssize_t)len;
}

static void show(const char *label, int atten) {
    attenuation = atten;
    printf("%s\n", label);
    hpsdr_configure_receiver(1536000, true, 0, 0);
    hpsdr_configure_high_priority(950000, 950000);
    printf("\n");
}

int main(void) {
    diversity = true;
    if (hpsdr_connect("127.0.0.1") < 0) { printf("connect failed\n"); return 1; }
    rx_antenna = RX_ANT_ANT1;
    show("=== as configured for your recordings (attenuation = 0) ===", 0);
    show("=== proving the knob reaches ADC1 (attenuation = 10) ===", 10);
    return 0;
}
