#ifndef BOOT_FORMAT_H
#define BOOT_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define RISC_MV_EXF_MAGIC "RMVEXF01"
#define CVM_BOOTINFO_MAGIC "CVMBOOT1"

#define RISC_MV_EXF_VERSION_MAJOR UINT16_C(1)
#define RISC_MV_EXF_VERSION_MINOR UINT16_C(0)
#define CVM_BOOTINFO_VERSION_MAJOR UINT16_C(1)
#define CVM_BOOTINFO_VERSION_MINOR UINT16_C(0)

#define RISC_MV_EXF_HEADER_SIZE ((size_t)128)
#define RISC_MV_EXF_SEGMENT_SIZE ((size_t)64)
#define RISC_MV_EXF_MAX_SEGMENTS UINT16_C(64)
#define CVM_KERNEL_HEADER_SIZE RISC_MV_EXF_HEADER_SIZE
#define CVM_KERNEL_SEGMENT_SIZE RISC_MV_EXF_SEGMENT_SIZE
#define CVM_BOOTINFO_HEADER_SIZE ((size_t)256)
#define CVM_MEMORY_MAP_ENTRY_SIZE ((size_t)32)
#define CVM_KERNEL_MAX_SEGMENTS RISC_MV_EXF_MAX_SEGMENTS
#define CVM_BOOT_VIRTUAL_HANDOFF_SIZE ((size_t)64)
#define CVM_BOOT_VIRTUAL_HANDOFF_MAGIC "CVMVIRT1"

#define RARCH_M64_ISA_ID UINT32_C(0x34364152) /* "RA64" little-endian. */
#define RARCH_M64_ISA_VERSION UINT32_C(1)
#define RARCH_M64_BYTE_ORDER_LITTLE UINT8_C(1)
#define RARCH_M64_ADDRESS_BITS UINT8_C(64)
#define CVM_ISA_VERSION RARCH_M64_ISA_VERSION
#define CVM_BYTE_ORDER_LITTLE RARCH_M64_BYTE_ORDER_LITTLE
#define CVM_ADDRESS_BITS RARCH_M64_ADDRESS_BITS

/*
 * Legacy source-level names remain available while the public architecture
 * and executable format use RISC-MV EXF and RArchM64 terminology. These
 * aliases do not make old CVMKERN1/.cvm images compatible with EXF v1.
 */
#define CVM_KERNEL_MAGIC RISC_MV_EXF_MAGIC
#define CVM_KERNEL_FORMAT_MAJOR RISC_MV_EXF_VERSION_MAJOR
#define CVM_KERNEL_FORMAT_MINOR RISC_MV_EXF_VERSION_MINOR
#define CVM_ISA_ID RARCH_M64_ISA_ID

#define RISC_MV_EXF_FLAG_RELOCATABLE_PHYSICAL \
    CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL
#define RISC_MV_EXF_SEGMENT_LOAD CVM_SEGMENT_LOAD
#define RISC_MV_EXF_SEGMENT_READ CVM_SEGMENT_READ
#define RISC_MV_EXF_SEGMENT_WRITE CVM_SEGMENT_WRITE
#define RISC_MV_EXF_SEGMENT_EXECUTE CVM_SEGMENT_EXECUTE

#define CVM_BOOTINFO_HANDOFF_MAGIC UINT64_C(0x31544F4F424D5643)

enum {
    CVM_SEGMENT_LOAD = 1
};

enum {
    CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL = UINT64_C(1) << 0
};

enum {
    CVM_SEGMENT_READ = 1u << 0,
    CVM_SEGMENT_WRITE = 1u << 1,
    CVM_SEGMENT_EXECUTE = 1u << 2
};

enum {
    CVM_BOOTINFO_FLAG_MMU_ENABLED = 1u << 0,
    CVM_BOOTINFO_FLAG_INITRD_PRESENT = 1u << 1,
    CVM_BOOTINFO_FLAG_COMMAND_LINE_PRESENT = 1u << 2,
    CVM_BOOTINFO_FLAG_RANDOM_SEED_VALID = 1u << 3,
    CVM_BOOTINFO_FLAG_GPT_BOOT = 1u << 4,
    CVM_BOOTINFO_FLAG_FALLBACK_BOOT = 1u << 5
};

