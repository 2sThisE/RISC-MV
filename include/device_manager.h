#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "bus.h"
#include "device_abi.h"

#define DEVICE_MANAGER_MAX_SLOTS 16U
#define DEVICE_MANAGER_MAX_SERVICES 16U
#define DEVICE_MANAGER_DYNAMIC_MMIO_BASE UINT64_C(0xFFFFFFFFF0000000)
#define DEVICE_MANAGER_DYNAMIC_MMIO_LIMIT UINT64_C(0xFFFFFFFFFFFBFFFF)

#define VIO_HUB_MMIO_BASE UINT64_C(0xFFFFFFFFFFFC0000)
#define VIO_HUB_MMIO_SIZE UINT64_C(4096)
#define VIO_HUB_MAGIC UINT64_C(0x314F4956)
#define VIO_HUB_ABI_VERSION UINT64_C(2)

#define VIO_HUB_MAGIC_OFFSET        UINT64_C(0x00)
#define VIO_HUB_ABI_OFFSET          UINT64_C(0x08)
#define VIO_HUB_SLOT_COUNT_OFFSET   UINT64_C(0x10)
#define VIO_HUB_GENERATION_OFFSET   UINT64_C(0x18)
#define VIO_HUB_EVENT_STATUS_OFFSET UINT64_C(0x20)
#define VIO_HUB_EVENT_ACK_OFFSET    UINT64_C(0x28)
#define VIO_HUB_SLOT_BASE           UINT64_C(0x100)
#define VIO_HUB_SLOT_STRIDE         UINT64_C(0xC0)

#define VIO_SLOT_STATUS_OFFSET         UINT64_C(0x00)
#define VIO_SLOT_CLASS_OFFSET          UINT64_C(0x08)
#define VIO_SLOT_VENDOR_OFFSET         UINT64_C(0x10)
#define VIO_SLOT_DEVICE_OFFSET         UINT64_C(0x18)
#define VIO_SLOT_VERSION_OFFSET        UINT64_C(0x20)
#define VIO_SLOT_BAR0_BASE_OFFSET      UINT64_C(0x28)
#define VIO_SLOT_BAR0_SIZE_OFFSET      UINT64_C(0x30)
#define VIO_SLOT_IRQ0_OFFSET           UINT64_C(0x38)
#define VIO_SLOT_FEATURES_OFFSET       UINT64_C(0x40)
#define VIO_SLOT_DEVICE_ABI_OFFSET     UINT64_C(0x48)
#define VIO_SLOT_BAR_COUNT_OFFSET      UINT64_C(0x50)
#define VIO_SLOT_IRQ_COUNT_OFFSET      UINT64_C(0x58)
#define VIO_SLOT_BAR1_BASE_OFFSET      UINT64_C(0x60)
#define VIO_SLOT_BAR1_SIZE_OFFSET      UINT64_C(0x68)
#define VIO_SLOT_BAR2_BASE_OFFSET      UINT64_C(0x70)
#define VIO_SLOT_BAR2_SIZE_OFFSET      UINT64_C(0x78)
#define VIO_SLOT_BAR3_BASE_OFFSET      UINT64_C(0x80)
#define VIO_SLOT_BAR3_SIZE_OFFSET      UINT64_C(0x88)
#define VIO_SLOT_IRQ1_OFFSET           UINT64_C(0x90)
#define VIO_SLOT_IRQ2_OFFSET           UINT64_C(0x98)
#define VIO_SLOT_IRQ3_OFFSET           UINT64_C(0xA0)

#define VIO_SLOT_STATUS_PRESENT  (UINT64_C(1) << 0)
#define VIO_SLOT_STATUS_EXTERNAL (UINT64_C(1) << 1)
#define VIO_HUB_EVENT_DEVICE_ADDED (UINT64_C(1) << 0)
#define VIO_HUB_EVENT_DEVICE_REMOVED (UINT64_C(1) << 1)

typedef void (*DeviceManagerLogCallback)(void *context,
                                         uint32_t level,
                                         const char *message);

struct DeviceManagerSlot;

typedef struct {
    struct DeviceManagerSlot *slot;
    uint32_t bar;
} DeviceBarAdapter;

typedef struct DeviceManagerSlot {
    int present;
    int external;
    VmDeviceDescriptor descriptor;
    VmDeviceResources resources;
    const VmDeviceModule *module;
    void *device_context;
    DeviceBarAdapter bars[VM_DEVICE_MAX_BARS];
} DeviceManagerSlot;

typedef struct {
    char name[VM_DEVICE_NAME_CAPACITY];
    uint32_t version;
    void *service;
} DeviceManagerService;

typedef struct {
    Bus *bus;
    InterruptRouter *interrupts;
    DeviceManagerSlot slots[DEVICE_MANAGER_MAX_SLOTS];
    size_t present_count;
    uint64_t generation;
    uint64_t event_status;
    unsigned char irq_used[INTERRUPT_LINE_COUNT];
    DeviceManagerService services[DEVICE_MANAGER_MAX_SERVICES];
    size_t service_count;
    VmDeviceHostApi host_api;
    DeviceManagerLogCallback log_callback;
    void *log_context;
    atomic_flag lock;
} DeviceManager;

int device_manager_init(DeviceManager *manager,
                        Bus *bus,
                        InterruptRouter *interrupts);
void device_manager_set_log_callback(DeviceManager *manager,
                                     DeviceManagerLogCallback callback,
                                     void *context);
int device_manager_register_service(DeviceManager *manager,
                                    const char *name,
                                    uint32_t version,
                                    void *service);
int device_manager_publish_fixed(DeviceManager *manager,
                                 const VmDeviceDescriptor *descriptor,
                                 const VmDeviceResources *resources,
                                 size_t *slot_index);
int device_manager_attach_module(DeviceManager *manager,
                                 const VmDeviceModule *module,
                                 const char *configuration,
                                 size_t *slot_index);
int device_manager_detach_module(DeviceManager *manager,
                                 size_t slot_index);
const DeviceManagerSlot *device_manager_slot(const DeviceManager *manager,
                                             size_t slot_index);
void device_manager_destroy(DeviceManager *manager);
BusDevice device_manager_hub_as_bus_device(DeviceManager *manager);

#endif
