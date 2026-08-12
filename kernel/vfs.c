#include "kernel_internal.h"

#include <cvm/mmio.h>
#include <rmfs_format.h>

int kernel_rmfs_mount(void);
int kernel_rmfs_read_file(const char *path,
                           void *buffer,
                           size_t capacity,
                           size_t *file_size);
int kernel_rmfs_file_size(const char *path, size_t *file_size);
int kernel_rmfs_read_range(const char *path,
                            size_t offset,
                            void *buffer,
                            size_t capacity,
                            size_t *read_size);
int kernel_rmfs_write_file(const char *path,
                            const void *buffer,
                            size_t size);
int kernel_rmfs_write_range(const char *path,
                            size_t offset,
                            const void *buffer,
                            size_t size,
                            size_t final_size);
int kernel_rmfs_sync(void);

#define KERNEL_VFS_PATH_LIMIT 128

struct KernelVnode {
    KernelSpinLock lock;
    size_t reference_count;
    size_t size;
    KernelVnodeKind kind;
    char path[KERNEL_VFS_PATH_LIMIT];
};

struct KernelOpenFile {
    KernelSpinLock lock;
    KernelVnode *vnode;
    uint64_t offset;
    size_t reference_count;
    uint32_t flags;
};

struct KernelFdTable {
    KernelSpinLock lock;
    KernelOpenFile *entries[KERNEL_FD_LIMIT];
    size_t open_count;
};

static int vfs_mounted;

int kernel_vfs_init(void)
{
    vfs_mounted = kernel_rmfs_mount();
    return vfs_mounted ? 0 : 1;
}

int kernel_vfs_read_file(const char *path,
                         void *buffer,
                         size_t capacity,
                         size_t *file_size)
{
    return vfs_mounted &&
           kernel_rmfs_read_file(path, buffer, capacity, file_size);
}

int kernel_vfs_read_all(const char *path,
                        size_t maximum_size,
                        uint8_t **data,
                        size_t *size)
{
    if (data == NULL || size == NULL) return 0;
    *data = NULL;
    *size = 0;
    size_t measured;
    if (!vfs_mounted || path == NULL || maximum_size == 0 ||
        !kernel_rmfs_file_size(path, &measured) || measured == 0 ||
        measured > maximum_size) {
        return 0;
    }
    uint8_t *buffer = kernel_malloc(measured);
    if (buffer == NULL) return 0;
    size_t loaded;
    if (!kernel_rmfs_read_file(path, buffer, measured, &loaded) ||
        loaded != measured) {
        kernel_free(buffer);
        return 0;
    }
    *data = buffer;
    *size = loaded;
    return 1;
}

int kernel_vfs_write_file(const char *path,
                          const void *buffer,
                          size_t size)
{
    return vfs_mounted && kernel_rmfs_write_file(path, buffer, size);
}

int kernel_vfs_sync(void)
{
    return vfs_mounted && kernel_rmfs_sync();
}

static void vfs_vnode_release(KernelVnode *vnode)
{
    if (vnode == NULL) return;
    kernel_spin_lock(&vnode->lock);
    if (vnode->reference_count == 0) {
        kernel_spin_unlock(&vnode->lock);
        return;
    }
    --vnode->reference_count;
    int destroy = vnode->reference_count == 0;
    kernel_spin_unlock(&vnode->lock);
    if (destroy) kernel_free(vnode);
}

static KernelOpenFile *vfs_open_vnode(const char *path,
                                      size_t size,
                                      KernelVnodeKind kind,
                                      uint32_t flags)
{
    if (path == NULL || flags == 0 ||
        (flags & ~(KERNEL_VFS_OPEN_READ | KERNEL_VFS_OPEN_WRITE |
                   KERNEL_VFS_OPEN_APPEND)) != 0 ||
        ((flags & KERNEL_VFS_OPEN_APPEND) != 0 &&
         (flags & KERNEL_VFS_OPEN_WRITE) == 0)) {
        return NULL;
    }
    size_t length = 0;
    while (path[length] != '\0') {
        if (length + 1 >= KERNEL_VFS_PATH_LIMIT) return NULL;
        ++length;
    }
    if (length == 0) return NULL;
    KernelVnode *vnode = kernel_calloc(1, sizeof(*vnode));
    KernelOpenFile *file = kernel_calloc(1, sizeof(*file));
    if (vnode == NULL || file == NULL) {
        kernel_free(vnode);
        kernel_free(file);
        return NULL;
    }
    kernel_spin_init(&vnode->lock);
    vnode->reference_count = 1;
    vnode->size = size;
    vnode->kind = kind;
    for (size_t i = 0; i <= length; ++i) vnode->path[i] = path[i];
    kernel_spin_init(&file->lock);
    file->vnode = vnode;
    file->reference_count = 1;
    file->flags = flags;
    return file;
}

