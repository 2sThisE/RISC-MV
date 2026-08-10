#ifndef MODULE_LOADER_H
#define MODULE_LOADER_H

#include <stddef.h>

#include "device_abi.h"

#define VM_DEVICE_MODULE_PATH_CAPACITY 4096U
#define VM_DEVICE_MODULE_CONFIGURATION_CAPACITY 4096U

typedef struct {
    void *native_handle;
    const VmDeviceModule *module;
} VmLoadedDeviceModule;

typedef struct {
    char path[VM_DEVICE_MODULE_PATH_CAPACITY];
    char configuration[VM_DEVICE_MODULE_CONFIGURATION_CAPACITY];
} VmDiscoveredDeviceModule;

int vm_device_module_discover(const char *argv0,
                              const char *directory_name,
                              VmDiscoveredDeviceModule *modules,
                              size_t capacity,
                              size_t *count,
                              char *error,
                              size_t error_capacity);

int vm_device_module_load(const char *path,
                          VmLoadedDeviceModule *loaded,
                          char *error,
                          size_t error_capacity);
void vm_device_module_unload(VmLoadedDeviceModule *loaded);

#endif
