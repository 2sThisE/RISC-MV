#include "kernel_internal.h"
#include "kernel_runtime.h"

#include <cvm/intrin.h>

void kernel_spin_init(KernelSpinLock *lock)
{
    if (lock != NULL) lock->value = 0;
}

void kernel_spin_lock(KernelSpinLock *lock)
{
    if (lock == NULL) return;
    while (cvm_cas64(&lock->value, 0, 1) != 0) cvm_nop();
    cvm_fence();
}

void kernel_spin_unlock(KernelSpinLock *lock)
{
    if (lock == NULL) return;
    cvm_fence();
    (void)cvm_xchg64(&lock->value, 0);
}

void kernel_list_init(KernelList *list)
{
    if (list == NULL) return;
    list->sentinel.previous = &list->sentinel;
    list->sentinel.next = &list->sentinel;
    list->count = 0;
}

int kernel_list_empty(const KernelList *list)
{
    return list == NULL || list->count == 0;
}

void kernel_list_push_back(KernelList *list, KernelListNode *node)
{
    if (list == NULL || node == NULL) return;
    node->previous = list->sentinel.previous;
    node->next = &list->sentinel;
    list->sentinel.previous->next = node;
    list->sentinel.previous = node;
    ++list->count;
}

KernelListNode *kernel_list_pop_front(KernelList *list)
{
    if (kernel_list_empty(list)) return NULL;
    KernelListNode *node = list->sentinel.next;
    kernel_list_remove(list, node);
    return node;
}

void kernel_list_remove(KernelList *list, KernelListNode *node)
{
    if (list == NULL || node == NULL || node == &list->sentinel ||
        node->previous == NULL || node->next == NULL || list->count == 0) {
        return;
    }
    node->previous->next = node->next;
    node->next->previous = node->previous;
    node->previous = NULL;
    node->next = NULL;
    --list->count;
}

int kernel_byte_queue_init(KernelByteQueue *queue, size_t capacity)
{
    if (queue == NULL || capacity == 0) return 0;
    uint8_t *storage = kernel_malloc(capacity);
    if (storage == NULL) return 0;
    queue->storage = storage;
    queue->capacity = capacity;
    queue->head = 0;
    queue->length = 0;
    kernel_spin_init(&queue->lock);
    return 1;
}

void kernel_byte_queue_destroy(KernelByteQueue *queue)
{
    if (queue == NULL) return;
    kernel_free(queue->storage);
    queue->storage = NULL;
    queue->capacity = 0;
    queue->head = 0;
    queue->length = 0;
}

int kernel_byte_queue_push(KernelByteQueue *queue, uint8_t value)
{
    if (queue == NULL || queue->storage == NULL) return 0;
    kernel_spin_lock(&queue->lock);
    if (queue->length == queue->capacity) {
        kernel_spin_unlock(&queue->lock);
        return 0;
    }
    size_t tail = (queue->head + queue->length) % queue->capacity;
    queue->storage[tail] = value;
    ++queue->length;
    kernel_spin_unlock(&queue->lock);
    return 1;
}

