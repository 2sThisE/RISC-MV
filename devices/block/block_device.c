#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "block_device.h"

#include "block_protocol.h"
#include "host_thread.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define BLOCK_PATH_CAPACITY 1024U

typedef struct {
    char path[BLOCK_PATH_CAPACITY];
    uint64_t create_size;
    int read_only;
} BlockConfiguration;

typedef struct {
    uint64_t command;
    uint64_t lba;
    uint64_t dma_address;
    uint64_t sector_count;
} BlockRequest;

typedef struct {
    VmDeviceHostApi host;
    VmDeviceResources resources;
    FILE *file;
    uint8_t *staging;
    uint64_t capacity_sectors;
    int read_only;
    atomic_flag state_lock;
    atomic_int stop_requested;
    atomic_int request_pending;
    HostThread worker;
    HostEvent request_event;
    int event_initialized;
    int worker_started;
    uint64_t lba;
    uint64_t dma_address;
    uint64_t sector_count;
    uint64_t status;
    uint64_t error;
    uint64_t control;
    uint64_t irq_status;
    BlockRequest request;
} BlockDevice;

static void block_lock(BlockDevice *device)
{
    while (atomic_flag_test_and_set_explicit(&device->state_lock,
                                             memory_order_acquire)) {
    }
}

static void block_unlock(BlockDevice *device)
{
    atomic_flag_clear_explicit(&device->state_lock, memory_order_release);
}

static char *trim_text(char *text)
{
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t length = strlen(text);
    while (length != 0 &&
           (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
    return text;
}

static int parse_u64(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0' || *text == '-') {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0') {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

static int parse_boolean(const char *text, int *value)
{
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0) {
        *value = 1;
        return 1;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0) {
        *value = 0;
        return 1;
    }
    return 0;
}

static int set_path(BlockConfiguration *configuration, const char *path)
{
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(configuration->path) ||
        configuration->path[0] != '\0') {
        return 0;
    }
    memcpy(configuration->path, path, length + 1);
    return 1;
}

static int parse_configuration(const char *text,
                               BlockConfiguration *configuration)
{
    if (text == NULL || configuration == NULL || *text == '\0') {
        return 0;
    }
    *configuration = (BlockConfiguration){0};
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, text, length + 1);

    int create_seen = 0;
    int readonly_seen = 0;
    int okay = 1;
    for (char *item = strtok(copy, ";");
         item != NULL;
         item = strtok(NULL, ";")) {
        item = trim_text(item);
        char *equals = strchr(item, '=');
        if (equals == NULL) {
            okay = set_path(configuration, item);
        } else {
            *equals = '\0';
            char *key = trim_text(item);
            char *value = trim_text(equals + 1);
            if (strcmp(key, "path") == 0) {
                okay = set_path(configuration, value);
            } else if (strcmp(key, "create") == 0 && !create_seen) {
                create_seen = 1;
                okay = parse_u64(value, &configuration->create_size);
            } else if (strcmp(key, "readonly") == 0 && !readonly_seen) {
                readonly_seen = 1;
                okay = parse_boolean(value, &configuration->read_only);
            } else {
                okay = 0;
            }
        }
        if (!okay) {
            break;
        }
    }
    if (configuration->path[0] == '\0' ||
        (configuration->create_size != 0 &&
         (configuration->create_size < VM_BLOCK_SECTOR_SIZE ||
          configuration->create_size % VM_BLOCK_SECTOR_SIZE != 0 ||
          configuration->read_only))) {
        okay = 0;
    }
    free(copy);
    return okay;
}

static int block_file_seek(FILE *file, uint64_t offset, int origin)
{
    if (offset > INT64_MAX) {
        return 0;
    }
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, origin) == 0;
#else
    if (offset > (uint64_t)LONG_MAX) {
        return 0;
    }
    return fseek(file, (long)offset, origin) == 0;
#endif
}

