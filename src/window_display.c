#include "window_display.h"

#include "display_queue.h"
#include "keyboard_protocol.h"

#include <stdlib.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "host_thread.h"

#define WINDOW_DISPLAY_FRAME_MESSAGE (WM_APP + 1U)
#define WINDOW_DISPLAY_CLASS_NAME L"CustomVmDisplayWindow"

struct WindowDisplayImpl {
    CRITICAL_SECTION state_lock;
    HANDLE ready_event;
    HostThread thread;
    int thread_started;
    HWND window;
    int initialization_failed;
    int closed;
    wchar_t title[128];
    DisplayFrameQueue queue;
    DisplayFrame current;
    size_t submitted_size;
    uint32_t submitted_width;
    uint32_t submitted_height;
    uint32_t submitted_stride;
    uint32_t submitted_format;
    uint64_t submitted_frame;
    KeyboardInput *keyboard_input;
    uint8_t pressed[256];
    uint8_t key_extended[256];
    uint8_t modifiers;
};

static void state_lock(WindowDisplayImpl *implementation)
{
    EnterCriticalSection(&implementation->state_lock);
}

static void state_unlock(WindowDisplayImpl *implementation)
{
    LeaveCriticalSection(&implementation->state_lock);
}

static int window_resize(void *context,
                         uint32_t width,
                         uint32_t height)
{
    WindowDisplay *display = context;
    return display != NULL && display->implementation != NULL &&
           width != 0 && height != 0;
}

static int window_present(void *context,
                          const void *pixels,
                          uint32_t width,
                          uint32_t height,
                          uint32_t stride,
                          uint32_t format,
                          uint64_t frame_number)
{
    WindowDisplay *display = context;
    if (display == NULL || display->implementation == NULL) {
        return 0;
    }
    WindowDisplayImpl *implementation = display->implementation;

    state_lock(implementation);
    int closed = implementation->closed;
    state_unlock(implementation);
    if (closed || !display_frame_queue_push(&implementation->queue,
                                            pixels,
                                            width,
                                            height,
                                            stride,
                                            format,
                                            frame_number)) {
        return 0;
    }

    state_lock(implementation);
    HWND window = implementation->window;
    closed = implementation->closed;
    if (!closed) {
        implementation->submitted_size =
            (size_t)width * UINT32_C(4) * height;
        implementation->submitted_width = width;
        implementation->submitted_height = height;
        implementation->submitted_stride = width * UINT32_C(4);
        implementation->submitted_format = format;
        implementation->submitted_frame = frame_number;
    }
    state_unlock(implementation);

    return !closed && window != NULL &&
           PostMessageW(window,
                        WINDOW_DISPLAY_FRAME_MESSAGE,
                        0,
                        0) != 0;
}

static void window_paint(WindowDisplayImpl *implementation,
                         HWND window)
{
    PAINTSTRUCT paint;
    HDC device_context = BeginPaint(window, &paint);
    if (device_context == NULL) {
        return;
    }

    RECT client;
    (void)GetClientRect(window, &client);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    (void)FillRect(device_context, &client, black);

    DisplayFrame *frame = &implementation->current;
    int client_width = client.right - client.left;
    int client_height = client.bottom - client.top;
    if (frame->pixels != NULL && client_width > 0 && client_height > 0) {
        int draw_width = client_width;
        int draw_height = (int)((int64_t)client_width * frame->height /
                                frame->width);
        if (draw_height > client_height) {
            draw_height = client_height;
            draw_width = (int)((int64_t)client_height * frame->width /
                               frame->height);
        }
        int draw_x = (client_width - draw_width) / 2;
        int draw_y = (client_height - draw_height) / 2;

        BITMAPINFO bitmap = {0};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = (LONG)frame->width;
        bitmap.bmiHeader.biHeight = -(LONG)frame->height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        (void)StretchDIBits(device_context,
                            draw_x,
                            draw_y,
                            draw_width,
                            draw_height,
                            0,
                            0,
                            (int)frame->width,
                            (int)frame->height,
                            frame->pixels,
                            &bitmap,
                            DIB_RGB_COLORS,
                            SRCCOPY);
    }
    (void)EndPaint(window, &paint);
}

