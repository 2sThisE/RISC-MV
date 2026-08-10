#ifndef CVM_ARCHIVE_FORMAT_H
#define CVM_ARCHIVE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define CVM_ARCHIVE_MAGIC "CVMAR1\0\0"
#define CVM_ARCHIVE_VERSION UINT32_C(1)
#define CVM_ARCHIVE_MAX_MEMBERS UINT32_C(4096)
#define CVM_ARCHIVE_MAX_SYMBOLS UINT32_C(1048576)
#define CVM_ARCHIVE_MAX_SIZE ((size_t)256 * 1024 * 1024)

enum {
    CVM_ARCHIVE_SYMBOL_ENTRY = 1u << 0
};

typedef struct {
    char *name;
    uint8_t *data;
    size_t size;
} CvmArchiveMember;

typedef struct {
    char *name;
    uint32_t member_index;
    uint32_t flags;
} CvmArchiveSymbol;

typedef struct {
    CvmArchiveMember *members;
    size_t member_count;
    CvmArchiveSymbol *symbols;
    size_t symbol_count;
} CvmArchive;

void cvm_archive_destroy(CvmArchive *archive);
int cvm_archive_write(const char *path, const CvmArchive *archive,
                      char *error, size_t error_size);
int cvm_archive_read(const char *path, CvmArchive *archive,
                     char *error, size_t error_size);

#endif
