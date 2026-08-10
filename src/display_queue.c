#include "display_queue.h"

#include <stdlib.h>
#include <string.h>

static void queue_lock(DisplayFrameQueue *queue)
{
    while (atomic_flag_test_and_set_explicit(&queue->lock,
                                             memory_order_acquire)) {
    }
}

static void queue_unlock(DisplayFrameQueue *queue)
{
    atomic_flag_clear_explicit(&queue->lock, memory_order_release);
}

void display_frame_release(DisplayFrame *frame)
{
    if (frame == NULL) {
        return;
    }
    free(frame->pixels);
    *frame = (DisplayFrame){0};
}

int display_frame_queue_init(DisplayFrameQueue *queue)
{
    if (queue == NULL) {
        return 0;
    }
    *queue = (DisplayFrameQueue){0};
    atomic_flag_clear(&queue->lock);
    return 1;
}

void display_frame_queue_destroy(DisplayFrameQueue *queue)
{
    if (queue == NULL) {
        return;
    }
    for (size_t i = 0; i < DISPLAY_FRAME_QUEUE_CAPACITY; ++i) {
        display_frame_release(&queue->frames[i]);
    }
    *queue = (DisplayFrameQueue){0};
}

int display_frame_queue_push(DisplayFrameQueue *queue,
                             const void *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             uint32_t format,
                             uint64_t frame_number)
{
    if (queue == NULL || pixels == NULL || width == 0 || height == 0 ||
        format != VM_PIXEL_FORMAT_XRGB8888 ||
        width > UINT32_MAX / UINT32_C(4) ||
        stride < width * UINT32_C(4) ||
        (size_t)height > SIZE_MAX / stride) {
        return 0;
    }

    uint32_t packed_stride = width * UINT32_C(4);
    size_t size = (size_t)packed_stride * height;
    uint8_t *copy = malloc(size);
    if (copy == NULL) {
        return 0;
    }
    const uint8_t *source = pixels;
    for (uint32_t row = 0; row < height; ++row) {
        memcpy(copy + (size_t)row * packed_stride,
               source + (size_t)row * stride,
               packed_stride);
    }

    DisplayFrame replacement = {
        .pixels = copy,
        .size = size,
        .width = width,
        .height = height,
        .stride = packed_stride,
        .format = format,
        .frame_number = frame_number
    };
    DisplayFrame discarded = {0};

    queue_lock(queue);
    if (queue->count == DISPLAY_FRAME_QUEUE_CAPACITY) {
        discarded = queue->frames[queue->head];
        queue->frames[queue->head] = (DisplayFrame){0};
        queue->head = (queue->head + 1) % DISPLAY_FRAME_QUEUE_CAPACITY;
        --queue->count;
        ++queue->dropped;
    }
    size_t tail = (queue->head + queue->count) %
                  DISPLAY_FRAME_QUEUE_CAPACITY;
    queue->frames[tail] = replacement;
    ++queue->count;
    queue_unlock(queue);

    display_frame_release(&discarded);
    return 1;
}

int display_frame_queue_take_latest(DisplayFrameQueue *queue,
                                    DisplayFrame *frame)
{
    if (queue == NULL || frame == NULL) {
        return 0;
    }

    DisplayFrame discarded[DISPLAY_FRAME_QUEUE_CAPACITY] = {0};
    size_t discarded_count = 0;
    DisplayFrame latest = {0};

    queue_lock(queue);
    if (queue->count != 0) {
        for (size_t i = 0; i < queue->count; ++i) {
            size_t index = (queue->head + i) %
                           DISPLAY_FRAME_QUEUE_CAPACITY;
            if (i + 1 == queue->count) {
                latest = queue->frames[index];
            } else {
                discarded[discarded_count++] = queue->frames[index];
                ++queue->dropped;
            }
            queue->frames[index] = (DisplayFrame){0};
        }
        queue->head = 0;
        queue->count = 0;
    }
    queue_unlock(queue);

    for (size_t i = 0; i < discarded_count; ++i) {
        display_frame_release(&discarded[i]);
    }
    if (latest.pixels == NULL) {
        return 0;
    }
    *frame = latest;
    return 1;
}

size_t display_frame_queue_pending(DisplayFrameQueue *queue)
{
    if (queue == NULL) {
        return 0;
    }
    queue_lock(queue);
    size_t count = queue->count;
    queue_unlock(queue);
    return count;
}

uint64_t display_frame_queue_dropped(DisplayFrameQueue *queue)
{
    if (queue == NULL) {
        return 0;
    }
    queue_lock(queue);
    uint64_t dropped = queue->dropped;
    queue_unlock(queue);
    return dropped;
}