enum {
    CVM_MEMORY_USABLE = 1,
    CVM_MEMORY_RESERVED = 2,
    CVM_MEMORY_KERNEL = 3,
    CVM_MEMORY_BOOTLOADER_RECLAIMABLE = 4,
    CVM_MEMORY_BOOT_INFO = 5,
    CVM_MEMORY_INITRD = 6,
    CVM_MEMORY_FIRMWARE = 7,
    CVM_MEMORY_MMIO = 8
};

enum {
    CVM_MEMORY_CACHEABLE = 1u << 0,
    CVM_MEMORY_WRITABLE = 1u << 1,
    CVM_MEMORY_EXECUTABLE = 1u << 2,
    CVM_MEMORY_DEVICE = 1u << 3,
    CVM_MEMORY_PERSISTENT = 1u << 4
};

typedef struct {
    uint8_t magic[8];
    uint16_t format_major;
    uint16_t format_minor;
    uint32_t header_size;
    uint64_t flags;
    uint32_t isa_id;
    uint32_t isa_version;
    uint8_t address_bits;
    uint8_t byte_order;
    uint16_t segment_count;
    uint32_t segment_entry_size;
    uint64_t segment_table_offset;
    uint64_t entry_physical_address;
    uint64_t image_file_size;
    uint64_t required_cpu_features;
    uint8_t build_id[16];
    uint32_t header_crc32;
    uint32_t payload_crc32;
    uint64_t entry_virtual_address;
    uint64_t virtual_base;
    uint64_t virtual_size;
    uint8_t reserved[8];
} CvmKernelHeader;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t file_offset;
    uint64_t load_address;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
    uint64_t reserved;
} CvmKernelSegment;

typedef CvmKernelHeader RiscMvExfHeader;
typedef CvmKernelSegment RiscMvExfSegment;

typedef struct {
    uint8_t magic[8];
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_size;
    uint32_t total_size;
    uint32_t flags;
    uint32_t boot_cpu_id;
    uint32_t reserved0;
    uint64_t memory_map_offset;
    uint32_t memory_map_count;
    uint32_t memory_map_entry_size;
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t kernel_entry;
    uint64_t kernel_physical_base;
    uint64_t kernel_physical_size;
    uint64_t initrd_base;
    uint64_t initrd_size;
    uint64_t command_line_offset;
    uint32_t command_line_size;
    uint32_t reserved1;
    uint64_t vio_hub_base;
    uint64_t system_info_base;
    uint64_t system_control_base;
    uint64_t irq_controller_base;
    uint64_t core_control_base;
    uint64_t timer_base;
    uint64_t uart_base;
    uint32_t boot_device_slot;
    uint32_t boot_partition_index;
    uint64_t boot_partition_lba;
    uint64_t boot_partition_sectors;
    uint32_t page_size;
    uint16_t physical_address_bits;
    uint16_t virtual_address_bits;
    uint8_t random_seed[32];
    uint32_t checksum;
    uint8_t reserved[12];
} CvmBootInfo;

typedef struct {
    uint8_t magic[8];
    uint64_t kernel_virtual_base;
    uint64_t kernel_virtual_size;
    uint64_t initial_page_table_root;
    uint64_t direct_map_base;
    uint64_t direct_map_size;
    uint64_t page_table_physical_base;
    uint64_t page_table_physical_size;
} CvmBootVirtualHandoff;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
    uint64_t reserved;
} CvmMemoryMapEntry;

typedef enum {
    CVM_BOOT_FORMAT_OK = 0,
    CVM_BOOT_FORMAT_INVALID_ARGUMENT,
    CVM_BOOT_FORMAT_TRUNCATED,
    CVM_BOOT_FORMAT_BAD_MAGIC,
    CVM_BOOT_FORMAT_UNSUPPORTED,
    CVM_BOOT_FORMAT_BAD_LAYOUT,
    CVM_BOOT_FORMAT_BAD_CHECKSUM,
    CVM_BOOT_FORMAT_BAD_SEGMENT,
    CVM_BOOT_FORMAT_BAD_ENTRY
} CvmBootFormatStatus;

