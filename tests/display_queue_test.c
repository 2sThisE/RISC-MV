#include "display_queue.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int test_display_queue(void)
{
    DisplayFrameQueue queue;
    assert(display_frame_queue_init(&queue));

    for (uint64_t frame_number = 1; frame_number <= 4; ++frame_number) {
        uint8_t pixels[12] = {
            (uint8_t)frame_number, 1, 2, 3,
            4, 5, 6, 7,
            0xEE, 0xEE, 0xEE, 0xEE
        };
        assert(display_frame_queue_push(&queue,
                                        pixels,
                                        2,
                                        1,
                                        sizeof(pixels),
                                        VM_PIXEL_FORMAT_XRGB8888,
                                        frame_number));
    }

    assert(display_frame_queue_pending(&queue) ==
           DISPLAY_FRAME_QUEUE_CAPACITY);
    assert(display_frame_queue_dropped(&queue) == 1);

    DisplayFrame latest;
    assert(display_frame_queue_take_latest(&queue, &latest));
    assert(latest.frame_number == 4);
    assert(latest.width == 2 && latest.height == 1);
    assert(latest.stride == 8 && latest.size == 8);
    const uint8_t expected[8] = {4, 1, 2, 3, 4, 5, 6, 7};
    assert(memcmp(latest.pixels, expected, sizeof(expected)) == 0);
    assert(display_frame_queue_pending(&queue) == 0);
    assert(display_frame_queue_dropped(&queue) == 3);
    assert(!display_frame_queue_take_latest(&queue, &latest));

    display_frame_release(&latest);
    display_frame_queue_destroy(&queue);
    return 0;
}