KernelOpenFile *kernel_vfs_open(const char *path, uint32_t flags)
{
    size_t size;
    if (!vfs_mounted || path == NULL ||
        ((flags & KERNEL_VFS_OPEN_WRITE) != 0 && kernel_block_read_only()) ||
        !kernel_rmfs_file_size(path, &size)) {
        return NULL;
    }
    return vfs_open_vnode(path, size, KERNEL_VNODE_REGULAR, flags);
}

void kernel_vfs_file_retain(KernelOpenFile *file)
{
    if (file == NULL) return;
    kernel_spin_lock(&file->lock);
    if (file->reference_count != KERNEL_SIZE_MAX) ++file->reference_count;
    kernel_spin_unlock(&file->lock);
}

void kernel_vfs_file_release(KernelOpenFile *file)
{
    if (file == NULL) return;
    kernel_spin_lock(&file->lock);
    if (file->reference_count == 0) {
        kernel_spin_unlock(&file->lock);
        return;
    }
    --file->reference_count;
    int destroy = file->reference_count == 0;
    KernelVnode *vnode = destroy ? file->vnode : NULL;
    if (destroy) file->vnode = NULL;
    kernel_spin_unlock(&file->lock);
    if (destroy) {
        vfs_vnode_release(vnode);
        kernel_free(file);
    }
}

KernelFdTable *kernel_fd_table_create(void)
{
    KernelFdTable *table = kernel_calloc(1, sizeof(*table));
    if (table != NULL) kernel_spin_init(&table->lock);
    return table;
}

void kernel_fd_table_destroy(KernelFdTable *table)
{
    if (table == NULL) return;
    KernelOpenFile *files[KERNEL_FD_LIMIT];
    kernel_spin_lock(&table->lock);
    for (size_t i = 0; i < KERNEL_FD_LIMIT; ++i) {
        files[i] = table->entries[i];
        table->entries[i] = NULL;
    }
    table->open_count = 0;
    kernel_spin_unlock(&table->lock);
    for (size_t i = 0; i < KERNEL_FD_LIMIT; ++i) {
        kernel_vfs_file_release(files[i]);
    }
    kernel_free(table);
}

int kernel_fd_install(KernelFdTable *table,
                      KernelOpenFile *file,
                      int minimum_fd)
{
    if (table == NULL || file == NULL || minimum_fd < 0 ||
        minimum_fd >= KERNEL_FD_LIMIT) {
        return -1;
    }
    kernel_spin_lock(&table->lock);
    int result = -1;
    for (int fd = minimum_fd; fd < KERNEL_FD_LIMIT; ++fd) {
        if (table->entries[fd] != NULL) continue;
        kernel_vfs_file_retain(file);
        table->entries[fd] = file;
        ++table->open_count;
        result = fd;
        break;
    }
    kernel_spin_unlock(&table->lock);
    return result;
}

KernelOpenFile *kernel_fd_acquire(KernelFdTable *table, int fd)
{
    if (table == NULL || fd < 0 || fd >= KERNEL_FD_LIMIT) return NULL;
    kernel_spin_lock(&table->lock);
    KernelOpenFile *file = table->entries[fd];
    kernel_vfs_file_retain(file);
    kernel_spin_unlock(&table->lock);
    return file;
}

int kernel_fd_close(KernelFdTable *table, int fd)
{
    if (table == NULL || fd < 0 || fd >= KERNEL_FD_LIMIT) return 0;
    kernel_spin_lock(&table->lock);
    KernelOpenFile *file = table->entries[fd];
    if (file != NULL) {
        table->entries[fd] = NULL;
        --table->open_count;
    }
    kernel_spin_unlock(&table->lock);
    if (file == NULL) return 0;
    kernel_vfs_file_release(file);
    return 1;
}

