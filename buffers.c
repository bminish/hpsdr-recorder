#include "buffers.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

ResourceDescriptor blocks_resource;
ResourceDescriptor samples_resource;
int16_t *insamples = NULL;

static BlockDescriptor *blocks;
static pthread_mutex_t blocks_lock;
static pthread_cond_t is_ready;
static pthread_mutex_t samples_lock;

int buffers_create() {
    pthread_mutex_init(&blocks_lock, NULL);
    pthread_cond_init(&is_ready, NULL);
    
    blocks = (BlockDescriptor *)malloc(blocks_buffer_capacity * sizeof(BlockDescriptor));
    if (!blocks) return -1;
    
    blocks_resource = (ResourceDescriptor) {
        .lock = &blocks_lock,
        .resource = blocks,
        .read_index = 0,
        .write_index = 0,
        .size = blocks_buffer_capacity,
        .nused = 0,
        .nused_max = 0,
        .nready = 0,
        .is_ready = &is_ready,
    };

    pthread_mutex_init(&samples_lock, NULL);
    insamples = (int16_t *)malloc(samples_buffer_capacity * sizeof(int16_t));
    if (!insamples) return -1;
    
    samples_resource = (ResourceDescriptor) {
        .lock = &samples_lock,
        .resource = insamples,
        .read_index = 0,
        .write_index = 0,
        .size = samples_buffer_capacity,
        .nused = 0,
        .nused_max = 0,
        .nready = 0,
        .is_ready = NULL,
    };

    return 0;
}

void buffers_free() {
    if (insamples) {
        free(insamples);
        insamples = NULL;
    }
    if (blocks) {
        free(blocks);
        blocks = NULL;
    }
}
