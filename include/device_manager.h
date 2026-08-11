#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "bus.h"
#include "device_abi.h"
#include "vio_protocol.h"

#define DEVICE_MANAGER_MAX_SLOTS 16U
#define DEVICE_MANAGER_MAX_SERVICES 16U
#define DEVICE_MANAGER_DYNAMIC_MMIO_BASE UINT64_C(0xFFFFFFFFF0000000)
#define DEVICE_MANAGER_DYNAMIC_MMIO_LIMIT UINT64_C(0xFFFFFFFFFFFBFFFF)

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
