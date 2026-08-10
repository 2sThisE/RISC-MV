#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "isa.h"
#include "font8x8_basic.h"

#define DISPLAY_BAR UINT64_C(0xFFFFFFFFF0000000)
#define DISPLAY_WIDTH_OFFSET UINT64_C(0x08)
#define DISPLAY_HEIGHT_OFFSET UINT64_C(0x10)
#define DISPLAY_STRIDE_OFFSET UINT64_C(0x18)
#define DISPLAY_FORMAT_OFFSET UINT64_C(0x20)
#define DISPLAY_FRAMEBUFFER_OFFSET UINT64_C(0x28)
#define DISPLAY_BUFFER_SIZE_OFFSET UINT64_C(0x30)
#define DISPLAY_CONTROL_OFFSET UINT64_C(0x00)
#define DISPLAY_COMMAND_OFFSET UINT64_C(0x38)

#define DEMO_WIDTH UINT64_C(320)
#define DEMO_HEIGHT UINT64_C(200)
#define DEMO_STRIDE (DEMO_WIDTH * UINT64_C(4))
#define DEMO_SIZE (DEMO_STRIDE * DEMO_HEIGHT)
#define PROGRAM_LOAD_ADDRESS UINT64_C(1)

typedef struct {
    uint8_t bytes[512];
    size_t size;
} CodeBuffer;

static int emit8(CodeBuffer *code, uint8_t value)
{
    if (code->size >= sizeof(code->bytes)) {
        return 0;
    }
    code->bytes[code->size++] = value;
    return 1;
}

static int emit64(CodeBuffer *code, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        if (!emit8(code, (uint8_t)(value >> (i * 8)))) {
            return 0;
        }
    }
    return 1;
}

static int emit32(CodeBuffer *code, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        if (!emit8(code, (uint8_t)(value >> (i * 8)))) {
            return 0;
        }
    }
    return 1;
}

static int emit_movi64(CodeBuffer *code,
                       uint8_t destination,
                       uint64_t value,
                       size_t *immediate_offset)
{
    if (!emit8(code, OP_MOVI64) || !emit8(code, destination)) {
        return 0;
    }
    if (immediate_offset != NULL) {
        *immediate_offset = code->size;
    }
    return emit64(code, value);
}

static int emit_movi32u(CodeBuffer *code,
                        uint8_t destination,
                        uint32_t value,
                        size_t *immediate_offset)
{
    if (!emit8(code, OP_MOVI32U) || !emit8(code, destination)) {
        return 0;
    }
    if (immediate_offset != NULL) {
        *immediate_offset = code->size;
    }
    return emit32(code, value);
}

static int emit_mmio_write(CodeBuffer *code,
                           uint32_t displacement,
                           uint32_t value,
                           size_t *value_offset)
{
    return emit_movi32u(code, 1, value, value_offset) &&
           emit8(code, OP_STORE64O) &&
           emit8(code, 0) &&
           emit8(code, 1) &&
           emit32(code, displacement);
}

static void patch32(CodeBuffer *code, size_t offset, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        code->bytes[offset + i] = (uint8_t)(value >> (i * 8));
    }
}

static void put_pixel(uint8_t *pixels,
                      uint32_t x,
                      uint32_t y,
                      uint32_t color)
{
    if (x >= DEMO_WIDTH || y >= DEMO_HEIGHT) {
        return;
    }
    size_t offset = ((size_t)y * DEMO_WIDTH + x) * 4;
    pixels[offset] = (uint8_t)color;
    pixels[offset + 1] = (uint8_t)(color >> 8);
    pixels[offset + 2] = (uint8_t)(color >> 16);
    pixels[offset + 3] = 0;
}

static void fill_rectangle(uint8_t *pixels,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           uint32_t color)
{
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            put_pixel(pixels, x + column, y + row, color);
        }
    }
}

static void draw_char(uint8_t *pixels,
                      uint32_t screen_x,
                      uint32_t screen_y,
                      uint8_t character,
                      uint32_t scale,
                      uint32_t color)
{
    if (character >= 128) {
        character = (uint8_t)'?';
    }
    for (uint32_t glyph_y = 0; glyph_y < 8; ++glyph_y) {
        uint8_t row = FONT8X8_BASIC[character][glyph_y];
        for (uint32_t glyph_x = 0; glyph_x < 8; ++glyph_x) {
            if ((row & (uint8_t)(UINT8_C(1) << glyph_x)) == 0) {
                continue;
            }
            for (uint32_t scale_y = 0; scale_y < scale; ++scale_y) {
                for (uint32_t scale_x = 0; scale_x < scale; ++scale_x) {
                    put_pixel(pixels,
                              screen_x + glyph_x * scale + scale_x,
                              screen_y + glyph_y * scale + scale_y,
                              color);
                }
            }
        }
    }
}

