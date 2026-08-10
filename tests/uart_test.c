#include "bus.h"
#include "cpu.h"
#include "interrupt.h"
#include "ram.h"
#include "uart.h"

#include <assert.h>
#include <stdint.h>

#define UART_TEST_BASE UINT64_C(0xFFFFFFFFFFFD0000)

typedef struct {
    uint8_t bytes[UART_FIFO_CAPACITY + 8];
    size_t count;
} UartCapture;

static void capture_tx(void *context, uint8_t value)
{
    UartCapture *capture = context;
    assert(capture != NULL);
    assert(capture->count < sizeof(capture->bytes));
    capture->bytes[capture->count++] = value;
}

static void write_u64_le(uint8_t *destination, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static void test_cpu_uart_tx(void)
{
    uint8_t memory[128] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    UartDevice uart;
    UartCapture capture = {0};
    CPU cpu;

    assert(bus_init(&bus, &ram));
    assert(uart_device_init(&uart, UART_INTERRUPT_LINE));
    uart_device_set_tx_callback(&uart, capture_tx, &capture);
    assert(bus_map_device(&bus,
                          UART_TEST_BASE,
                          UART_MMIO_SIZE,
                          uart_device_as_bus_device(&uart)));

    size_t cursor = 1;
    memory[cursor++] = OP_MOVI64;
    memory[cursor++] = 0;
    write_u64_le(&memory[cursor],
                 UART_TEST_BASE + UART_CONTROL_OFFSET);
    cursor += 8;
    memory[cursor++] = OP_MOVI64;
    memory[cursor++] = 1;
    write_u64_le(&memory[cursor], UART_CONTROL_ENABLE);
    cursor += 8;
    memory[cursor++] = OP_STORE64;
    memory[cursor++] = 0;
    memory[cursor++] = 1;
    memory[cursor++] = OP_MOVI64;
    memory[cursor++] = 0;
    write_u64_le(&memory[cursor], UART_TEST_BASE + UART_TXDATA_OFFSET);
    cursor += 8;
    memory[cursor++] = OP_MOVI64;
    memory[cursor++] = 1;
    write_u64_le(&memory[cursor], (uint8_t)'Z');
    cursor += 8;
    memory[cursor++] = OP_STORE8;
    memory[cursor++] = 0;
    memory[cursor++] = 1;
    memory[cursor++] = OP_HALT;

    assert(cpu_init(&cpu, &ram));
    cpu.pc = 1;
    while (!cpu.halted) {
        assert(cpu_step(&cpu, &bus));
    }
    assert(uart_device_flush_tx(&uart) == 1);
    assert(capture.count == 1);
    assert(capture.bytes[0] == (uint8_t)'Z');
}

int test_uart(void)
{
    test_cpu_uart_tx();

    uint8_t memory[64] = {0};
    RAM ram = {
        .data = memory,
        .size = sizeof(memory)
    };
    Bus bus;
    assert(bus_init(&bus, &ram));

    UartDevice uart;
    assert(!uart_device_init(&uart, INTERRUPT_LINE_COUNT));
    assert(uart_device_init(&uart, UART_INTERRUPT_LINE));

    UartCapture capture = {0};
    uart_device_set_tx_callback(&uart, capture_tx, &capture);
    assert(bus_map_device(&bus,
                          UART_TEST_BASE,
                          UART_MMIO_SIZE,
                          uart_device_as_bus_device(&uart)));

    InterruptController controller;
    interrupt_controller_init(&controller);
    InterruptController *targets[1] = { &controller };
    InterruptRouter router;
    assert(interrupt_router_init(&router, targets, 1));

    uint64_t value;
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert(value == UART_STATUS_TX_EMPTY);
    assert(!uart_device_receive_byte(&uart, (uint8_t)'x'));

    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_CONTROL_OFFSET,
                     8,
                     UART_CONTROL_ENABLE |
                         UART_CONTROL_RX_IRQ_ENABLE |
                         UART_CONTROL_TX_IRQ_ENABLE));
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_BAUD_OFFSET,
                    8,
                    &value));
    assert(value == UART_DEFAULT_BAUD);
    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_BAUD_OFFSET,
                     8,
                     9600));
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_BAUD_OFFSET,
                    8,
                    &value));
    assert(value == 9600);

    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_TXDATA_OFFSET,
                     1,
                     (uint8_t)'A'));
    assert(capture.count == 0);
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_STATUS_TX_READY) != 0);
    assert((value & UART_STATUS_TX_EMPTY) == 0);

    bus_tick(&bus, 1, &router);
    assert(capture.count == 1);
    assert(capture.bytes[0] == (uint8_t)'A');
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_IRQ_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_IRQ_TX_PENDING) != 0);

    unsigned int line;
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == UART_INTERRUPT_LINE);
    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_IRQ_STATUS_OFFSET,
                     8,
                     UART_IRQ_TX_PENDING));

    const uint8_t input[] = { (uint8_t)'B', (uint8_t)'C' };
    assert(uart_device_receive(&uart, input, sizeof(input)) ==
           sizeof(input));
    bus_tick(&bus, 1, &router);
    assert(interrupt_controller_take_next(&controller, &line));
    assert(line == UART_INTERRUPT_LINE);

    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_RXDATA_OFFSET,
                    1,
                    &value));
    assert(value == (uint8_t)'B');
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_RXDATA_OFFSET,
                    1,
                    &value));
    assert(value == (uint8_t)'C');
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_RXDATA_OFFSET,
                    1,
                    &value));
    assert(value == 0);
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_STATUS_RX_READY) == 0);
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_IRQ_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_IRQ_RX_PENDING) == 0);

    for (size_t i = 0; i < UART_FIFO_CAPACITY; ++i) {
        assert(uart_device_receive_byte(&uart, (uint8_t)i));
    }
    assert(!uart_device_receive_byte(&uart, 0));
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_STATUS_RX_OVERRUN) != 0);
    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_STATUS_OFFSET,
                     8,
                     UART_STATUS_RX_OVERRUN));
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_STATUS_RX_OVERRUN) == 0);
    for (size_t i = 0; i < UART_FIFO_CAPACITY; ++i) {
        assert(bus_read(&bus,
                        UART_TEST_BASE + UART_RXDATA_OFFSET,
                        1,
                        &value));
        assert(value == (uint8_t)i);
    }

    uart_device_set_tx_callback(&uart, NULL, NULL);
    for (size_t i = 0; i < UART_FIFO_CAPACITY; ++i) {
        assert(bus_write(&bus,
                         UART_TEST_BASE + UART_TXDATA_OFFSET,
                         1,
                         (uint8_t)i));
    }
    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_TXDATA_OFFSET,
                     1,
                     0));
    assert(bus_read(&bus,
                    UART_TEST_BASE + UART_STATUS_OFFSET,
                    8,
                    &value));
    assert((value & UART_STATUS_TX_READY) == 0);
    assert((value & UART_STATUS_TX_OVERRUN) != 0);
    assert(uart_device_flush_tx(&uart) == 0);

    uart_device_set_tx_callback(&uart, capture_tx, &capture);
    assert(uart_device_flush_tx(&uart) == UART_FIFO_CAPACITY);
    assert(capture.count == UART_FIFO_CAPACITY + 1);
    assert(bus_write(&bus,
                     UART_TEST_BASE + UART_STATUS_OFFSET,
                     8,
                     UART_STATUS_TX_OVERRUN));

    /* DATA는 1바이트, 나머지 레지스터는 8바이트 접근만 허용한다. */
    assert(!bus_write(&bus,
                      UART_TEST_BASE + UART_TXDATA_OFFSET,
                      8,
                      0));
    assert(!bus_read(&bus,
                     UART_TEST_BASE + UART_RXDATA_OFFSET,
                     8,
                     &value));
    assert(!bus_read(&bus,
                     UART_TEST_BASE + UART_STATUS_OFFSET,
                     4,
                     &value));
    assert(!bus_read(&bus,
                     UART_TEST_BASE + 6,
                     1,
                     &value));
    return 0;
}