int kernel_byte_queue_pop(KernelByteQueue *queue, uint8_t *value)
{
    if (queue == NULL || queue->storage == NULL || value == NULL) return 0;
    kernel_spin_lock(&queue->lock);
    if (queue->length == 0) {
        kernel_spin_unlock(&queue->lock);
        return 0;
    }
    *value = queue->storage[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->length;
    kernel_spin_unlock(&queue->lock);
    return 1;
}

size_t kernel_byte_queue_size(KernelByteQueue *queue)
{
    if (queue == NULL) return 0;
    kernel_spin_lock(&queue->lock);
    size_t size = queue->length;
    kernel_spin_unlock(&queue->lock);
    return size;
}

int kernel_bitmap_init(KernelBitmap *bitmap, size_t bit_count)
{
    if (bitmap == NULL || bit_count == 0 ||
        bit_count > KERNEL_SIZE_MAX - 63) {
        return 0;
    }
    size_t words = (bit_count + 63) / 64;
    uint64_t *storage = kernel_calloc(words, sizeof(uint64_t));
    if (storage == NULL) return 0;
    bitmap->words = storage;
    bitmap->bit_count = bit_count;
    bitmap->word_count = words;
    kernel_spin_init(&bitmap->lock);
    return 1;
}

void kernel_bitmap_destroy(KernelBitmap *bitmap)
{
    if (bitmap == NULL) return;
    kernel_free(bitmap->words);
    bitmap->words = NULL;
    bitmap->bit_count = 0;
    bitmap->word_count = 0;
}

static int bitmap_change(KernelBitmap *bitmap, size_t bit, int set)
{
    if (bitmap == NULL || bitmap->words == NULL || bit >= bitmap->bit_count) {
        return 0;
    }
    kernel_spin_lock(&bitmap->lock);
    uint64_t mask = UINT64_C(1) << (bit & 63);
    if (set) bitmap->words[bit / 64] |= mask;
    else bitmap->words[bit / 64] &= ~mask;
    kernel_spin_unlock(&bitmap->lock);
    return 1;
}

int kernel_bitmap_test(KernelBitmap *bitmap, size_t bit)
{
    if (bitmap == NULL || bitmap->words == NULL || bit >= bitmap->bit_count) {
        return 0;
    }
    kernel_spin_lock(&bitmap->lock);
    int result = (bitmap->words[bit / 64] &
                  (UINT64_C(1) << (bit & 63))) != 0;
    kernel_spin_unlock(&bitmap->lock);
    return result;
}

int kernel_bitmap_set(KernelBitmap *bitmap, size_t bit)
{
    return bitmap_change(bitmap, bit, 1);
}

int kernel_bitmap_clear(KernelBitmap *bitmap, size_t bit)
{
    return bitmap_change(bitmap, bit, 0);
}

int kernel_bitmap_find_clear_and_set(KernelBitmap *bitmap, size_t *bit)
{
    if (bitmap == NULL || bitmap->words == NULL || bit == NULL) return 0;
    kernel_spin_lock(&bitmap->lock);
    for (size_t candidate = 0; candidate < bitmap->bit_count; ++candidate) {
        uint64_t mask = UINT64_C(1) << (candidate & 63);
        if ((bitmap->words[candidate / 64] & mask) == 0) {
            bitmap->words[candidate / 64] |= mask;
            *bit = candidate;
            kernel_spin_unlock(&bitmap->lock);
            return 1;
        }
    }
    kernel_spin_unlock(&bitmap->lock);
    return 0;
}

int kernel_runtime_self_test(void)
{
    KernelList list;
    KernelListNode first;
    KernelListNode second;
    first.previous = NULL;
    first.next = NULL;
    second.previous = NULL;
    second.next = NULL;
    kernel_list_init(&list);
    kernel_list_push_back(&list, &first);
    kernel_list_push_back(&list, &second);
    if (list.count != 2 || kernel_list_pop_front(&list) != &first ||
        kernel_list_pop_front(&list) != &second || !kernel_list_empty(&list)) {
        return 1;
    }

    KernelByteQueue queue;
    if (!kernel_byte_queue_init(&queue, 4)) return 1;
    for (uint8_t value = 1; value <= 4; ++value) {
        if (!kernel_byte_queue_push(&queue, value)) return 1;
    }
    if (kernel_byte_queue_push(&queue, 5)) return 1;
    uint8_t value;
    for (uint8_t expected = 1; expected <= 2; ++expected) {
        if (!kernel_byte_queue_pop(&queue, &value) || value != expected) {
            return 1;
        }
    }
    if (!kernel_byte_queue_push(&queue, 5) ||
        !kernel_byte_queue_push(&queue, 6) ||
        kernel_byte_queue_size(&queue) != 4) {
        return 1;
    }
    for (uint8_t expected = 3; expected <= 6; ++expected) {
        if (!kernel_byte_queue_pop(&queue, &value) || value != expected) {
            return 1;
        }
    }
    kernel_byte_queue_destroy(&queue);

    KernelBitmap bitmap;
    if (!kernel_bitmap_init(&bitmap, 130) ||
        !kernel_bitmap_set(&bitmap, 0) ||
        !kernel_bitmap_set(&bitmap, 64) ||
        !kernel_bitmap_set(&bitmap, 129) ||
        !kernel_bitmap_test(&bitmap, 64)) {
        return 1;
    }
    size_t selected;
    if (!kernel_bitmap_find_clear_and_set(&bitmap, &selected) ||
        selected != 1 || !kernel_bitmap_clear(&bitmap, 0) ||
        kernel_bitmap_test(&bitmap, 0)) {
        return 1;
    }
    kernel_bitmap_destroy(&bitmap);
    return 0;
}