static int block_file_size(FILE *file, uint64_t *size)
{
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return 0;
    }
    __int64 measured = _ftelli64(file);
    if (measured < 0 || _fseeki64(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    *size = (uint64_t)measured;
#else
    if (fseek(file, 0, SEEK_END) != 0) {
        return 0;
    }
    long measured = ftell(file);
    if (measured < 0 || fseek(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    *size = (uint64_t)measured;
#endif
    return 1;
}

static int block_file_flush(FILE *file)
{
    if (fflush(file) != 0) {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static FILE *create_backing_file_exclusive(const char *path)
{
#ifdef _WIN32
    int descriptor = _open(path,
                           _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
    if (descriptor < 0) {
        return NULL;
    }
    FILE *file = _fdopen(descriptor, "w+b");
    if (file == NULL) {
        _close(descriptor);
    }
#else
    int descriptor = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor < 0) {
        return NULL;
    }
    FILE *file = fdopen(descriptor, "w+b");
    if (file == NULL) {
        close(descriptor);
    }
#endif
    return file;
}

static FILE *open_backing_file(const BlockConfiguration *configuration,
                               uint64_t *size)
{
    FILE *file = fopen(configuration->path,
                       configuration->read_only ? "rb" : "r+b");
    if (file == NULL && configuration->create_size != 0) {
        file = create_backing_file_exclusive(configuration->path);
        if (file == NULL ||
            !block_file_seek(file,
                             configuration->create_size - 1,
                             SEEK_SET) ||
            fputc(0, file) == EOF || !block_file_flush(file)) {
            if (file != NULL) {
                fclose(file);
            }
            return NULL;
        }
    }
    if (file == NULL || !block_file_size(file, size) ||
        *size == 0 || *size % VM_BLOCK_SECTOR_SIZE != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    return file;
}

static uint64_t ready_status(const BlockDevice *device)
{
    return VM_BLOCK_STATUS_READY |
           (device->read_only ? VM_BLOCK_STATUS_READ_ONLY : 0);
}

static void raise_irq(BlockDevice *device, uint64_t reason)
{
    block_lock(device);
    device->irq_status |= reason;
    int enabled = (device->control & VM_BLOCK_CONTROL_IRQ_ENABLE) != 0;
    block_unlock(device);
    if (enabled && device->host.raise_irq != NULL) {
        (void)device->host.raise_irq(device->host.context,
                                     device->resources.irqs[0]);
    }
}

static VmBlockError execute_request(BlockDevice *device,
                                    const BlockRequest *request)
{
    if (request->command == VM_BLOCK_COMMAND_FLUSH) {
        return (device->read_only || block_file_flush(device->file))
                   ? VM_BLOCK_ERROR_NONE
                   : VM_BLOCK_ERROR_IO;
    }

    size_t byte_count =
        (size_t)(request->sector_count * VM_BLOCK_SECTOR_SIZE);
    uint64_t file_offset = request->lba * VM_BLOCK_SECTOR_SIZE;
    if (request->command == VM_BLOCK_COMMAND_WRITE) {
        if (device->host.dma_read == NULL ||
            !device->host.dma_read(device->host.context,
                                   request->dma_address,
                                   device->staging,
                                   byte_count)) {
            return VM_BLOCK_ERROR_DMA;
        }
        clearerr(device->file);
        if (!block_file_seek(device->file, file_offset, SEEK_SET) ||
            fwrite(device->staging, 1, byte_count, device->file) !=
                byte_count) {
            return VM_BLOCK_ERROR_IO;
        }
        return VM_BLOCK_ERROR_NONE;
    }

    clearerr(device->file);
    if (!block_file_seek(device->file, file_offset, SEEK_SET) ||
        fread(device->staging, 1, byte_count, device->file) != byte_count) {
        return VM_BLOCK_ERROR_IO;
    }
    if (device->host.dma_write == NULL ||
        !device->host.dma_write(device->host.context,
                                request->dma_address,
                                device->staging,
                                byte_count)) {
        return VM_BLOCK_ERROR_DMA;
    }
    return VM_BLOCK_ERROR_NONE;
}

static int block_worker(void *context)
{
    BlockDevice *device = context;
    for (;;) {
        if (!host_event_wait(&device->request_event)) return 0;
        if (atomic_load_explicit(&device->stop_requested,
                                 memory_order_acquire)) break;
        if (!atomic_exchange_explicit(&device->request_pending,
                                      0,
                                      memory_order_acquire)) continue;

        block_lock(device);
        BlockRequest request = device->request;
        block_unlock(device);
        VmBlockError error = execute_request(device, &request);

        block_lock(device);
        device->error = (uint64_t)error;
        device->status = ready_status(device) |
            (error == VM_BLOCK_ERROR_NONE
                 ? VM_BLOCK_STATUS_DONE
                 : VM_BLOCK_STATUS_ERROR);
        block_unlock(device);
        raise_irq(device,
                  error == VM_BLOCK_ERROR_NONE
                      ? VM_BLOCK_IRQ_COMPLETE
                      : VM_BLOCK_IRQ_ERROR);
    }
    return 0;
}

static uint64_t submit_request(BlockDevice *device, uint64_t command)
{
    if ((device->status & VM_BLOCK_STATUS_BUSY) != 0) {
        device->error = VM_BLOCK_ERROR_BUSY;
        return VM_BLOCK_IRQ_ERROR;
    }
    if (command != VM_BLOCK_COMMAND_READ &&
        command != VM_BLOCK_COMMAND_WRITE &&
        command != VM_BLOCK_COMMAND_FLUSH) {
        device->status = ready_status(device) | VM_BLOCK_STATUS_ERROR;
        device->error = VM_BLOCK_ERROR_COMMAND;
        return VM_BLOCK_IRQ_ERROR;
    }
    if (command == VM_BLOCK_COMMAND_WRITE && device->read_only) {
        device->status = ready_status(device) | VM_BLOCK_STATUS_ERROR;
        device->error = VM_BLOCK_ERROR_READ_ONLY;
        return VM_BLOCK_IRQ_ERROR;
    }
    if (command != VM_BLOCK_COMMAND_FLUSH &&
        (device->sector_count == 0 ||
         device->sector_count > VM_BLOCK_MAX_TRANSFER_SECTORS ||
         device->lba >= device->capacity_sectors ||
         device->sector_count > device->capacity_sectors - device->lba)) {
        device->status = ready_status(device) | VM_BLOCK_STATUS_ERROR;
        device->error = VM_BLOCK_ERROR_RANGE;
        return VM_BLOCK_IRQ_ERROR;
    }

    device->request = (BlockRequest){
        .command = command,
        .lba = device->lba,
        .dma_address = device->dma_address,
        .sector_count = device->sector_count
    };
    device->status = VM_BLOCK_STATUS_BUSY |
        (device->read_only ? VM_BLOCK_STATUS_READ_ONLY : 0);
    device->error = VM_BLOCK_ERROR_NONE;
    atomic_store_explicit(&device->request_pending,
                          1,
                          memory_order_release);
    (void)host_event_signal(&device->request_event);
    return 0;
}

static int block_create(const VmDeviceHostApi *host,
                        const VmDeviceResources *resources,
                        const char *configuration_text,
                        void **device_context)
{
    if (host == NULL || resources == NULL || device_context == NULL ||
        host->abi_version != VM_DEVICE_ABI_VERSION ||
        host->struct_size < sizeof(*host) ||
        host->dma_read == NULL || host->dma_write == NULL ||
        resources->struct_size < sizeof(*resources) ||
        resources->bar_count != 1 || resources->irq_count != 1) {
        return 0;
    }

    BlockConfiguration configuration;
    if (!parse_configuration(configuration_text, &configuration)) {
        return 0;
    }
    uint64_t file_size;
    FILE *file = open_backing_file(&configuration, &file_size);
    if (file == NULL) {
        return 0;
    }

    BlockDevice *device = calloc(1, sizeof(*device));
    uint8_t *staging = malloc((size_t)(VM_BLOCK_MAX_TRANSFER_SECTORS *
                                       VM_BLOCK_SECTOR_SIZE));
    if (device == NULL || staging == NULL) {
        free(staging);
        free(device);
        fclose(file);
        return 0;
    }
    device->host = *host;
    device->resources = *resources;
    device->file = file;
    device->staging = staging;
    device->capacity_sectors = file_size / VM_BLOCK_SECTOR_SIZE;
    device->read_only = configuration.read_only;
    device->status = ready_status(device);
    atomic_flag_clear(&device->state_lock);
    atomic_init(&device->stop_requested, 0);
    atomic_init(&device->request_pending, 0);

    if (!host_event_init(&device->request_event)) {
        free(staging);
        fclose(file);
        free(device);
        return 0;
    }
    device->event_initialized = 1;
    if (!host_thread_create(&device->worker, block_worker, device)) {
        host_event_destroy(&device->request_event);
        free(staging);
        fclose(file);
        free(device);
        return 0;
    }
    device->worker_started = 1;
    *device_context = device;
    if (device->host.log != NULL) {
        char message[256];
        (void)snprintf(message,
                       sizeof(message),
                       "block device attached: %llu sectors%s",
                       (unsigned long long)device->capacity_sectors,
                       device->read_only ? " (read-only)" : "");
        device->host.log(device->host.context, 1, message);
    }
    return 1;
}

static void block_destroy(void *device_context)
{
    BlockDevice *device = device_context;
    if (device == NULL) {
        return;
    }
    atomic_store_explicit(&device->stop_requested, 1, memory_order_release);
    if (device->event_initialized) {
        (void)host_event_signal(&device->request_event);
    }
    if (device->worker_started) {
        (void)host_thread_join(&device->worker, NULL);
    }
    if (device->event_initialized) {
        host_event_destroy(&device->request_event);
    }
    if (!device->read_only) {
        (void)block_file_flush(device->file);
    }
    fclose(device->file);
    free(device->staging);
    free(device);
}

static int block_read(void *device_context,
                      uint32_t bar,
                      uint64_t offset,
                      uint32_t width,
                      uint64_t *value)
{
    BlockDevice *device = device_context;
    if (device == NULL || value == NULL || bar != 0 || width != 8) {
        return 0;
    }
    block_lock(device);
    int okay = 1;
    switch (offset) {
        case VM_BLOCK_MAGIC_OFFSET: *value = VM_BLOCK_MAGIC; break;
        case VM_BLOCK_VERSION_OFFSET: *value = VM_BLOCK_VERSION; break;
        case VM_BLOCK_FEATURES_OFFSET:
            *value = VM_BLOCK_FEATURE_DMA | VM_BLOCK_FEATURE_FLUSH |
                (device->read_only ? VM_BLOCK_FEATURE_READ_ONLY : 0);
            break;
        case VM_BLOCK_CAPACITY_OFFSET:
            *value = device->capacity_sectors;
            break;
        case VM_BLOCK_SECTOR_SIZE_OFFSET:
            *value = VM_BLOCK_SECTOR_SIZE;
            break;
        case VM_BLOCK_MAX_TRANSFER_OFFSET:
            *value = VM_BLOCK_MAX_TRANSFER_SECTORS;
            break;
        case VM_BLOCK_LBA_OFFSET: *value = device->lba; break;
        case VM_BLOCK_DMA_ADDRESS_OFFSET:
            *value = device->dma_address;
            break;
        case VM_BLOCK_SECTOR_COUNT_OFFSET:
            *value = device->sector_count;
            break;
        case VM_BLOCK_COMMAND_OFFSET: *value = VM_BLOCK_COMMAND_NONE; break;
        case VM_BLOCK_STATUS_OFFSET: *value = device->status; break;
        case VM_BLOCK_ERROR_OFFSET: *value = device->error; break;
        case VM_BLOCK_CONTROL_OFFSET: *value = device->control; break;
        case VM_BLOCK_IRQ_STATUS_OFFSET: *value = device->irq_status; break;
        case VM_BLOCK_IRQ_ACK_OFFSET: *value = 0; break;
        default: okay = 0; break;
    }
    block_unlock(device);
    return okay;
}

static int block_write(void *device_context,
                       uint32_t bar,
                       uint64_t offset,
                       uint32_t width,
                       uint64_t value)
{
    BlockDevice *device = device_context;
    if (device == NULL || bar != 0 || width != 8) {
        return 0;
    }

    uint64_t immediate_irq = 0;
    block_lock(device);
    int okay = 1;
    switch (offset) {
        case VM_BLOCK_LBA_OFFSET:
            device->lba = value;
            break;
        case VM_BLOCK_DMA_ADDRESS_OFFSET:
            device->dma_address = value;
            break;
        case VM_BLOCK_SECTOR_COUNT_OFFSET:
            device->sector_count = value;
            break;
        case VM_BLOCK_COMMAND_OFFSET:
            immediate_irq = submit_request(device, value);
            break;
        case VM_BLOCK_STATUS_OFFSET:
            device->status &= ~(value &
                (VM_BLOCK_STATUS_DONE | VM_BLOCK_STATUS_ERROR));
            if ((value & VM_BLOCK_STATUS_ERROR) != 0) {
                device->error = VM_BLOCK_ERROR_NONE;
            }
            break;
        case VM_BLOCK_CONTROL_OFFSET:
            device->control = value & VM_BLOCK_CONTROL_IRQ_ENABLE;
            break;
        case VM_BLOCK_IRQ_ACK_OFFSET:
            device->irq_status &= ~(value &
                (VM_BLOCK_IRQ_COMPLETE | VM_BLOCK_IRQ_ERROR));
            break;
        default:
            okay = 0;
            break;
    }
    block_unlock(device);
    if (immediate_irq != 0) {
        raise_irq(device, immediate_irq);
    }
    return okay;
}

static void block_reset(void *device_context)
{
    BlockDevice *device = device_context;
    if (device == NULL) {
        return;
    }
    block_lock(device);
    if ((device->status & VM_BLOCK_STATUS_BUSY) == 0) {
        device->lba = 0;
        device->dma_address = 0;
        device->sector_count = 0;
        device->status = ready_status(device);
        device->error = VM_BLOCK_ERROR_NONE;
        device->control = 0;
        device->irq_status = 0;
    }
    block_unlock(device);
}

static const VmDeviceModule BLOCK_MODULE = {
    .abi_version = VM_DEVICE_ABI_VERSION,
    .struct_size = sizeof(VmDeviceModule),
    .descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .name = "raw-block-device",
        .device_class = VM_DEVICE_CLASS_STORAGE,
        .vendor_id = UINT32_C(0x564D),
        .device_id = UINT32_C(0x2000),
        .device_version = 1,
        .features = VM_BLOCK_FEATURE_DMA | VM_BLOCK_FEATURE_FLUSH,
        .bar_count = 1,
        .irq_count = 1,
        .bar_sizes = { VM_BLOCK_MMIO_SIZE },
        .bar_alignments = { UINT64_C(4096) }
    },
    .create = block_create,
    .destroy = block_destroy,
    .read = block_read,
    .write = block_write,
    .tick = NULL,
    .reset = block_reset
};

const VmDeviceModule *vm_block_device_module(void)
{
    return &BLOCK_MODULE;
}

#ifndef VM_BLOCK_DEVICE_STATIC
#ifdef _WIN32
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const VmDeviceModule *vm_device_query(uint32_t host_abi_version)
{
    return host_abi_version == VM_DEVICE_ABI_VERSION
               ? &BLOCK_MODULE
               : NULL;
}
#endif
