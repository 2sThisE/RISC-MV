#ifndef UART_H
#define UART_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "bus.h"
#include "builtin_device_protocol.h"

typedef void (*UartTxCallback)(void *context, uint8_t value);

typedef struct {
    uint8_t rx_fifo[UART_FIFO_CAPACITY];
    uint8_t tx_fifo[UART_FIFO_CAPACITY];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
    size_t tx_head;
    size_t tx_tail;
    size_t tx_count;
    uint64_t control;
    uint64_t baud;
    uint64_t error_status;
    uint64_t irq_status;
    unsigned int interrupt_line;
    UartTxCallback tx_callback;
    void *tx_context;
    atomic_flag lock;
} UartDevice;

int uart_device_init(UartDevice *uart, unsigned int interrupt_line);
void uart_device_set_tx_callback(UartDevice *uart,
                                 UartTxCallback callback,
                                 void *context);
int uart_device_receive_byte(UartDevice *uart, uint8_t value);
size_t uart_device_receive(UartDevice *uart,
                           const uint8_t *data,
                           size_t size);
size_t uart_device_flush_tx(UartDevice *uart);
BusDevice uart_device_as_bus_device(UartDevice *uart);

#endif
