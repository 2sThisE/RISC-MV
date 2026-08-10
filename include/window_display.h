#ifndef WINDOW_DISPLAY_H
#define WINDOW_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "display_host.h"
#include "keyboard_input.h"

typedef struct WindowDisplayImpl WindowDisplayImpl;

typedef struct {
    VmDisplayHost host;
    WindowDisplayImpl *implementation;
} WindowDisplay;

int window_display_supported(void);
int window_display_init(WindowDisplay *display,
                        const char *title,
                        KeyboardInput *keyboard_input);
void window_display_destroy(WindowDisplay *display);
VmDisplayHost *window_display_host(WindowDisplay *display);
int window_display_wait_until_closed(WindowDisplay *display);
int window_display_frame_info(WindowDisplay *display,
                              size_t *frame_size,
                              uint32_t *width,
                              uint32_t *height,
                              uint32_t *stride,
                              uint32_t *format,
                              uint64_t *frame_number,
                              uint64_t *dropped_frames);

#endif
