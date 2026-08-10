#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

int cpu_internal_fetch_u8(CPU *cpu, RAM *ram, uint8_t *result);
int cpu_internal_data_read(CPU *cpu,
                           RAM *ram,
                           uint64_t address,
                           size_t width,
                           uint64_t *value);
int cpu_internal_data_write(CPU *cpu,
                            RAM *ram,
                            uint64_t address,
                            size_t width,
                            uint64_t value);
int cpu_internal_vector_read(CPU *cpu,
                             RAM *ram,
                             uint64_t address,
                             uint8_t value[VECTOR_REGISTER_SIZE]);
int cpu_internal_vector_write(CPU *cpu,
                              RAM *ram,
                              uint64_t address,
                              const uint8_t value[VECTOR_REGISTER_SIZE]);
int cpu_internal_illegal_instruction(CPU *cpu);

#endif
