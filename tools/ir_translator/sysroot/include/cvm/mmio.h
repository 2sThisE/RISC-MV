#ifndef _CVM_MMIO_H
#define _CVM_MMIO_H

#include <stdint.h>

static inline uint8_t cvm_mmio_read8(uintptr_t address)
{
    return *(volatile uint8_t *)address;
}

static inline uint16_t cvm_mmio_read16(uintptr_t address)
{
    return *(volatile uint16_t *)address;
}

static inline uint32_t cvm_mmio_read32(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static inline uint64_t cvm_mmio_read64(uintptr_t address)
{
    return *(volatile uint64_t *)address;
}

static inline void cvm_mmio_write8(uintptr_t address, uint8_t value)
{
    *(volatile uint8_t *)address = value;
}

static inline void cvm_mmio_write16(uintptr_t address, uint16_t value)
{
    *(volatile uint16_t *)address = value;
}

static inline void cvm_mmio_write32(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

static inline void cvm_mmio_write64(uintptr_t address, uint64_t value)
{
    *(volatile uint64_t *)address = value;
}

#endif
