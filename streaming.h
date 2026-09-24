#ifndef STREAMING_H
#define STREAMING_H

int stream(void);
void streaming_stop(void);

// Diagnostics for the "radio never streamed" case: what arrived, and from where.
long long streaming_iq_packets(void);
long long streaming_foreign_iq(void);
void streaming_port_report(void);

#endif // STREAMING_H
