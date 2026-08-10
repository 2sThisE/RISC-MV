#ifndef VM_ASSEMBLER_ENCODER_H
#define VM_ASSEMBLER_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "isa.h"

typedef enum {
    ASM_FORMAT_NONE = 0,
    ASM_FORMAT_R,
    ASM_FORMAT_RR,
    ASM_FORMAT_RRR,
    ASM_FORMAT_RRRR,
    ASM_FORMAT_R_IMM64,
    ASM_FORMAT_R_IMM32U,
    ASM_FORMAT_R_IMM32S,
    ASM_FORMAT_R_U8,
    ASM_FORMAT_TARGET64,
    ASM_FORMAT_RR_DISP32,
    ASM_FORMAT_REL32,
    ASM_FORMAT_CC_REL32,
    ASM_FORMAT_V_R,
    ASM_FORMAT_R_V,
    ASM_FORMAT_VV,
    ASM_FORMAT_VVV
} AsmInstructionFormat;

typedef struct {
    const char *name;
    uint8_t opcode;
    AsmInstructionFormat format;
} AsmInstructionSpec;

const AsmInstructionSpec *asm_instruction_find(const char *name);
size_t asm_instruction_size(AsmInstructionFormat format);
int asm_parse_register(const char *text, uint8_t *reg);
int asm_parse_vector_register(const char *text, uint8_t *reg);
int asm_parse_condition(const char *text, uint8_t *condition);
int asm_text_equal_ignore_case(const char *left, const char *right);

#endif