size_t kernel_fd_open_count(KernelFdTable *table)
{
    if (table == NULL) return 0;
    kernel_spin_lock(&table->lock);
    size_t count = table->open_count;
    kernel_spin_unlock(&table->lock);
    return count;
}

int64_t kernel_vfs_file_read(KernelOpenFile *file, void *buffer, size_t size)
{
    if (file == NULL || (buffer == NULL && size != 0)) return -1;
    kernel_spin_lock(&file->lock);
    if ((file->flags & KERNEL_VFS_OPEN_READ) == 0 || file->vnode == NULL) {
        kernel_spin_unlock(&file->lock);
        return -1;
    }
    int64_t result = -1;
    if (file->vnode->kind == KERNEL_VNODE_KEYBOARD) {
        if (size == 0) {
            result = 0;
        } else {
            uint64_t event;
            if (!kernel_keyboard_poll(&event)) {
                result = 0;
            } else {
                ((uint8_t *)buffer)[0] = (uint8_t)event;
                result = 1;
            }
        }
    } else if (file->vnode->kind == KERNEL_VNODE_REGULAR) {
        size_t read_size;
        if (kernel_rmfs_read_range(file->vnode->path,
                                    (size_t)file->offset,
                                    buffer,
                                    size,
                                    &read_size)) {
            file->offset += read_size;
            result = (int64_t)read_size;
        }
    }
    kernel_spin_unlock(&file->lock);
    return result;
}

int64_t kernel_vfs_file_write(KernelOpenFile *file,
                              const void *buffer,
                              size_t size)
{
    if (file == NULL || (buffer == NULL && size != 0)) return -1;
    kernel_spin_lock(&file->lock);
    if ((file->flags & KERNEL_VFS_OPEN_WRITE) == 0 || file->vnode == NULL) {
        kernel_spin_unlock(&file->lock);
        return -1;
    }
    if (size == 0) {
        kernel_spin_unlock(&file->lock);
        return 0;
    }
    if (file->vnode->kind == KERNEL_VNODE_REGULAR &&
        (file->flags & KERNEL_VFS_OPEN_APPEND) != 0) {
        file->offset = file->vnode->size;
    }
    int64_t result = -1;
    if (file->vnode->kind == KERNEL_VNODE_UART) {
        const uint8_t *input = buffer;
        for (size_t i = 0; i < size; ++i) {
            cvm_mmio_write8(kernel_uart_address, input[i]);
        }
        result = (int64_t)size;
    } else if (file->vnode->kind == KERNEL_VNODE_REGULAR &&
               file->offset <= KERNEL_SIZE_MAX &&
               size <= KERNEL_SIZE_MAX - (size_t)file->offset) {
        size_t end = (size_t)file->offset + size;
        size_t new_size = file->vnode->size > end ? file->vnode->size : end;
        int okay = kernel_rmfs_write_range(file->vnode->path,
                                           (size_t)file->offset,
                                           buffer,
                                           size,
                                           new_size);
        if (okay) {
            file->offset = end;
            file->vnode->size = new_size;
            result = (int64_t)size;
        }
    }
    kernel_spin_unlock(&file->lock);
    return result;
}

int64_t kernel_vfs_file_seek(KernelOpenFile *file,
                             int64_t offset,
                             uint32_t whence)
{
    if (file == NULL || whence > 2) return -1;
    kernel_spin_lock(&file->lock);
    if (file->vnode == NULL || file->vnode->kind != KERNEL_VNODE_REGULAR) {
        kernel_spin_unlock(&file->lock);
        return -1;
    }
    uint64_t base = whence == 0 ? 0 :
                    whence == 1 ? file->offset : file->vnode->size;
    uint64_t next;
    if ((offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base) ||
        (offset >= 0 && (uint64_t)offset > UINT64_MAX - base)) {
        kernel_spin_unlock(&file->lock);
        return -1;
    }
    next = offset < 0 ? base - ((uint64_t)(-(offset + 1)) + 1)
                      : base + (uint64_t)offset;
    if (next > INT64_MAX || next > KERNEL_SIZE_MAX) {
        kernel_spin_unlock(&file->lock);
        return -1;
    }
    file->offset = next;
    kernel_spin_unlock(&file->lock);
    return (int64_t)next;
}

