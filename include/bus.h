#ifndef BUS_H
#define BUS_H

#include <stddef.h>
#include <stdint.h>

#include "interrupt.h"
#include "ram.h"

#define BUS_MAX_DEVICES 64U

typedef int (*BusDeviceRead)(void *context,
                             uint64_t offset,
                             size_t width,
                             uint64_t *value);
typedef int (*BusDeviceWrite)(void *context,
                              uint64_t offset,
                              size_t width,
                              uint64_t value);
typedef void (*BusDeviceTick)(void *context,
                              uint64_t ticks,
                              InterruptRouter *interrupts);
typedef void (*BusDeviceReset)(void *context);

typedef struct {
    void *context;
    BusDeviceRead read;
    BusDeviceWrite write;
    BusDeviceTick tick;
    BusDeviceReset reset;
} BusDevice;

typedef struct {
    uint64_t base;
    uint64_t size;
    BusDevice device;
    atomic_flag lock;
    atomic_int present;
} BusMapping;

typedef struct {
    uint64_t base;
    uint64_t size;
    const uint8_t *data;
    int present;
} BusRomMapping;

typedef struct Bus {
    RAM *ram;
    BusMapping mappings[BUS_MAX_DEVICES];
    BusRomMapping rom;
    size_t mapping_count;
    atomic_flag mapping_table_lock;
} Bus;

int bus_init(Bus *bus, RAM *ram);
int bus_map_device(Bus *bus,
                   uint64_t base,
                   uint64_t size,
                   BusDevice device);
int bus_unmap_device(Bus *bus,
                     uint64_t base,
                     uint64_t size);
int bus_map_rom(Bus *bus,
                uint64_t base,
                const uint8_t *data,
                size_t size);
int bus_fetch(Bus *bus,
              uint64_t address,
              size_t width,
              uint64_t *value);
int bus_read(Bus *bus,
             uint64_t address,
             size_t width,
             uint64_t *value);
int bus_write(Bus *bus,
              uint64_t address,
              size_t width,
              uint64_t value);
int bus_compare_exchange64(Bus *bus,
                           uint64_t address,
                           uint64_t *expected,
                           uint64_t desired);
int bus_exchange64(Bus *bus,
                   uint64_t address,
                   uint64_t *value);
int bus_fetch_add64(Bus *bus,
                    uint64_t address,
                    uint64_t *value);
void bus_memory_fence(void);
void bus_tick(Bus *bus,
              uint64_t ticks,
              InterruptRouter *interrupts);
void bus_reset(Bus *bus);

#endif