static void draw_string(uint8_t *pixels,
                        uint32_t x,
                        uint32_t y,
                        const char *text,
                        uint32_t scale,
                        uint32_t color)
{
    while (text != NULL && *text != '\0') {
        draw_char(pixels, x, y, (uint8_t)*text, scale, color);
        x += 9 * scale;
        ++text;
    }
}

static int write_pixels(FILE *file)
{
    uint8_t *pixels = malloc((size_t)DEMO_SIZE);
    if (pixels == NULL) {
        return 0;
    }

    for (uint64_t y = 0; y < DEMO_HEIGHT; ++y) {
        for (uint64_t x = 0; x < DEMO_WIDTH; ++x) {
            uint32_t blue = (uint32_t)(x * UINT64_C(255) /
                                       (DEMO_WIDTH - 1));
            uint32_t green = (uint32_t)(y * UINT64_C(255) /
                                        (DEMO_HEIGHT - 1));
            uint32_t red = (((x / 32) ^ (y / 25)) & 1) != 0 ? 240U : 32U;
            put_pixel(pixels,
                      (uint32_t)x,
                      (uint32_t)y,
                      blue | (green << 8) | (red << 16));
        }
    }

    fill_rectangle(pixels, 8, 74, 304, 52, UINT32_C(0x00101018));
    draw_string(pixels, 18, 86, "HELLO VM", 4, UINT32_C(0x00005070));
    draw_string(pixels, 16, 84, "HELLO VM", 4, UINT32_C(0x00FFFFFF));

    int succeeded = fwrite(pixels,
                           1,
                           (size_t)DEMO_SIZE,
                           file) == (size_t)DEMO_SIZE;
    free(pixels);
    return succeeded;
}

int main(int argc, char **argv)
{
    const char *output_path = argc > 1 ? argv[1] : "display_demo.bin";
    CodeBuffer code = {0};
    size_t framebuffer_immediate;

    if (!emit_movi64(&code, 0, DISPLAY_BAR, NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_WIDTH_OFFSET,
                         DEMO_WIDTH,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_HEIGHT_OFFSET,
                         DEMO_HEIGHT,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_STRIDE_OFFSET,
                         DEMO_STRIDE,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_FORMAT_OFFSET,
                         1,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_FRAMEBUFFER_OFFSET,
                         0,
                         &framebuffer_immediate) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_BUFFER_SIZE_OFFSET,
                         DEMO_SIZE,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_CONTROL_OFFSET,
                         1,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_COMMAND_OFFSET,
                         1,
                         NULL) ||
        !emit8(&code, OP_HALT)) {
        fputs("failed to build demo code\n", stderr);
        return EXIT_FAILURE;
    }

    uint64_t framebuffer_address = PROGRAM_LOAD_ADDRESS + code.size;
    if (framebuffer_address > UINT32_MAX) {
        fputs("display demo framebuffer address is too large\n", stderr);
        return EXIT_FAILURE;
    }
    patch32(&code,
            framebuffer_immediate,
            (uint32_t)framebuffer_address);
    FILE *file = fopen(output_path, "wb");
    if (file == NULL) {
        perror("failed to open output");
        return EXIT_FAILURE;
    }
    int succeeded = fwrite(code.bytes, 1, code.size, file) == code.size &&
                    write_pixels(file);
    if (fclose(file) != 0) {
        succeeded = 0;
    }
    if (!succeeded) {
        fputs("failed to write display demo\n", stderr);
        return EXIT_FAILURE;
    }
    printf("Built %s (%zu bytes of code, %llu bytes of pixels)\n",
           output_path,
           code.size,
           (unsigned long long)DEMO_SIZE);
    return EXIT_SUCCESS;
}
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "isa.h"
#include "device_abi.h"
#include "font8x8_basic.h"

#define VIO_HUB_BASE UINT64_C(0xFFFFFFFFFFFC0000)
#define VIO_SLOT_COUNT_OFFSET UINT32_C(0x10)
#define VIO_SLOT_BASE_OFFSET UINT32_C(0x100)
#define VIO_SLOT_STRIDE UINT32_C(0xC0)
#define VIO_SLOT_STATUS_OFFSET UINT32_C(0x00)
#define VIO_SLOT_CLASS_OFFSET UINT32_C(0x08)
#define VIO_SLOT_BAR0_BASE_OFFSET UINT32_C(0x28)
#define VIO_SLOT_PRESENT UINT32_C(0x01)