int kernel_vfs_file_sync(KernelOpenFile *file)
{
    if (file == NULL) return 0;
    kernel_spin_lock(&file->lock);
    int regular = file->vnode != NULL &&
                  file->vnode->kind == KERNEL_VNODE_REGULAR;
    kernel_spin_unlock(&file->lock);
    return regular && kernel_vfs_sync();
}

int kernel_fd_populate_standard(KernelFdTable *table)
{
    if (table == NULL || kernel_fd_open_count(table) != 0) return 0;
    KernelOpenFile *input = vfs_open_vnode(
        "/DEV/KEYBOARD", 0, KERNEL_VNODE_KEYBOARD, KERNEL_VFS_OPEN_READ);
    KernelOpenFile *output = vfs_open_vnode(
        "/DEV/UART", 0, KERNEL_VNODE_UART, KERNEL_VFS_OPEN_WRITE);
    KernelOpenFile *error = vfs_open_vnode(
        "/DEV/UART", 0, KERNEL_VNODE_UART, KERNEL_VFS_OPEN_WRITE);
    int okay = input != NULL && output != NULL && error != NULL &&
               kernel_fd_install(table, input, 0) == 0 &&
               kernel_fd_install(table, output, 1) == 1 &&
               kernel_fd_install(table, error, 2) == 2;
    kernel_vfs_file_release(input);
    kernel_vfs_file_release(output);
    kernel_vfs_file_release(error);
    if (!okay) {
        (void)kernel_fd_close(table, 0);
        (void)kernel_fd_close(table, 1);
        (void)kernel_fd_close(table, 2);
    }
    return okay;
}

