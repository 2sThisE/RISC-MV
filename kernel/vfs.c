#include "kernel_internal.h"

int kernel_fat32_mount(void);
int kernel_fat32_read_file(const char *path,
                           void *buffer,
                           size_t capacity,
                           size_t *file_size);
int kernel_fat32_write_file(const char *path,
                            const void *buffer,
                            size_t size);

static int vfs_mounted;

int kernel_vfs_init(void)
{
    vfs_mounted = kernel_fat32_mount();
    return vfs_mounted ? 0 : 1;
}

int kernel_vfs_read_file(const char *path,
                         void *buffer,
                         size_t capacity,
                         size_t *file_size)
{
    return vfs_mounted &&
           kernel_fat32_read_file(path, buffer, capacity, file_size);
}

int kernel_vfs_write_file(const char *path,
                          const void *buffer,
                          size_t size)
{
    return vfs_mounted && kernel_fat32_write_file(path, buffer, size);
}

int kernel_vfs_self_test(void)
{
    uint8_t *boot_image = kernel_malloc(8192);
    if (boot_image == NULL) return 1;
    size_t boot_size;
    if (!kernel_vfs_read_file("/BOOT/BOOT.EXF", boot_image, 8192,
                              &boot_size) ||
        boot_size < CVM_KERNEL_HEADER_SIZE) {
        kernel_free(boot_image);
        return 1;
    }
    static const uint8_t magic[8] = RISC_MV_EXF_MAGIC;
    for (size_t i = 0; i < 8; ++i) {
        if (boot_image[i] != magic[i]) {
            kernel_free(boot_image);
            return 1;
        }
    }
    kernel_free(boot_image);

    static const uint8_t payload[] = {
        'C', 'V', 'M', ' ', 'F', 'A', 'T', '3', '2', ' ',
        'R', 'W', ' ', 'O', 'K', '\n'
    };
    if (!kernel_vfs_write_file("/BOOT/KTEST.TXT", payload,
                               sizeof(payload))) return 1;
    uint8_t result[sizeof(payload)];
    size_t result_size;
    if (!kernel_vfs_read_file("/BOOT/KTEST.TXT", result,
                              sizeof(result), &result_size) ||
        result_size != sizeof(payload)) return 1;
    for (size_t i = 0; i < sizeof(payload); ++i) {
        if (result[i] != payload[i]) return 1;
    }
    return 0;
}
