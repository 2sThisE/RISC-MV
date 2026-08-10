#ifndef DISPLAY_QUEUE_H
#define DISPLAY_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "display_host.h"

#define DISPLAY_FRAME_QUEUE_CAPACITY 3U

typedef struct {
    uint8_t *pixels;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t frame_number;
} DisplayFrame;

typedef struct {
    DisplayFrame frames[DISPLAY_FRAME_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    uint64_t dropped;
    atomic_flag lock;
} DisplayFrameQueue;

int display_frame_queue_init(DisplayFrameQueue *queue);
void display_frame_queue_destroy(DisplayFrameQueue *queue);
int display_frame_queue_push(DisplayFrameQueue *queue,
                             const void *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             uint32_t format,
                             uint64_t frame_number);
int display_frame_queue_take_latest(DisplayFrameQueue *queue,
                                    DisplayFrame *frame);
void display_frame_release(DisplayFrame *frame);
size_t display_frame_queue_pending(DisplayFrameQueue *queue);
uint64_t display_frame_queue_dropped(DisplayFrameQueue *queue);

#endif
