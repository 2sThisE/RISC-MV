#ifndef VM_ASSEMBLER_PREPROCESSOR_H
#define VM_ASSEMBLER_PREPROCESSOR_H

#include "assembler.h"

int asm_preprocess_file(const char *path, char **output, AssemblyError *error);

#endif