#define BRCC_ALWAYS UINT8_C(0)
#define BRCC_EQ UINT8_C(1)
#define BRCC_GE UINT8_C(6)
#define DISPLAY_WIDTH_OFFSET UINT64_C(0x08)
#define DISPLAY_HEIGHT_OFFSET UINT64_C(0x10)
#define DISPLAY_STRIDE_OFFSET UINT64_C(0x18)
#define DISPLAY_FORMAT_OFFSET UINT64_C(0x20)
#define DISPLAY_FRAMEBUFFER_OFFSET UINT64_C(0x28)
#define DISPLAY_BUFFER_SIZE_OFFSET UINT64_C(0x30)
#define DISPLAY_CONTROL_OFFSET UINT64_C(0x00)
#define DISPLAY_COMMAND_OFFSET UINT64_C(0x38)

#define DEMO_WIDTH UINT64_C(320)
#define DEMO_HEIGHT UINT64_C(200)
#define DEMO_STRIDE (DEMO_WIDTH * UINT64_C(4))
#define DEMO_SIZE (DEMO_STRIDE * DEMO_HEIGHT)
#define PROGRAM_LOAD_ADDRESS UINT64_C(1)

typedef struct {
    uint8_t bytes[512];
    size_t size;
} CodeBuffer;

static int emit8(CodeBuffer *code, uint8_t value)
{
    if (code->size >= sizeof(code->bytes)) {
        return 0;
    }
    code->bytes[code->size++] = value;
    return 1;
}

static int emit64(CodeBuffer *code, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        if (!emit8(code, (uint8_t)(value >> (i * 8)))) {
            return 0;
        }
    }
    return 1;
}

static int emit32(CodeBuffer *code, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        if (!emit8(code, (uint8_t)(value >> (i * 8)))) {
            return 0;
        }
    }
    return 1;
}

static int emit_movi64(CodeBuffer *code,
                       uint8_t destination,
                       uint64_t value,
                       size_t *immediate_offset)
{
    if (!emit8(code, OP_MOVI64) || !emit8(code, destination)) {
        return 0;
    }
    if (immediate_offset != NULL) {
        *immediate_offset = code->size;
    }
    return emit64(code, value);
}

static int emit_movi32u(CodeBuffer *code,
                        uint8_t destination,
                        uint32_t value,
                        size_t *immediate_offset)
{
    if (!emit8(code, OP_MOVI32U) || !emit8(code, destination)) {
        return 0;
    }
    if (immediate_offset != NULL) {
        *immediate_offset = code->size;
    }
    return emit32(code, value);
}

static int emit_mmio_write(CodeBuffer *code,
                           uint32_t displacement,
                           uint32_t value,
                           size_t *value_offset)
{
    return emit_movi32u(code, 1, value, value_offset) &&
           emit8(code, OP_STORE64O) &&
           emit8(code, 0) &&
           emit8(code, 1) &&
           emit32(code, displacement);
}

static void patch32(CodeBuffer *code, size_t offset, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        code->bytes[offset + i] = (uint8_t)(value >> (i * 8));
    }
}


static int emit_load64o(CodeBuffer *code,
                        uint8_t destination,
                        uint8_t base,
                        uint32_t displacement)
{
    return emit8(code, OP_LOAD64O) &&
           emit8(code, destination) &&
           emit8(code, base) &&
           emit32(code, displacement);
}

static int emit_addi32(CodeBuffer *code, uint8_t destination, int32_t value)
{
    return emit8(code, OP_ADDI32) &&
           emit8(code, destination) &&
           emit32(code, (uint32_t)value);
}

static int emit_cmpi32(CodeBuffer *code, uint8_t lhs, int32_t value)
{
    return emit8(code, OP_CMPI32) &&
           emit8(code, lhs) &&
           emit32(code, (uint32_t)value);
}

static int emit_testi32(CodeBuffer *code, uint8_t lhs, uint32_t value)
{
    return emit8(code, OP_TESTI32) &&
           emit8(code, lhs) &&
           emit32(code, value);
}

static int emit_cmp(CodeBuffer *code, uint8_t lhs, uint8_t rhs)
{
    return emit8(code, OP_CMP) && emit8(code, lhs) && emit8(code, rhs);
}

