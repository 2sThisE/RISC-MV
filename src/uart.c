#include "uart.h"

#define UART_CONTROL_MASK (UART_CONTROL_ENABLE | \
                           UART_CONTROL_RX_IRQ_ENABLE | \
                           UART_CONTROL_TX_IRQ_ENABLE)
#define UART_ERROR_STATUS_MASK (UART_STATUS_RX_OVERRUN | \
                                UART_STATUS_TX_OVERRUN)
#define UART_IRQ_STATUS_MASK (UART_IRQ_RX_PENDING | \
                              UART_IRQ_TX_PENDING)

static void uart_lock(UartDevice *uart)
{
    while (atomic_flag_test_and_set_explicit(&uart->lock,
                                             memory_order_acquire)) {
    }
}

static void uart_unlock(UartDevice *uart)
{
    atomic_flag_clear_explicit(&uart->lock, memory_order_release);
}

static uint64_t uart_status_locked(const UartDevice *uart)
{
    uint64_t status = uart->error_status;
    if (uart->rx_count != 0) {
        status |= UART_STATUS_RX_READY;
    }
    if ((uart->control & UART_CONTROL_ENABLE) != 0 &&
        uart->tx_count < UART_FIFO_CAPACITY) {
        status |= UART_STATUS_TX_READY;
    }
    if (uart->tx_count == 0) {
        status |= UART_STATUS_TX_EMPTY;
    }
    return status;
}

static int uart_read(void *context,
                     uint64_t offset,
                     size_t width,
                     uint64_t *value)
{
    UartDevice *uart = context;
    if (uart == NULL || value == NULL) {
        return 0;
    }

    uart_lock(uart);
    int result = 1;
    if (offset == UART_RXDATA_OFFSET && width == 1) {
        if (uart->rx_count == 0) {
            *value = 0;
        } else {
            *value = uart->rx_fifo[uart->rx_head];
            uart->rx_head = (uart->rx_head + 1) % UART_FIFO_CAPACITY;
            --uart->rx_count;
            if (uart->rx_count == 0) {
                uart->irq_status &= ~UART_IRQ_RX_PENDING;
            }
        }
    } else if (width != sizeof(uint64_t)) {
        result = 0;
    } else {
        switch (offset) {
            case UART_STATUS_OFFSET:
                *value = uart_status_locked(uart);
                break;
            case UART_CONTROL_OFFSET:
                *value = uart->control;
                break;
            case UART_BAUD_OFFSET:
                *value = uart->baud;
                break;
            case UART_IRQ_STATUS_OFFSET:
                *value = uart->irq_status;
                break;
            default:
                result = 0;
                break;
        }
    }
    uart_unlock(uart);
    return result;
}

static int uart_write(void *context,
                      uint64_t offset,
                      size_t width,
                      uint64_t value)
{
    UartDevice *uart = context;
    if (uart == NULL) {
        return 0;
    }

    uart_lock(uart);
    int result = 1;
    if (offset == UART_TXDATA_OFFSET && width == 1) {
        if ((uart->control & UART_CONTROL_ENABLE) != 0) {
            if (uart->tx_count == UART_FIFO_CAPACITY) {
                uart->error_status |= UART_STATUS_TX_OVERRUN;
            } else {
                uart->tx_fifo[uart->tx_tail] = (uint8_t)value;
                uart->tx_tail = (uart->tx_tail + 1) % UART_FIFO_CAPACITY;
                ++uart->tx_count;
                uart->irq_status &= ~UART_IRQ_TX_PENDING;
            }
        }
    } else if (width != sizeof(uint64_t)) {
        result = 0;
    } else {
        switch (offset) {
            case UART_STATUS_OFFSET:
                uart->error_status &= ~(value & UART_ERROR_STATUS_MASK);
                break;
            case UART_CONTROL_OFFSET:
                uart->control = value & UART_CONTROL_MASK;
                break;
            case UART_BAUD_OFFSET:
                uart->baud = value;
                break;
            case UART_IRQ_STATUS_OFFSET:
                uart->irq_status &= ~(value & UART_IRQ_STATUS_MASK);
                break;
            default:
                result = 0;
                break;
        }
    }
    uart_unlock(uart);
    return result;
}

