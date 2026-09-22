#ifndef BUFFERS_H
#define BUFFERS_H

#include <pthread.h>
#include <stdint.h>

typedef struct {
    unsigned int first_sample_num;
    unsigned int num_samples;
    unsigned int samples_index;
} BlockDescriptor;

typedef struct {
    pthread_mutex_t *lock;
    void *resource;
    unsigned int read_index;
    unsigned int write_index;
    unsigned int size;
    unsigned int nused;
    unsigned int nused_max;
    unsigned int nready;
    pthread_cond_t *is_ready;
} ResourceDescriptor;

extern ResourceDescriptor blocks_resource;
extern ResourceDescriptor samples_resource;
extern int16_t *insamples;

int buffers_create();
void buffers_free();

#endif // BUFFERS_H