static uint16_t keyboard_usage(WPARAM word_parameter,
                               LPARAM long_parameter)
{
    unsigned int key = (unsigned int)word_parameter;
    int extended = (long_parameter & (LPARAM)(UINT64_C(1) << 24)) != 0;
    unsigned int scan = ((unsigned long long)long_parameter >> 16) & 0xFFU;

    if (key >= 'A' && key <= 'Z') {
        return (uint16_t)(VM_KEY_A + (key - 'A'));
    }
    if (key >= '1' && key <= '9') {
        return (uint16_t)(VM_KEY_1 + (key - '1'));
    }
    if (key >= VK_F1 && key <= VK_F12) {
        return (uint16_t)(VM_KEY_F1 + (key - VK_F1));
    }
    if (key >= VK_F13 && key <= VK_F24) {
        return (uint16_t)(VM_KEY_F13 + (key - VK_F13));
    }
    if (key >= VK_NUMPAD1 && key <= VK_NUMPAD9) {
        return (uint16_t)(VM_KEY_KEYPAD_1 + (key - VK_NUMPAD1));
    }

    switch (key) {
        case '0': return VM_KEY_0;
        case VK_RETURN:
            return extended ? VM_KEY_KEYPAD_ENTER : VM_KEY_ENTER;
        case VK_ESCAPE: return VM_KEY_ESCAPE;
        case VK_BACK: return VM_KEY_BACKSPACE;
        case VK_TAB: return VM_KEY_TAB;
        case VK_SPACE: return VM_KEY_SPACE;
        case VK_OEM_MINUS: return VM_KEY_MINUS;
        case VK_OEM_PLUS: return VM_KEY_EQUAL;
        case VK_OEM_4: return VM_KEY_LEFT_BRACKET;
        case VK_OEM_6: return VM_KEY_RIGHT_BRACKET;
        case VK_OEM_5: return VM_KEY_BACKSLASH;
        case VK_OEM_1: return VM_KEY_SEMICOLON;
        case VK_OEM_7: return VM_KEY_APOSTROPHE;
        case VK_OEM_3: return VM_KEY_GRAVE;
        case VK_OEM_COMMA: return VM_KEY_COMMA;
        case VK_OEM_PERIOD: return VM_KEY_PERIOD;
        case VK_OEM_2: return VM_KEY_SLASH;
        case VK_CAPITAL: return VM_KEY_CAPS_LOCK;
        case VK_SNAPSHOT: return VM_KEY_PRINT_SCREEN;
        case VK_SCROLL: return VM_KEY_SCROLL_LOCK;
        case VK_PAUSE: return VM_KEY_PAUSE;
        case VK_INSERT:
            return extended ? VM_KEY_INSERT : VM_KEY_KEYPAD_0;
        case VK_HOME:
            return extended ? VM_KEY_HOME : VM_KEY_KEYPAD_1 + 6;
        case VK_PRIOR:
            return extended ? VM_KEY_PAGE_UP : VM_KEY_KEYPAD_1 + 8;
        case VK_DELETE:
            return extended ? VM_KEY_DELETE : VM_KEY_KEYPAD_DECIMAL;
        case VK_END:
            return extended ? VM_KEY_END : VM_KEY_KEYPAD_1;
        case VK_NEXT:
            return extended ? VM_KEY_PAGE_DOWN : VM_KEY_KEYPAD_1 + 2;
        case VK_RIGHT:
            return extended ? VM_KEY_RIGHT : VM_KEY_KEYPAD_1 + 5;
        case VK_LEFT:
            return extended ? VM_KEY_LEFT : VM_KEY_KEYPAD_1 + 3;
        case VK_DOWN:
            return extended ? VM_KEY_DOWN : VM_KEY_KEYPAD_1 + 1;
        case VK_UP:
            return extended ? VM_KEY_UP : VM_KEY_KEYPAD_1 + 7;
        case VK_CLEAR: return VM_KEY_KEYPAD_1 + 4;
        case VK_NUMLOCK: return VM_KEY_NUM_LOCK;
        case VK_DIVIDE: return VM_KEY_KEYPAD_DIVIDE;
        case VK_MULTIPLY: return VM_KEY_KEYPAD_MULTIPLY;
        case VK_SUBTRACT: return VM_KEY_KEYPAD_SUBTRACT;
        case VK_ADD: return VM_KEY_KEYPAD_ADD;
        case VK_NUMPAD0: return VM_KEY_KEYPAD_0;
        case VK_DECIMAL: return VM_KEY_KEYPAD_DECIMAL;
        case VK_APPS: return VM_KEY_APPLICATION;
        case VK_LCONTROL: return VM_KEY_LEFT_CTRL;
        case VK_RCONTROL: return VM_KEY_RIGHT_CTRL;
        case VK_CONTROL:
            return extended ? VM_KEY_RIGHT_CTRL : VM_KEY_LEFT_CTRL;
        case VK_LSHIFT: return VM_KEY_LEFT_SHIFT;
        case VK_RSHIFT: return VM_KEY_RIGHT_SHIFT;
        case VK_SHIFT:
            return scan == 0x36U ? VM_KEY_RIGHT_SHIFT : VM_KEY_LEFT_SHIFT;
        case VK_LMENU: return VM_KEY_LEFT_ALT;
        case VK_RMENU: return VM_KEY_RIGHT_ALT;
        case VK_MENU:
            return extended ? VM_KEY_RIGHT_ALT : VM_KEY_LEFT_ALT;
        case VK_LWIN: return VM_KEY_LEFT_GUI;
        case VK_RWIN: return VM_KEY_RIGHT_GUI;
        case VK_HANGUL: return VM_KEY_LANG1;
        case VK_HANJA: return VM_KEY_LANG2;
        default: return VM_KEY_NONE;
    }
}

