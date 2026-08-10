#ifndef CVM_IR_TRANSLATOR_H
#define CVM_IR_TRANSLATOR_H

#include <stddef.h>

#define CVM_LLVM_TARGET_TRIPLE "cvm64-unknown-none"
#define CVM_LLVM_DATA_LAYOUT \
    "e-p:64:64-i8:8-i16:16-i32:32-i64:64-f32:32-f64:64-" \
    "v128:128-a:0:64-n8:16:32:64-S128"

typedef struct {
    size_t line;
    size_t column;
    char message[192];
} CvmIrError;

typedef struct {
    int allow_foreign_triple;
} CvmIrOptions;

/* Parse and verify LLVM text IR with LLVM 22, then lower supported IR to CVM .s. */
int cvmir_translate(const char *source,
                    const CvmIrOptions *options,
                    char **assembly,
                    CvmIrError *error);

#endif