static int emit_brcc(CodeBuffer *code, uint8_t condition, size_t *disp_offset)
{
    if (!emit8(code, OP_BRCC) || !emit8(code, condition)) {
        return 0;
    }
    if (disp_offset != NULL) {
        *disp_offset = code->size;
    }
    return emit32(code, 0);
}

static int patch_rel32(CodeBuffer *code, size_t displacement_offset, size_t target_offset)
{
    int64_t next_pc = (int64_t)(PROGRAM_LOAD_ADDRESS + displacement_offset + 4);
    int64_t target_pc = (int64_t)(PROGRAM_LOAD_ADDRESS + target_offset);
    int64_t displacement = target_pc - next_pc;
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        return 0;
    }
    patch32(code, displacement_offset, (uint32_t)(int32_t)displacement);
    return 1;
}

static void put_pixel(uint8_t *pixels,
                      uint32_t x,
                      uint32_t y,
                      uint32_t color)
{
    if (x >= DEMO_WIDTH || y >= DEMO_HEIGHT) {
        return;
    }
    size_t offset = ((size_t)y * DEMO_WIDTH + x) * 4;
    pixels[offset] = (uint8_t)color;
    pixels[offset + 1] = (uint8_t)(color >> 8);
    pixels[offset + 2] = (uint8_t)(color >> 16);
    pixels[offset + 3] = 0;
}

static void fill_rectangle(uint8_t *pixels,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height,
                           uint32_t color)
{
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            put_pixel(pixels, x + column, y + row, color);
        }
    }
}

static void draw_char(uint8_t *pixels,
                      uint32_t screen_x,
                      uint32_t screen_y,
                      uint8_t character,
                      uint32_t scale,
                      uint32_t color)
{
    if (character >= 128) {
        character = (uint8_t)'?';
    }
    for (uint32_t glyph_y = 0; glyph_y < 8; ++glyph_y) {
        uint8_t row = FONT8X8_BASIC[character][glyph_y];
        for (uint32_t glyph_x = 0; glyph_x < 8; ++glyph_x) {
            if ((row & (uint8_t)(UINT8_C(1) << glyph_x)) == 0) {
                continue;
            }
            for (uint32_t scale_y = 0; scale_y < scale; ++scale_y) {
                for (uint32_t scale_x = 0; scale_x < scale; ++scale_x) {
                    put_pixel(pixels,
                              screen_x + glyph_x * scale + scale_x,
                              screen_y + glyph_y * scale + scale_y,
                              color);
                }
            }
        }
    }
}

static void draw_string(uint8_t *pixels,
                        uint32_t x,
                        uint32_t y,
                        const char *text,
                        uint32_t scale,
                        uint32_t color)
{
    while (text != NULL && *text != '\0') {
        draw_char(pixels, x, y, (uint8_t)*text, scale, color);
        x += 9 * scale;
        ++text;
    }
}

static int write_pixels(FILE *file)
{
    uint8_t *pixels = malloc((size_t)DEMO_SIZE);
    if (pixels == NULL) {
        return 0;
    }

    for (uint64_t y = 0; y < DEMO_HEIGHT; ++y) {
        for (uint64_t x = 0; x < DEMO_WIDTH; ++x) {
            uint32_t blue = (uint32_t)(x * UINT64_C(255) /
                                       (DEMO_WIDTH - 1));
            uint32_t green = (uint32_t)(y * UINT64_C(255) /
                                        (DEMO_HEIGHT - 1));
            uint32_t red = (((x / 32) ^ (y / 25)) & 1) != 0 ? 240U : 32U;
            put_pixel(pixels,
                      (uint32_t)x,
                      (uint32_t)y,
                      blue | (green << 8) | (red << 16));
        }
    }

    fill_rectangle(pixels, 8, 74, 304, 52, UINT32_C(0x00101018));
    draw_string(pixels, 18, 86, "HELLO VM", 4, UINT32_C(0x00005070));
    draw_string(pixels, 16, 84, "HELLO VM", 4, UINT32_C(0x00FFFFFF));

    int succeeded = fwrite(pixels,
                           1,
                           (size_t)DEMO_SIZE,
                           file) == (size_t)DEMO_SIZE;
    free(pixels);
    return succeeded;
}

