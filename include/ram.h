#ifndef RAM_H
#define RAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define RAM_LOCK_STRIPE_COUNT 256U
#define RAM_LOCK_GRANULARITY 64U

typedef struct {
    uint8_t *data;
    size_t size;
    atomic_flag locks[RAM_LOCK_STRIPE_COUNT];
    int synchronization_ready;
} RAM;

int parse_ram_size(const char *text, size_t *result);
int ram_enable_synchronization(RAM *ram);
int ram_read(RAM *ram,
             uint64_t address,
             size_t width,
             uint64_t *value);
int ram_write(RAM *ram,
              uint64_t address,
              size_t width,
              uint64_t value);
int ram_read_block(RAM *ram,
                   uint64_t address,
                   void *buffer,
                   size_t size);
int ram_write_block(RAM *ram,
                    uint64_t address,
                    const void *buffer,
                    size_t size);
int ram_compare_exchange64(RAM *ram,
                           uint64_t address,
                           uint64_t *expected,
                           uint64_t desired);
int ram_exchange64(RAM *ram,
                   uint64_t address,
                   uint64_t *value);
int ram_fetch_add64(RAM *ram,
                    uint64_t address,
                    uint64_t *value);
void ram_memory_fence(void);

#endif