static uint8_t modifier_for_usage(uint16_t usage)
{
    switch (usage) {
        case VM_KEY_LEFT_CTRL: return VM_KEYBOARD_MOD_LEFT_CTRL;
        case VM_KEY_LEFT_SHIFT: return VM_KEYBOARD_MOD_LEFT_SHIFT;
        case VM_KEY_LEFT_ALT: return VM_KEYBOARD_MOD_LEFT_ALT;
        case VM_KEY_LEFT_GUI: return VM_KEYBOARD_MOD_LEFT_GUI;
        case VM_KEY_RIGHT_CTRL: return VM_KEYBOARD_MOD_RIGHT_CTRL;
        case VM_KEY_RIGHT_SHIFT: return VM_KEYBOARD_MOD_RIGHT_SHIFT;
        case VM_KEY_RIGHT_ALT: return VM_KEYBOARD_MOD_RIGHT_ALT;
        case VM_KEY_RIGHT_GUI: return VM_KEYBOARD_MOD_RIGHT_GUI;
        default: return 0;
    }
}

static void window_key_event(WindowDisplayImpl *implementation,
                             WPARAM word_parameter,
                             LPARAM long_parameter,
                             int down)
{
    if (implementation == NULL || implementation->keyboard_input == NULL) {
        return;
    }
    uint16_t usage = keyboard_usage(word_parameter, long_parameter);
    if (usage == VM_KEY_NONE || usage >= 256U) {
        return;
    }

    int extended = (long_parameter &
                    (LPARAM)(UINT64_C(1) << 24)) != 0;
    int repeat = down &&
                 ((long_parameter &
                   (LPARAM)(UINT64_C(1) << 30)) != 0 ||
                  implementation->pressed[usage] != 0);
    uint8_t modifier = modifier_for_usage(usage);
    implementation->pressed[usage] = down ? 1U : 0U;
    implementation->key_extended[usage] = extended ? 1U : 0U;
    if (down) {
        implementation->modifiers |= modifier;
    } else {
        implementation->modifiers &= (uint8_t)~modifier;
    }

    uint64_t event = vm_keyboard_event_make(
        usage,
        down,
        repeat,
        extended,
        implementation->modifiers);
    (void)keyboard_input_emit(implementation->keyboard_input, event);
}

static void window_release_keys(WindowDisplayImpl *implementation)
{
    if (implementation == NULL || implementation->keyboard_input == NULL) {
        return;
    }
    for (uint16_t usage = 1; usage < 256U; ++usage) {
        if (implementation->pressed[usage] == 0) {
            continue;
        }
        implementation->pressed[usage] = 0;
        implementation->modifiers &=
            (uint8_t)~modifier_for_usage(usage);
        uint64_t event = vm_keyboard_event_make(
            usage,
            0,
            0,
            implementation->key_extended[usage] != 0,
            implementation->modifiers);
        (void)keyboard_input_emit(implementation->keyboard_input, event);
    }
}

