#include "kernel_internal.h"

#include <cvm/mmio.h>

#define KERNEL_SYSCALL_PATH_LIMIT 128U

#define TRAP_REGISTER(frame, reg) ((frame)[15U - (reg)])
#define KERNEL_UINTPTR_MAX ((uintptr_t)-1)

static int user_buffer_bounds_valid(uintptr_t address, size_t size)
{
    return size <= (size_t)INT64_MAX &&
           (size == 0 || address <= KERNEL_UINTPTR_MAX - (size - 1));
}

static int user_range_valid(const KernelAddressSpace *space,
                            uintptr_t address,
                            size_t size,
                            uint64_t flags)
{
    if (space == NULL) return 0;
    if (size == 0) return 1;
    if (address > KERNEL_UINTPTR_MAX - (size - 1)) return 0;
    uintptr_t last = address + size - 1;
    uintptr_t page = address & (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    uintptr_t last_page = last & (uintptr_t)KERNEL_PTE_ADDRESS_MASK;
    for (;;) {
        uintptr_t physical;
        if (!kernel_address_space_resolve(space, page, flags, &physical)) {
            return 0;
        }
        if (page == last_page) break;
        if (page > KERNEL_UINTPTR_MAX - (uintptr_t)KERNEL_PAGE_SIZE) return 0;
        page += (uintptr_t)KERNEL_PAGE_SIZE;
    }
    return 1;
}

int kernel_copy_from_user(const KernelAddressSpace *space,
                          void *destination,
                          uintptr_t source,
                          size_t size)
{
    if ((destination == NULL && size != 0) ||
        !user_range_valid(space, source, size, KERNEL_PTE_READ)) {
        return 0;
    }
    uint8_t *output = destination;
    for (size_t i = 0; i < size;) {
        uintptr_t physical;
        if (!kernel_address_space_resolve(space, source + i,
                                          KERNEL_PTE_READ, &physical)) {
            return 0;
        }
        size_t chunk = (size_t)(KERNEL_PAGE_SIZE -
            (physical & (uintptr_t)KERNEL_PAGE_MASK));
        if (chunk > size - i) chunk = size - i;
        const uint8_t *input = kernel_phys_to_virt(physical);
        if (input == NULL) return 0;
        for (size_t j = 0; j < chunk; ++j) output[i + j] = input[j];
        i += chunk;
    }
    return 1;
}

int kernel_copy_to_user(const KernelAddressSpace *space,
                        uintptr_t destination,
                        const void *source,
                        size_t size)
{
    if ((source == NULL && size != 0) ||
        !user_range_valid(space, destination, size,
                          KERNEL_PTE_READ | KERNEL_PTE_WRITE)) {
        return 0;
    }
    const uint8_t *input = source;
    for (size_t i = 0; i < size;) {
        uintptr_t physical;
        if (!kernel_address_space_resolve(
                space, destination + i,
                KERNEL_PTE_READ | KERNEL_PTE_WRITE, &physical)) {
            return 0;
        }
        size_t chunk = (size_t)(KERNEL_PAGE_SIZE -
            (physical & (uintptr_t)KERNEL_PAGE_MASK));
        if (chunk > size - i) chunk = size - i;
        uint8_t *output = kernel_phys_to_virt(physical);
        if (output == NULL) return 0;
        for (size_t j = 0; j < chunk; ++j) output[j] = input[i + j];
        i += chunk;
    }
    return 1;
}

static int64_t syscall_write(uint64_t fd, uintptr_t user_buffer, size_t size)
{
    KernelAddressSpace *space = kernel_scheduler_current_space();
    if (!user_buffer_bounds_valid(user_buffer, size)) {
        return -KERNEL_ERROR_FAULT;
    }
    KernelFdTable *table = kernel_scheduler_current_fd_table();
    if (fd >= KERNEL_FD_LIMIT || table == NULL) return -KERNEL_ERROR_BAD_FD;
    KernelOpenFile *file = kernel_fd_acquire(table, (int)fd);
    if (file == NULL) return -KERNEL_ERROR_BAD_FD;
    uint8_t buffer[64];
    size_t written = 0;
    while (written < size) {
        size_t chunk = size - written;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (!kernel_copy_from_user(space, buffer,
                                   user_buffer + written, chunk)) {
            kernel_vfs_file_release(file);
            return -KERNEL_ERROR_FAULT;
        }
        int64_t result = kernel_vfs_file_write(file, buffer, chunk);
        if (result < 0) {
            kernel_vfs_file_release(file);
            return written != 0 ? (int64_t)written : -KERNEL_ERROR_ACCESS;
        }
        written += (size_t)result;
        if ((size_t)result < chunk) break;
    }
    kernel_vfs_file_release(file);
    if (written != 0) kernel_scheduler_note_write();
    return (int64_t)written;
}

static int64_t syscall_read(uint64_t fd, uintptr_t user_buffer, size_t size)
{
    KernelFdTable *table = kernel_scheduler_current_fd_table();
    if (fd >= KERNEL_FD_LIMIT || table == NULL) return -KERNEL_ERROR_BAD_FD;
    if (!user_buffer_bounds_valid(user_buffer, size)) {
        return -KERNEL_ERROR_FAULT;
    }
    if (size == 0) return 0;
    KernelOpenFile *file = kernel_fd_acquire(table, (int)fd);
    if (file == NULL) return -KERNEL_ERROR_BAD_FD;
    uint8_t buffer[64];
    size_t read = 0;
    while (read < size) {
        size_t chunk = size - read;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int64_t result = kernel_vfs_file_read(file, buffer, chunk);
        if (result < 0) {
            kernel_vfs_file_release(file);
            return read != 0 ? (int64_t)read : -KERNEL_ERROR_ACCESS;
        }
        if (result == 0) break;
        if (!kernel_copy_to_user(kernel_scheduler_current_space(),
                                 user_buffer + read,
                                 buffer,
                                 (size_t)result)) {
            kernel_vfs_file_release(file);
            return -KERNEL_ERROR_FAULT;
        }
        read += (size_t)result;
        if ((size_t)result < chunk) break;
    }
    kernel_vfs_file_release(file);
    return (int64_t)read;
}

static int copy_user_path(uintptr_t user_path, char path[128])
{
    if (user_path == 0) return 0;
    for (size_t i = 0; i < KERNEL_SYSCALL_PATH_LIMIT; ++i) {
        if (user_path > KERNEL_UINTPTR_MAX - i) return 0;
        if (!kernel_copy_from_user(kernel_scheduler_current_space(),
                                   &path[i], user_path + i, 1)) {
            return 0;
        }
        if (path[i] == '\0') return i != 0;
    }
    return 0;
}

static int64_t syscall_open(uintptr_t user_path, uint32_t flags)
{
    if (flags == 0 ||
        (flags & ~(KERNEL_VFS_OPEN_READ | KERNEL_VFS_OPEN_WRITE |
                   KERNEL_VFS_OPEN_APPEND)) != 0 ||
        ((flags & KERNEL_VFS_OPEN_APPEND) != 0 &&
         (flags & KERNEL_VFS_OPEN_WRITE) == 0)) {
        return -KERNEL_ERROR_INVALID;
    }
    char path[KERNEL_SYSCALL_PATH_LIMIT];
    if (!copy_user_path(user_path, path)) return -KERNEL_ERROR_FAULT;
    KernelOpenFile *file = kernel_vfs_open(path, flags);
    if (file == NULL) return -KERNEL_ERROR_NO_ENTRY;
    int fd = kernel_fd_install(kernel_scheduler_current_fd_table(), file, 3);
    kernel_vfs_file_release(file);
    return fd >= 0 ? fd : -KERNEL_ERROR_TOO_MANY_FILES;
}

static int64_t syscall_close(uint64_t fd)
{
    if (fd >= KERNEL_FD_LIMIT ||
        !kernel_fd_close(kernel_scheduler_current_fd_table(), (int)fd)) {
        return -KERNEL_ERROR_BAD_FD;
    }
    return 0;
}

static int64_t syscall_seek(uint64_t fd, int64_t offset, uint32_t whence)
{
    if (fd >= KERNEL_FD_LIMIT) return -KERNEL_ERROR_BAD_FD;
    KernelOpenFile *file = kernel_fd_acquire(
        kernel_scheduler_current_fd_table(), (int)fd);
    if (file == NULL) return -KERNEL_ERROR_BAD_FD;
    int64_t result = kernel_vfs_file_seek(file, offset, whence);
    kernel_vfs_file_release(file);
    return result >= 0 ? result : -KERNEL_ERROR_INVALID;
}

static int64_t syscall_fsync(uint64_t fd)
{
    if (fd >= KERNEL_FD_LIMIT) return -KERNEL_ERROR_BAD_FD;
    KernelOpenFile *file = kernel_fd_acquire(
        kernel_scheduler_current_fd_table(), (int)fd);
    if (file == NULL) return -KERNEL_ERROR_BAD_FD;
    int okay = kernel_vfs_file_sync(file);
    kernel_vfs_file_release(file);
    return okay ? 0 : -KERNEL_ERROR_IO;
}

void kernel_syscall_dispatch(uint64_t *frame)
{
    if (frame == NULL || kernel_scheduler_current_space() == NULL) return;
    uint64_t number = TRAP_REGISTER(frame, 0);
    uint64_t argument1 = TRAP_REGISTER(frame, 1);
    uint64_t argument2 = TRAP_REGISTER(frame, 2);
    uint64_t argument3 = TRAP_REGISTER(frame, 3);
    int64_t result;
    if (number == RARCHM64_SYS_EXIT) {
        kernel_scheduler_exit(frame, (int64_t)argument1);
        return;
    } else if (number == RARCHM64_SYS_WRITE) {
        result = syscall_write(argument1, (uintptr_t)argument2,
                               (size_t)argument3);
    } else if (number == RARCHM64_SYS_READ) {
        result = syscall_read(argument1, (uintptr_t)argument2,
                              (size_t)argument3);
    } else if (number == RARCHM64_SYS_YIELD) {
        kernel_scheduler_yield(frame);
        return;
    } else if (number == RARCHM64_SYS_GETPID) {
        result = (int64_t)kernel_scheduler_current_pid();
    } else if (number == RARCHM64_SYS_WAIT) {
        if (!kernel_scheduler_waitpid(frame, UINT64_MAX,
                                      (uintptr_t)argument1, &result)) {
            return;
        }
    } else if (number == RARCHM64_SYS_WAITPID) {
        if (argument1 == 0 || argument1 > INT64_MAX) {
            result = -KERNEL_ERROR_INVALID;
        } else if (!kernel_scheduler_waitpid(frame, argument1,
                                             (uintptr_t)argument2, &result)) {
            return;
        }
    } else if (number == RARCHM64_SYS_JOIN) {
        if (!kernel_scheduler_join(frame, argument1,
                                   (uintptr_t)argument2, &result)) {
            return;
        }
    } else if (number == RARCHM64_SYS_OPEN) {
        result = syscall_open((uintptr_t)argument1, (uint32_t)argument2);
    } else if (number == RARCHM64_SYS_CLOSE) {
        result = syscall_close(argument1);
    } else if (number == RARCHM64_SYS_SEEK) {
        result = syscall_seek(argument1, (int64_t)argument2,
                              (uint32_t)argument3);
    } else if (number == RARCHM64_SYS_FSYNC) {
        result = syscall_fsync(argument1);
    } else {
        result = -KERNEL_ERROR_NOT_IMPLEMENTED;
    }
    TRAP_REGISTER(frame, 0) = (uint64_t)result;
}

int kernel_syscall_self_test(void)
{
    KernelAddressSpace *space = kernel_address_space_create();
    if (space == NULL) return 1;
    uintptr_t first;
    uintptr_t read_only;
    uintptr_t base = (uintptr_t)KERNEL_USER_IMAGE_BASE;
    if (kernel_address_space_map_anonymous(
            space, base, KERNEL_PTE_READ | KERNEL_PTE_WRITE, &first) != 0 ||
        kernel_address_space_map_anonymous(
            space, base + 2 * (uintptr_t)KERNEL_PAGE_SIZE,
            KERNEL_PTE_READ, &read_only) != 0) {
        kernel_address_space_destroy(space);
        return 1;
    }
    uint8_t *memory = kernel_phys_to_virt(first);
    if (memory == NULL) return 1;
    memory[KERNEL_PAGE_SIZE - 4] = 0x11;
    memory[KERNEL_PAGE_SIZE - 3] = 0x22;
    memory[KERNEL_PAGE_SIZE - 2] = 0x33;
    memory[KERNEL_PAGE_SIZE - 1] = 0x44;
    uint8_t output[4];
    static const uint8_t replacement[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    int ok = kernel_copy_from_user(
                 space, output, base + KERNEL_PAGE_SIZE - 4, 4) &&
             output[0] == 0x11 && output[3] == 0x44 &&
             !kernel_copy_from_user(
                 space, output, base + KERNEL_PAGE_SIZE - 4, 8) &&
             kernel_copy_to_user(space, base + 8, replacement, 4) &&
             memory[8] == 0xA1 && memory[11] == 0xD4 &&
             !kernel_copy_to_user(
                 space, base + 2 * KERNEL_PAGE_SIZE, replacement, 4) &&
             !kernel_copy_from_user(space, output, KERNEL_UINTPTR_MAX - 1, 4) &&
             user_buffer_bounds_valid(KERNEL_UINTPTR_MAX, 0) &&
             user_buffer_bounds_valid(base, KERNEL_PAGE_SIZE) &&
             !user_buffer_bounds_valid(KERNEL_UINTPTR_MAX - 1, 4) &&
             !user_buffer_bounds_valid(base, (size_t)INT64_MAX + 1);
    kernel_address_space_destroy(space);
    return ok ? 0 : 1;
}
