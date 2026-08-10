#ifndef CPU_VECTOR_H
#define CPU_VECTOR_H

#include <stdint.h>

#include "cpu.h"

int cpu_vector_opcode(uint8_t opcode);
int cpu_vector_execute(CPU *cpu, RAM *ram, uint8_t opcode);

#endif
