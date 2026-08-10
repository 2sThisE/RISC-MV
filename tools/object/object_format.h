#ifndef CVM_OBJECT_FORMAT_H
#define CVM_OBJECT_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define CVM_OBJECT_MAGIC "CVMOBJ1\0"
#define CVM_OBJECT_VERSION UINT32_C(1)
#define CVM_OBJECT_MAX_SECTIONS UINT32_C(32)
#define CVM_OBJECT_MAX_SYMBOLS UINT32_C(65536)
#define CVM_OBJECT_MAX_SIZE ((size_t)64 * 1024 * 1024)

enum {
    CVM_OBJECT_SECTION_ALLOC = 1u << 0,
    CVM_OBJECT_SECTION_WRITE = 1u << 1,
    CVM_OBJECT_SECTION_EXECUTE = 1u << 2,
    CVM_OBJECT_SECTION_NOBITS = 1u << 3
};

enum {
    CVM_OBJECT_SYMBOL_GLOBAL = 1u << 0,
    CVM_OBJECT_SYMBOL_DEFINED = 1u << 1,
    CVM_OBJECT_SYMBOL_ENTRY = 1u << 2
};

#define CVM_OBJECT_UNDEFINED_SECTION UINT16_MAX

typedef struct {
    char *name;
    uint32_t flags;
    uint64_t alignment;
    char *source;
    size_t source_size;
} CvmObjectSection;

typedef struct {
    char *name;
    uint16_t section_index;
    uint16_t flags;
} CvmObjectSymbol;

typedef struct {
    CvmObjectSection *sections;
    size_t section_count;
    CvmObjectSymbol *symbols;
    size_t symbol_count;
} CvmObjectFile;

void cvm_object_destroy(CvmObjectFile *object);
int cvm_object_write(const char *path,
                     const CvmObjectFile *object,
                     char *error,
                     size_t error_size);
int cvm_object_read(const char *path,
                    CvmObjectFile *object,
                    char *error,
                    size_t error_size);

#endif
