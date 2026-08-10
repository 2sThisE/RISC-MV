#ifndef BOOT_H
#define BOOT_H

#include <stddef.h>
#include <stdint.h>

#define VM_DIRECT_LOAD_ADDRESS ((size_t)1)
#define VM_BOOT_ROM_BASE UINT64_C(0x0000007FFFF00000)
#define VM_BOOT_ROM_MAX_SIZE ((size_t)1024 * 1024)

#endif
