#ifndef CVM_OBJECT_ASSEMBLER_H
#define CVM_OBJECT_ASSEMBLER_H

#include "assembler.h"
#include "object_format.h"

int assembler_assemble_object(const char *source,
                              CvmObjectFile *object,
                              AssemblyError *error);

#endif