size_t uart_device_flush_tx(UartDevice *uart)
{
    if (uart == NULL) {
        return 0;
    }

    size_t delivered = 0;
    for (;;) {
        uart_lock(uart);
        UartTxCallback callback = uart->tx_callback;
        void *callback_context = uart->tx_context;
        if (callback == NULL || uart->tx_count == 0) {
            uart_unlock(uart);
            break;
        }

        uint8_t value = uart->tx_fifo[uart->tx_head];
        uart->tx_head = (uart->tx_head + 1) % UART_FIFO_CAPACITY;
        --uart->tx_count;
        if (uart->tx_count == 0) {
            uart->irq_status |= UART_IRQ_TX_PENDING;
        }
        uart_unlock(uart);

        callback(callback_context, value);
        ++delivered;
    }
    return delivered;
}

static void uart_tick(void *context,
                      uint64_t ticks,
                      InterruptRouter *interrupts)
{
    UartDevice *uart = context;
    if (uart == NULL || ticks == 0) {
        return;
    }

    uart_lock(uart);
    int enabled = (uart->control & UART_CONTROL_ENABLE) != 0;
    uart_unlock(uart);
    if (enabled) {
        (void)uart_device_flush_tx(uart);
    }

    uart_lock(uart);
    if (enabled && uart->rx_count != 0) {
        uart->irq_status |= UART_IRQ_RX_PENDING;
    }
    uint64_t enabled_pending = uart->irq_status &
        (((uart->control & UART_CONTROL_RX_IRQ_ENABLE) != 0
              ? UART_IRQ_RX_PENDING
              : 0) |
         ((uart->control & UART_CONTROL_TX_IRQ_ENABLE) != 0
              ? UART_IRQ_TX_PENDING
              : 0));
    unsigned int interrupt_line = uart->interrupt_line;
    uart_unlock(uart);

    if (enabled && enabled_pending != 0) {
        (void)interrupt_router_raise(interrupts, interrupt_line);
    }
}

int uart_device_init(UartDevice *uart, unsigned int interrupt_line)
{
    if (uart == NULL || interrupt_line >= INTERRUPT_LINE_COUNT) {
        return 0;
    }

    *uart = (UartDevice){0};
    uart->baud = UART_DEFAULT_BAUD;
    uart->interrupt_line = interrupt_line;
    atomic_flag_clear(&uart->lock);
    return 1;
}

static void uart_reset(void *context)
{
    UartDevice *uart = context;
    if (uart == NULL) {
        return;
    }

    uart_lock(uart);
    unsigned int interrupt_line = uart->interrupt_line;
    UartTxCallback tx_callback = uart->tx_callback;
    void *tx_context = uart->tx_context;
    uart->rx_head = 0;
    uart->rx_tail = 0;
    uart->rx_count = 0;
    uart->tx_head = 0;
    uart->tx_tail = 0;
    uart->tx_count = 0;
    uart->control = 0;
    uart->baud = UART_DEFAULT_BAUD;
    uart->error_status = 0;
    uart->irq_status = 0;
    uart->interrupt_line = interrupt_line;
    uart->tx_callback = tx_callback;
    uart->tx_context = tx_context;
    uart_unlock(uart);
}

void uart_device_set_tx_callback(UartDevice *uart,
                                 UartTxCallback callback,
                                 void *context)
{
    if (uart == NULL) {
        return;
    }

    uart_lock(uart);
    uart->tx_callback = callback;
    uart->tx_context = context;
    uart_unlock(uart);
}

int uart_device_receive_byte(UartDevice *uart, uint8_t value)
{
    if (uart == NULL) {
        return 0;
    }

    uart_lock(uart);
    if ((uart->control & UART_CONTROL_ENABLE) == 0) {
        uart_unlock(uart);
        return 0;
    }
    if (uart->rx_count == UART_FIFO_CAPACITY) {
        uart->error_status |= UART_STATUS_RX_OVERRUN;
        uart->irq_status |= UART_IRQ_RX_PENDING;
        uart_unlock(uart);
        return 0;
    }

    uart->rx_fifo[uart->rx_tail] = value;
    uart->rx_tail = (uart->rx_tail + 1) % UART_FIFO_CAPACITY;
    ++uart->rx_count;
    uart->irq_status |= UART_IRQ_RX_PENDING;
    uart_unlock(uart);
    return 1;
}

size_t uart_device_receive(UartDevice *uart,
                           const uint8_t *data,
                           size_t size)
{
    if (uart == NULL || (data == NULL && size != 0)) {
        return 0;
    }

    size_t accepted = 0;
    while (accepted < size &&
           uart_device_receive_byte(uart, data[accepted])) {
        ++accepted;
    }
    return accepted;
}

BusDevice uart_device_as_bus_device(UartDevice *uart)
{
    BusDevice device = {
        .context = uart,
        .read = uart_read,
        .write = uart_write,
        .tick = uart_tick,
        .reset = uart_reset
    };
    return device;
}
