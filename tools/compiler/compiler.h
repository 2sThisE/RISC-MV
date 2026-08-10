#ifndef CVM_COMPILER_H
#define CVM_COMPILER_H

#include <stddef.h>

typedef struct {
    size_t line;
    size_t column;
    char message[192];
} CvmCompilerError;

/* The returned assembly buffer belongs to the caller and must be freed. */
int cvm_compile_c_source(const char *source,
                         char **assembly,
                         CvmCompilerError *error);

#endif
