#ifndef CVM_KERNEL_RUNTIME_H
#define CVM_KERNEL_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define KERNEL_SIZE_MAX ((size_t)-1)

typedef struct {
    volatile uint64_t value;
} KernelSpinLock;

typedef struct KernelListNode {
    struct KernelListNode *previous;
    struct KernelListNode *next;
} KernelListNode;

typedef struct {
    KernelListNode sentinel;
    size_t count;
} KernelList;

typedef struct {
    uint8_t *storage;
    size_t capacity;
    size_t head;
    size_t length;
    KernelSpinLock lock;
} KernelByteQueue;

typedef struct {
    uint64_t *words;
    size_t bit_count;
    size_t word_count;
    KernelSpinLock lock;
} KernelBitmap;

#define KERNEL_USER_IMAGE_BASE UINT64_C(0x01000000)
#define KERNEL_USER_IMAGE_LIMIT UINT64_C(0x3E000000)
#define KERNEL_USER_STACK_TOP UINT64_C(0x3F000000)
#define KERNEL_USER_STACK_SIZE UINT64_C(0x00010000)

typedef struct KernelAddressSpace KernelAddressSpace;

typedef struct {
    KernelAddressSpace *address_space;
    uintptr_t entry;
    uintptr_t stack_pointer;
    uintptr_t image_base;
    uintptr_t image_end;
} KernelUserImage;

void kernel_spin_init(KernelSpinLock *lock);
void kernel_spin_lock(KernelSpinLock *lock);
void kernel_spin_unlock(KernelSpinLock *lock);

void kernel_list_init(KernelList *list);
int kernel_list_empty(const KernelList *list);
void kernel_list_push_back(KernelList *list, KernelListNode *node);
KernelListNode *kernel_list_pop_front(KernelList *list);
void kernel_list_remove(KernelList *list, KernelListNode *node);

int kernel_byte_queue_init(KernelByteQueue *queue, size_t capacity);
void kernel_byte_queue_destroy(KernelByteQueue *queue);
int kernel_byte_queue_push(KernelByteQueue *queue, uint8_t value);
int kernel_byte_queue_pop(KernelByteQueue *queue, uint8_t *value);
size_t kernel_byte_queue_size(KernelByteQueue *queue);

int kernel_bitmap_init(KernelBitmap *bitmap, size_t bit_count);
void kernel_bitmap_destroy(KernelBitmap *bitmap);
int kernel_bitmap_test(KernelBitmap *bitmap, size_t bit);
int kernel_bitmap_set(KernelBitmap *bitmap, size_t bit);
int kernel_bitmap_clear(KernelBitmap *bitmap, size_t bit);
int kernel_bitmap_find_clear_and_set(KernelBitmap *bitmap, size_t *bit);

int kernel_runtime_self_test(void);

KernelAddressSpace *kernel_address_space_create(void);
void kernel_address_space_destroy(KernelAddressSpace *space);
uintptr_t kernel_address_space_root(const KernelAddressSpace *space);
int kernel_address_space_map_anonymous(KernelAddressSpace *space,
                                       uintptr_t virtual_address,
                                       uint64_t flags,
                                       uintptr_t *physical_address);
int kernel_address_space_resolve(const KernelAddressSpace *space,
                                 uintptr_t virtual_address,
                                 uint64_t required_flags,
                                 uintptr_t *physical_address);

int kernel_user_image_load(const uint8_t *data,
                           size_t size,
                           KernelUserImage *image);
void kernel_user_image_destroy(KernelUserImage *image);
int kernel_user_loader_self_test(void);
uint8_t *kernel_user_test_program_create(const char *message,
                                         uint32_t loop_count,
                                         size_t *size);

#endif