static LRESULT CALLBACK window_procedure(HWND window,
                                         UINT message,
                                         WPARAM word_parameter,
                                         LPARAM long_parameter)
{
    WindowDisplay *display = (WindowDisplay *)GetWindowLongPtrW(
        window,
        GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *creation = (CREATESTRUCTW *)long_parameter;
        display = creation->lpCreateParams;
        (void)SetWindowLongPtrW(window,
                                GWLP_USERDATA,
                                (LONG_PTR)display);
    }

    WindowDisplayImpl *implementation =
        display != NULL ? display->implementation : NULL;
    switch (message) {
        case WINDOW_DISPLAY_FRAME_MESSAGE:
            if (implementation != NULL) {
                DisplayFrame latest;
                if (display_frame_queue_take_latest(
                        &implementation->queue,
                        &latest)) {
                    display_frame_release(&implementation->current);
                    implementation->current = latest;
                    (void)InvalidateRect(window, NULL, FALSE);
                }
            }
            return 0;
        case WM_PAINT:
            if (implementation != NULL) {
                window_paint(implementation, window);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            window_key_event(implementation,
                             word_parameter,
                             long_parameter,
                             1);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            window_key_event(implementation,
                             word_parameter,
                             long_parameter,
                             0);
            return 0;
        case WM_KILLFOCUS:
            window_release_keys(implementation);
            return 0;
        case WM_CLOSE:
            (void)DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (implementation != NULL) {
                window_release_keys(implementation);
                state_lock(implementation);
                implementation->window = NULL;
                implementation->closed = 1;
                state_unlock(implementation);
            }
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            (void)SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return DefWindowProcW(window,
                          message,
                          word_parameter,
                          long_parameter);
}

static int window_thread(void *context)
{
    WindowDisplay *display = context;
    WindowDisplayImpl *implementation = display->implementation;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = window_procedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)),
        .lpszClassName = WINDOW_DISPLAY_CLASS_NAME
    };
    if (RegisterClassW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        state_lock(implementation);
        implementation->initialization_failed = 1;
        implementation->closed = 1;
        state_unlock(implementation);
        (void)SetEvent(implementation->ready_event);
        return 0;
    }

    HWND window = CreateWindowExW(
        0,
        WINDOW_DISPLAY_CLASS_NAME,
        implementation->title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        800,
        600,
        NULL,
        NULL,
        instance,
        display);
    if (window == NULL) {
        state_lock(implementation);
        implementation->initialization_failed = 1;
        implementation->closed = 1;
        state_unlock(implementation);
        (void)SetEvent(implementation->ready_event);
        return 0;
    }

    state_lock(implementation);
    implementation->window = window;
    state_unlock(implementation);
    ShowWindow(window, SW_SHOW);
    (void)UpdateWindow(window);
    (void)SetEvent(implementation->ready_event);

    MSG message;
    int result;
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        (void)TranslateMessage(&message);
        (void)DispatchMessageW(&message);
    }
    state_lock(implementation);
    implementation->window = NULL;
    implementation->closed = 1;
    state_unlock(implementation);
    return result >= 0;
}

int window_display_supported(void)
{
    return 1;
}

int window_display_init(WindowDisplay *display,
                        const char *title,
                        KeyboardInput *keyboard_input)
{
    if (display == NULL) {
        return 0;
    }
    *display = (WindowDisplay){0};

    WindowDisplayImpl *implementation = calloc(1, sizeof(*implementation));
    if (implementation == NULL) {
        return 0;
    }
    InitializeCriticalSection(&implementation->state_lock);
    implementation->keyboard_input = keyboard_input;
    if (!display_frame_queue_init(&implementation->queue)) {
        DeleteCriticalSection(&implementation->state_lock);
        free(implementation);
        return 0;
    }
    implementation->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (implementation->ready_event == NULL) {
        display_frame_queue_destroy(&implementation->queue);
        DeleteCriticalSection(&implementation->state_lock);
        free(implementation);
        return 0;
    }

    const char *actual_title = title != NULL ? title : "VM Display";
    if (MultiByteToWideChar(CP_UTF8,
                            0,
                            actual_title,
                            -1,
                            implementation->title,
                            (int)(sizeof(implementation->title) /
                                  sizeof(implementation->title[0]))) == 0) {
        (void)CloseHandle(implementation->ready_event);
        display_frame_queue_destroy(&implementation->queue);
        DeleteCriticalSection(&implementation->state_lock);
        free(implementation);
        return 0;
    }

    display->implementation = implementation;
    display->host = (VmDisplayHost){
        .version = VM_DISPLAY_HOST_VERSION,
        .struct_size = sizeof(VmDisplayHost),
        .context = display,
        .resize = window_resize,
        .present = window_present
    };
    if (!host_thread_create(&implementation->thread,
                            window_thread,
                            display)) {
        window_display_destroy(display);
        return 0;
    }
    implementation->thread_started = 1;
    if (WaitForSingleObject(implementation->ready_event, INFINITE) !=
        WAIT_OBJECT_0) {
        window_display_destroy(display);
        return 0;
    }

    state_lock(implementation);
    int failed = implementation->initialization_failed;
    state_unlock(implementation);
    if (failed) {
        window_display_destroy(display);
        return 0;
    }
    return 1;
}