typedef CvmBootFormatStatus RiscMvExfStatus;

_Static_assert(sizeof(CvmKernelHeader) == CVM_KERNEL_HEADER_SIZE,
               "CvmKernelHeader layout changed");
_Static_assert(sizeof(CvmKernelSegment) == CVM_KERNEL_SEGMENT_SIZE,
               "CvmKernelSegment layout changed");
_Static_assert(sizeof(CvmBootInfo) == CVM_BOOTINFO_HEADER_SIZE,
               "CvmBootInfo layout changed");
_Static_assert(sizeof(CvmBootVirtualHandoff) ==
                   CVM_BOOT_VIRTUAL_HANDOFF_SIZE,
               "CvmBootVirtualHandoff layout changed");
_Static_assert(sizeof(CvmMemoryMapEntry) == CVM_MEMORY_MAP_ENTRY_SIZE,
               "CvmMemoryMapEntry layout changed");

uint32_t cvm_crc32(const void *data, size_t size);

void cvm_kernel_header_encode(uint8_t output[CVM_KERNEL_HEADER_SIZE],
                              const CvmKernelHeader *header);
void cvm_kernel_header_decode(const uint8_t input[CVM_KERNEL_HEADER_SIZE],
                              CvmKernelHeader *header);
void cvm_kernel_segment_encode(uint8_t output[CVM_KERNEL_SEGMENT_SIZE],
                               const CvmKernelSegment *segment);
void cvm_kernel_segment_decode(const uint8_t input[CVM_KERNEL_SEGMENT_SIZE],
                               CvmKernelSegment *segment);

CvmBootFormatStatus cvm_kernel_image_finalize(uint8_t *image,
                                               size_t image_size);
CvmBootFormatStatus cvm_kernel_image_validate(const uint8_t *image,
                                               size_t image_size,
                                               CvmKernelHeader *header,
                                               char *error,
                                               size_t error_size);

/* Canonical names for new RISC-MV EXF code; implementations remain legacy. */
#define risc_mv_exf_header_encode cvm_kernel_header_encode
#define risc_mv_exf_header_decode cvm_kernel_header_decode
#define risc_mv_exf_segment_encode cvm_kernel_segment_encode
#define risc_mv_exf_segment_decode cvm_kernel_segment_decode
#define risc_mv_exf_finalize cvm_kernel_image_finalize
#define risc_mv_exf_validate cvm_kernel_image_validate

void cvm_boot_info_encode(uint8_t output[CVM_BOOTINFO_HEADER_SIZE],
                          const CvmBootInfo *info);
void cvm_boot_info_decode(const uint8_t input[CVM_BOOTINFO_HEADER_SIZE],
                          CvmBootInfo *info);
void cvm_boot_virtual_handoff_encode(
    uint8_t output[CVM_BOOT_VIRTUAL_HANDOFF_SIZE],
    const CvmBootVirtualHandoff *handoff);
void cvm_boot_virtual_handoff_decode(
    const uint8_t input[CVM_BOOT_VIRTUAL_HANDOFF_SIZE],
    CvmBootVirtualHandoff *handoff);
void cvm_memory_map_entry_encode(uint8_t output[CVM_MEMORY_MAP_ENTRY_SIZE],
                                 const CvmMemoryMapEntry *entry);
void cvm_memory_map_entry_decode(const uint8_t input[CVM_MEMORY_MAP_ENTRY_SIZE],
                                 CvmMemoryMapEntry *entry);

CvmBootFormatStatus cvm_boot_info_finalize(uint8_t *data, size_t data_size);
CvmBootFormatStatus cvm_boot_info_validate(const uint8_t *data,
                                            size_t data_size,
                                            CvmBootInfo *info,
                                            char *error,
                                            size_t error_size);

const char *cvm_boot_format_status_name(CvmBootFormatStatus status);

#endif