int kernel_vfs_self_test(void)
{
    uint8_t *boot_image;
    size_t boot_size;
    if (!kernel_vfs_read_all("/BIN/INIT.EXF", 8192,
                             &boot_image, &boot_size) ||
        boot_size < CVM_KERNEL_HEADER_SIZE) {
        kernel_uart_puts("RMFS TEST E1\n");
        return 1;
    }
    static const uint8_t magic[8] = RISC_MV_EXF_MAGIC;
    for (size_t i = 0; i < 8; ++i) {
        if (boot_image[i] != magic[i]) {
            kernel_free(boot_image);
            kernel_uart_puts("RMFS TEST E2\n");
            return 1;
        }
    }
    kernel_free(boot_image);

    static const uint8_t payload[] = {
        'R', 'I', 'S', 'C', '-', 'M', 'V', ' ', 'R', 'M', 'F', 'S', ' ',
        'R', 'W', ' ', 'O', 'K', '\n'
    };
    if (!kernel_vfs_write_file("/KTEST.TXT", payload,
                               sizeof(payload))) {
        kernel_uart_puts("RMFS TEST E3\n");
        return 1;
    }
    uint8_t result[sizeof(payload)];
    size_t result_size;
    if (!kernel_vfs_read_file("/KTEST.TXT", result,
                              sizeof(result), &result_size) ||
        result_size != sizeof(payload)) {
        kernel_uart_puts("RMFS TEST E4\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(payload); ++i) {
        if (result[i] != payload[i]) return 1;
    }
    KernelFdTable *table = kernel_fd_table_create();
    KernelOpenFile *file = kernel_vfs_open(
        "/KTEST.TXT", KERNEL_VFS_OPEN_READ);
    if (table == NULL || file == NULL) {
        kernel_vfs_file_release(file);
        kernel_fd_table_destroy(table);
        return 1;
    }
    int first = kernel_fd_install(table, file, 3);
    int second = kernel_fd_install(table, file, 3);
    KernelOpenFile *acquired = kernel_fd_acquire(table, first);
    kernel_vfs_file_release(acquired);
    kernel_vfs_file_release(file);
    int handles_ok = first == 3 && second == 4 &&
                     kernel_fd_open_count(table) == 2 &&
                     kernel_fd_close(table, first) &&
                     !kernel_fd_close(table, first) &&
                     kernel_fd_close(table, second) &&
                     kernel_fd_open_count(table) == 0;
    kernel_fd_table_destroy(table);
    if (!handles_ok) return 1;

    table = kernel_fd_table_create();
    if (table == NULL || !kernel_fd_populate_standard(table) ||
        kernel_fd_open_count(table) != 3) {
        kernel_fd_table_destroy(table);
        return 1;
    }
    KernelOpenFile *input = kernel_fd_acquire(table, 0);
    KernelOpenFile *output = kernel_fd_acquire(table, 1);
    KernelOpenFile *error = kernel_fd_acquire(table, 2);
    uint8_t permission_probe = 0;
    int standard_ok = input != NULL && output != NULL && error != NULL &&
                      kernel_vfs_file_write(input, &permission_probe, 1) < 0 &&
                      kernel_vfs_file_read(output, &permission_probe, 1) < 0 &&
                      kernel_vfs_file_read(error, &permission_probe, 1) < 0;
    kernel_vfs_file_release(input);
    kernel_vfs_file_release(output);
    kernel_vfs_file_release(error);
    kernel_fd_table_destroy(table);
    if (!standard_ok) return 1;

    file = kernel_vfs_open("/KTEST.TXT",
                           KERNEL_VFS_OPEN_READ | KERNEL_VFS_OPEN_WRITE);
    uint8_t prefix[4];
    static const uint8_t replacement[] = {'F', 'D'};
    uint8_t replaced[2];
    int offset_ok = file != NULL &&
                    kernel_vfs_file_read(file, prefix, sizeof(prefix)) == 4 &&
                    prefix[0] == 'R' && prefix[3] == 'C' &&
                    kernel_vfs_file_seek(file, 4, 0) == 4 &&
                    kernel_vfs_file_write(file, replacement,
                                          sizeof(replacement)) == 2 &&
                    kernel_vfs_file_seek(file, 4, 0) == 4 &&
                    kernel_vfs_file_read(file, replaced,
                                         sizeof(replaced)) == 2 &&
                    replaced[0] == 'F' && replaced[1] == 'D';
    kernel_vfs_file_release(file);
    KernelOpenFile *independent_a = kernel_vfs_open(
        "/KTEST.TXT", KERNEL_VFS_OPEN_READ);
    KernelOpenFile *independent_b = kernel_vfs_open(
        "/KTEST.TXT", KERNEL_VFS_OPEN_READ);
    uint8_t byte_a;
    uint8_t byte_b;
    int independent_ok = independent_a != NULL && independent_b != NULL &&
                         kernel_vfs_file_seek(independent_a, 4, 0) == 4 &&
                         kernel_vfs_file_read(independent_a, &byte_a, 1) == 1 &&
                         kernel_vfs_file_read(independent_b, &byte_b, 1) == 1 &&
                         byte_a == 'F' && byte_b == 'R' &&
                         kernel_vfs_file_write(independent_b, &byte_b, 1) < 0;
    kernel_vfs_file_release(independent_a);
    kernel_vfs_file_release(independent_b);

    KernelOpenFile *append = kernel_vfs_open(
        "/KTEST.TXT", KERNEL_VFS_OPEN_WRITE | KERNEL_VFS_OPEN_APPEND);
    static const uint8_t appended = '!';
    int append_ok = append != NULL &&
                    kernel_vfs_file_seek(append, 0, 0) == 0 &&
                    kernel_vfs_file_write(append, &appended, 1) == 1 &&
                    kernel_vfs_file_seek(append, 0, 2) ==
                        (int64_t)(sizeof(payload) + 1);
    kernel_vfs_file_release(append);
    if (!offset_ok || !independent_ok || !append_ok ||
        !kernel_vfs_write_file("/KTEST.TXT", payload,
                               sizeof(payload))) {
        return 1;
    }

    /* Force the root directory beyond its original 16-entry block. */
    char expansion_path[10];
    expansion_path[0] = '/';
    expansion_path[1] = 'D';
    expansion_path[2] = '0';
    expansion_path[3] = '0';
    expansion_path[4] = '.';
    expansion_path[5] = 'T';
    expansion_path[6] = 'X';
    expansion_path[7] = 'T';
    expansion_path[8] = '\0';
    for (uint32_t index = 0; index < 18; ++index) {
        expansion_path[2] = (char)('0' + index / 10);
        expansion_path[3] = (char)('0' + index % 10);
        uint8_t marker = (uint8_t)index;
        if (!kernel_vfs_write_file(expansion_path, &marker, 1)) return 1;
    }
    uint8_t marker;
    size_t marker_size;
    if (!kernel_vfs_read_file("/D17.TXT", &marker, 1, &marker_size) ||
        marker_size != 1 || marker != 17) return 1;

    /* Rewrites leave small holes; the following sequence requires the
       allocator to combine more than one free run when appropriate. */
    size_t fragment_size = (size_t)3 * RMFS_BLOCK_SIZE;
    uint8_t *fragment = kernel_malloc(fragment_size);
    if (fragment == NULL) return 1;
    for (size_t i = 0; i < fragment_size; ++i) {
        fragment[i] = (uint8_t)(i ^ (i >> 8));
    }
    int fragment_ok =
        kernel_vfs_write_file("/FRAGA.BIN", fragment,
                              (size_t)2 * RMFS_BLOCK_SIZE) &&
        kernel_vfs_write_file("/FRAGB.BIN", fragment,
                              (size_t)2 * RMFS_BLOCK_SIZE) &&
        kernel_vfs_write_file("/FRAGA.BIN", fragment, fragment_size) &&
        kernel_vfs_write_file("/FRAGB.BIN", fragment, fragment_size);
    size_t cow_offset = (size_t)RMFS_BLOCK_SIZE + 17;
    static const uint8_t cow_patch[] = {'C', 'W'};
    KernelOpenFile *cow = NULL;
    if (fragment_ok) {
        cow = kernel_vfs_open("/FRAGB.BIN",
                              KERNEL_VFS_OPEN_READ |
                                  KERNEL_VFS_OPEN_WRITE);
        fragment_ok = cow != NULL &&
                      kernel_vfs_file_seek(cow, (int64_t)cow_offset, 0) ==
                          (int64_t)cow_offset &&
                      kernel_vfs_file_write(cow, cow_patch,
                                            sizeof(cow_patch)) ==
                          (int64_t)sizeof(cow_patch) &&
                      kernel_vfs_file_seek(cow, 0, 2) ==
                          (int64_t)fragment_size;
        kernel_vfs_file_release(cow);
    }
    size_t fragment_read = 0;
    if (fragment_ok) {
        for (size_t i = 0; i < fragment_size; ++i) fragment[i] = 0;
        fragment_ok = kernel_vfs_read_file("/FRAGB.BIN", fragment,
                                           fragment_size, &fragment_read) &&
                      fragment_read == fragment_size;
        for (size_t i = 0; fragment_ok && i < fragment_size; ++i) {
            uint8_t expected = i == cow_offset ? cow_patch[0] :
                               i == cow_offset + 1 ? cow_patch[1] :
                               (uint8_t)(i ^ (i >> 8));
            if (fragment[i] != expected) fragment_ok = 0;
        }
    }
    if (fragment_ok) {
        size_t gap_offset = fragment_size + RMFS_BLOCK_SIZE + 7;
        size_t gap_size = gap_offset + 1 - fragment_size;
        static const uint8_t gap_marker = 'G';
        cow = kernel_vfs_open("/FRAGB.BIN",
                              KERNEL_VFS_OPEN_READ |
                                  KERNEL_VFS_OPEN_WRITE);
        fragment_ok = cow != NULL && gap_size <= fragment_size &&
                      kernel_vfs_file_seek(cow, (int64_t)gap_offset, 0) ==
                          (int64_t)gap_offset &&
                      kernel_vfs_file_write(cow, &gap_marker, 1) == 1 &&
                      kernel_vfs_file_seek(cow, (int64_t)fragment_size, 0) ==
                          (int64_t)fragment_size &&
                      kernel_vfs_file_read(cow, fragment, gap_size) ==
                          (int64_t)gap_size;
        kernel_vfs_file_release(cow);
        for (size_t i = 0; fragment_ok && i < gap_size; ++i) {
            uint8_t expected = i + fragment_size == gap_offset
                                   ? gap_marker : 0;
            if (fragment[i] != expected) fragment_ok = 0;
        }
    }
    kernel_free(fragment);
    if (!fragment_ok || !kernel_vfs_sync()) return 1;
    return 0;
}