int main(int argc, char **argv)
{
    const char *output_path = argc > 1 ? argv[1] : "display_demo.bin";
    CodeBuffer code = {0};
    size_t framebuffer_immediate;
    size_t branch_not_found;
    size_t branch_next_if_absent;
    size_t branch_found;
    size_t branch_loop;
    size_t loop_offset;
    size_t next_slot_offset;
    size_t found_offset;
    size_t not_found_offset;

    /*
     * Guest-side VIO discovery.
     * R2 = VIO Hub base
     * R3 = slot count
     * R4 = current slot index
     * R5 = current slot configuration address
     * R6 = scratch
     * R0 = discovered display BAR0
     */
    if (!emit_movi64(&code, 2, VIO_HUB_BASE, NULL) ||
        !emit_load64o(&code, 3, 2, VIO_SLOT_COUNT_OFFSET) ||
        !emit_movi32u(&code, 4, 0, NULL) ||
        !emit_movi64(&code, 5, VIO_HUB_BASE + VIO_SLOT_BASE_OFFSET, NULL)) {
        fputs("failed to build VIO discovery prologue\n", stderr);
        return EXIT_FAILURE;
    }

    loop_offset = code.size;
    if (!emit_cmp(&code, 4, 3) ||
        !emit_brcc(&code, BRCC_GE, &branch_not_found) ||
        !emit_load64o(&code, 6, 5, VIO_SLOT_STATUS_OFFSET) ||
        !emit_testi32(&code, 6, VIO_SLOT_PRESENT) ||
        !emit_brcc(&code, BRCC_EQ, &branch_next_if_absent) ||
        !emit_load64o(&code, 6, 5, VIO_SLOT_CLASS_OFFSET) ||
        !emit_cmpi32(&code, 6, (int32_t)VM_DEVICE_CLASS_DISPLAY) ||
        !emit_brcc(&code, BRCC_EQ, &branch_found)) {
        fputs("failed to build VIO discovery loop\n", stderr);
        return EXIT_FAILURE;
    }

    next_slot_offset = code.size;
    if (!emit_addi32(&code, 5, (int32_t)VIO_SLOT_STRIDE) ||
        !emit_addi32(&code, 4, 1) ||
        !emit_brcc(&code, BRCC_ALWAYS, &branch_loop)) {
        fputs("failed to build VIO discovery iteration\n", stderr);
        return EXIT_FAILURE;
    }

    found_offset = code.size;
    if (!emit_load64o(&code, 0, 5, VIO_SLOT_BAR0_BASE_OFFSET) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_WIDTH_OFFSET,
                         DEMO_WIDTH,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_HEIGHT_OFFSET,
                         DEMO_HEIGHT,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_STRIDE_OFFSET,
                         DEMO_STRIDE,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_FORMAT_OFFSET,
                         1,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_FRAMEBUFFER_OFFSET,
                         0,
                         &framebuffer_immediate) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_BUFFER_SIZE_OFFSET,
                         DEMO_SIZE,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_CONTROL_OFFSET,
                         1,
                         NULL) ||
        !emit_mmio_write(&code,
                         (uint32_t)DISPLAY_COMMAND_OFFSET,
                         1,
                         NULL) ||
        !emit8(&code, OP_HALT)) {
        fputs("failed to build demo code\n", stderr);
        return EXIT_FAILURE;
    }

    not_found_offset = code.size;
    if (!emit8(&code, OP_HALT) ||
        !patch_rel32(&code, branch_not_found, not_found_offset) ||
        !patch_rel32(&code, branch_next_if_absent, next_slot_offset) ||
        !patch_rel32(&code, branch_found, found_offset) ||
        !patch_rel32(&code, branch_loop, loop_offset)) {
        fputs("failed to finalize VIO discovery branches\n", stderr);
        return EXIT_FAILURE;
    }

    uint64_t framebuffer_address = PROGRAM_LOAD_ADDRESS + code.size;
    if (framebuffer_address > UINT32_MAX) {
        fputs("display demo framebuffer address is too large\n", stderr);
        return EXIT_FAILURE;
    }
    patch32(&code,
            framebuffer_immediate,
            (uint32_t)framebuffer_address);
    FILE *file = fopen(output_path, "wb");
    if (file == NULL) {
        perror("failed to open output");
        return EXIT_FAILURE;
    }
    int succeeded = fwrite(code.bytes, 1, code.size, file) == code.size &&
                    write_pixels(file);
    if (fclose(file) != 0) {
        succeeded = 0;
    }
    if (!succeeded) {
        fputs("failed to write display demo\n", stderr);
        return EXIT_FAILURE;
    }
    printf("Built %s (%zu bytes of code, %llu bytes of pixels)\n",
           output_path,
           code.size,
           (unsigned long long)DEMO_SIZE);
    return EXIT_SUCCESS;
}