int window_display_wait_until_closed(WindowDisplay *display)
{
    if (display == NULL || display->implementation == NULL) {
        return 0;
    }
    WindowDisplayImpl *implementation = display->implementation;
    if (!implementation->thread_started) {
        return 1;
    }
    int result;
    if (!host_thread_join(&implementation->thread, &result)) {
        return 0;
    }
    implementation->thread_started = 0;
    return result;
}

void window_display_destroy(WindowDisplay *display)
{
    if (display == NULL || display->implementation == NULL) {
        return;
    }
    WindowDisplayImpl *implementation = display->implementation;
    if (implementation->thread_started) {
        state_lock(implementation);
        HWND window = implementation->window;
        state_unlock(implementation);
        if (window != NULL) {
            (void)PostMessageW(window, WM_CLOSE, 0, 0);
        }
        (void)host_thread_join(&implementation->thread, NULL);
        implementation->thread_started = 0;
    }

    display_frame_release(&implementation->current);
    display_frame_queue_destroy(&implementation->queue);
    if (implementation->ready_event != NULL) {
        (void)CloseHandle(implementation->ready_event);
    }
    DeleteCriticalSection(&implementation->state_lock);
    free(implementation);
    *display = (WindowDisplay){0};
}

VmDisplayHost *window_display_host(WindowDisplay *display)
{
    return display != NULL && display->implementation != NULL
               ? &display->host
               : NULL;
}

int window_display_frame_info(WindowDisplay *display,
                              size_t *frame_size,
                              uint32_t *width,
                              uint32_t *height,
                              uint32_t *stride,
                              uint32_t *format,
                              uint64_t *frame_number,
                              uint64_t *dropped_frames)
{
    if (display == NULL || display->implementation == NULL) {
        return 0;
    }
    WindowDisplayImpl *implementation = display->implementation;
    state_lock(implementation);
    if (frame_size != NULL) {
        *frame_size = implementation->submitted_size;
    }
    if (width != NULL) {
        *width = implementation->submitted_width;
    }
    if (height != NULL) {
        *height = implementation->submitted_height;
    }
    if (stride != NULL) {
        *stride = implementation->submitted_stride;
    }
    if (format != NULL) {
        *format = implementation->submitted_format;
    }
    if (frame_number != NULL) {
        *frame_number = implementation->submitted_frame;
    }
    state_unlock(implementation);
    if (dropped_frames != NULL) {
        *dropped_frames = display_frame_queue_dropped(
            &implementation->queue);
    }
    return 1;
}

#else

struct WindowDisplayImpl {
    int unused;
};

int window_display_supported(void)
{
    return 0;
}

int window_display_init(WindowDisplay *display,
                        const char *title,
                        KeyboardInput *keyboard_input)
{
    (void)display;
    (void)title;
    (void)keyboard_input;
    return 0;
}

void window_display_destroy(WindowDisplay *display)
{
    (void)display;
}

VmDisplayHost *window_display_host(WindowDisplay *display)
{
    (void)display;
    return NULL;
}

int window_display_wait_until_closed(WindowDisplay *display)
{
    (void)display;
    return 0;
}

int window_display_frame_info(WindowDisplay *display,
                              size_t *frame_size,
                              uint32_t *width,
                              uint32_t *height,
                              uint32_t *stride,
                              uint32_t *format,
                              uint64_t *frame_number,
                              uint64_t *dropped_frames)
{
    (void)display;
    (void)frame_size;
    (void)width;
    (void)height;
    (void)stride;
    (void)format;
    (void)frame_number;
    (void)dropped_frames;
    return 0;
}

#endif
