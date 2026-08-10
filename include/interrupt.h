#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define INTERRUPT_LINE_COUNT 64U
#define INTERRUPT_EXTERNAL_LINE_COUNT 48U
#define INTERRUPT_IPI_LINE_BASE INTERRUPT_EXTERNAL_LINE_COUNT
#define INTERRUPT_IPI_LINE_COUNT \
    (INTERRUPT_LINE_COUNT - INTERRUPT_IPI_LINE_BASE)
#define INTERRUPT_EXTERNAL_LINE_MASK \
    ((UINT64_C(1) << INTERRUPT_EXTERNAL_LINE_COUNT) - 1)
#define TIMER_INTERRUPT_LINE 0U
#define UART_INTERRUPT_LINE 1U

typedef struct {
    atomic_uint_fast64_t pending;
} InterruptController;

typedef struct {
    InterruptController **targets;
    size_t target_count;
    size_t routes[INTERRUPT_LINE_COUNT];
    uint64_t enabled;
    uint64_t pending;
    uint64_t active;
    int programmable;
    atomic_flag lock;
} InterruptRouter;

void interrupt_controller_init(InterruptController *controller);
int interrupt_controller_raise(InterruptController *controller,
                               unsigned int line);
int interrupt_controller_clear(InterruptController *controller,
                               unsigned int line);
int interrupt_controller_take_next(InterruptController *controller,
                                   unsigned int *line);
int interrupt_router_init(InterruptRouter *router,
                          InterruptController **targets,
                          size_t target_count);
int interrupt_router_set_route(InterruptRouter *router,
                               unsigned int line,
                               size_t target_index);
int interrupt_router_raise(InterruptRouter *router,
                           unsigned int line);
int interrupt_router_clear(InterruptRouter *router,
                           unsigned int line);
void interrupt_router_programmable_reset(InterruptRouter *router);
int interrupt_router_set_enabled(InterruptRouter *router, uint64_t mask);
int interrupt_router_enable_lines(InterruptRouter *router, uint64_t mask);
int interrupt_router_disable_lines(InterruptRouter *router, uint64_t mask);
uint64_t interrupt_router_enabled(InterruptRouter *router);
uint64_t interrupt_router_pending(InterruptRouter *router);
uint64_t interrupt_router_active(InterruptRouter *router);
int interrupt_router_get_route(InterruptRouter *router,
                               unsigned int line,
                               size_t *target_index);
int interrupt_router_eoi(InterruptRouter *router, unsigned int line);

#endif
