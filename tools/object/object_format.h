#ifndef CVM_OBJECT_FORMAT_H
#define CVM_OBJECT_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define CVM_OBJECT_MAGIC "CVMOBJ2\0"
#define CVM_OBJECT_VERSION UINT32_C(2)
#define CVM_OBJECT_MAX_SECTIONS UINT32_C(32)
#define CVM_OBJECT_MAX_SYMBOLS UINT32_C(65536)
#define CVM_OBJECT_MAX_RELOCATIONS UINT32_C(1048576)
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
    CVM_OBJECT_SYMBOL_ENTRY = 1u << 2,
    CVM_OBJECT_SYMBOL_WEAK = 1u << 3,
    CVM_OBJECT_SYMBOL_FUNCTION = 1u << 4,
    CVM_OBJECT_SYMBOL_OBJECT = 1u << 5,
    CVM_OBJECT_SYMBOL_COMMON = 1u << 6
};

enum {
    CVM_OBJECT_RELOCATION_ABS32 = 1,
    CVM_OBJECT_RELOCATION_ABS64 = 2,
    CVM_OBJECT_RELOCATION_REL32 = 3
};

#define CVM_OBJECT_UNDEFINED_SECTION UINT16_MAX
#define CVM_OBJECT_ABSOLUTE_SECTION (UINT16_MAX - UINT16_C(1))
#define CVM_OBJECT_COMMON_SECTION (UINT16_MAX - UINT16_C(2))

typedef struct {
    char *name;
    uint32_t flags;
    uint64_t alignment;
    uint8_t *data;
    size_t file_size;
    uint64_t memory_size;
} CvmObjectSection;

typedef struct {
    char *name;
    uint16_t section_index;
    uint16_t flags;
    uint64_t value;
    uint64_t size;
} CvmObjectSymbol;

typedef struct {
    uint16_t section_index;
    uint16_t type;
    uint32_t symbol_index;
    uint64_t offset;
    int64_t addend;
} CvmObjectRelocation;

typedef struct {
    CvmObjectSection *sections;
    size_t section_count;
    CvmObjectSymbol *symbols;
    size_t symbol_count;
    CvmObjectRelocation *relocations;
    size_t relocation_count;
} CvmObjectFile;

size_t cvm_object_relocation_width(uint16_t type);
void cvm_object_destroy(CvmObjectFile *object);
int cvm_object_write(const char *path,
                     const CvmObjectFile *object,
                     char *error,
                     size_t error_size);
int cvm_object_read(const char *path,
                    CvmObjectFile *object,
                    char *error,
                    size_t error_size);
int cvm_object_decode(const void *data,
                      size_t data_size,
                      CvmObjectFile *object,
                      char *error,
                      size_t error_size);

#endif
