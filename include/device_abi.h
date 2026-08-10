#ifndef DEVICE_ABI_H
#define DEVICE_ABI_H

#include <stdint.h>

#define VM_DEVICE_ABI_VERSION 1U
#define VM_DEVICE_NAME_CAPACITY 64U
#define VM_DEVICE_MAX_BARS 4U
#define VM_DEVICE_MAX_IRQS 4U
#define VM_DEVICE_QUERY_SYMBOL "vm_device_query"

typedef enum {
    VM_DEVICE_CLASS_OTHER = 0,
    VM_DEVICE_CLASS_SYSTEM = 1,
    VM_DEVICE_CLASS_TIMER = 2,
    VM_DEVICE_CLASS_SERIAL = 3,
    VM_DEVICE_CLASS_DISPLAY = 4,
    VM_DEVICE_CLASS_USB_CONTROLLER = 5,
    VM_DEVICE_CLASS_STORAGE = 6,
    VM_DEVICE_CLASS_INPUT = 7,
    VM_DEVICE_CLASS_NETWORK = 8,
    VM_DEVICE_CLASS_INTERRUPT_CONTROLLER = 9,
    VM_DEVICE_CLASS_TEST = UINT32_C(0xFFFF)
} VmDeviceClass;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    char name[VM_DEVICE_NAME_CAPACITY];
    uint32_t device_class;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_version;
    uint64_t features;
    uint32_t bar_count;
    uint32_t irq_count;
    uint64_t bar_sizes[VM_DEVICE_MAX_BARS];
    uint64_t bar_alignments[VM_DEVICE_MAX_BARS];
} VmDeviceDescriptor;

typedef struct {
    uint32_t struct_size;
    uint32_t bar_count;
    uint32_t irq_count;
    uint32_t reserved;
    uint64_t bar_bases[VM_DEVICE_MAX_BARS];
    uint64_t bar_sizes[VM_DEVICE_MAX_BARS];
    uint32_t irqs[VM_DEVICE_MAX_IRQS];
} VmDeviceResources;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    void *context;
    int (*dma_read)(void *context,
                    uint64_t physical_address,
                    void *buffer,
                    uint64_t size);
    int (*dma_write)(void *context,
                     uint64_t physical_address,
                     const void *buffer,
                     uint64_t size);
    int (*raise_irq)(void *context, uint32_t irq);
    void (*log)(void *context, uint32_t level, const char *message);
    void *(*get_service)(void *context,
                         const char *service_name,
                         uint32_t service_version);
} VmDeviceHostApi;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    VmDeviceDescriptor descriptor;
    int (*create)(const VmDeviceHostApi *host,
                  const VmDeviceResources *resources,
                  const char *configuration,
                  void **device_context);
    void (*destroy)(void *device_context);
    int (*read)(void *device_context,
                uint32_t bar,
                uint64_t offset,
                uint32_t width,
                uint64_t *value);
    int (*write)(void *device_context,
                 uint32_t bar,
                 uint64_t offset,
                 uint32_t width,
                 uint64_t value);
    void (*tick)(void *device_context, uint64_t ticks);
    void (*reset)(void *device_context);
} VmDeviceModule;

typedef const VmDeviceModule *(*VmDeviceQuery)(uint32_t host_abi_version);

#endif
