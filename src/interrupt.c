#include "interrupt.h"

#include <stddef.h>

static void router_lock(InterruptRouter *router)
{
    while (atomic_flag_test_and_set_explicit(&router->lock,
                                             memory_order_acquire)) {
    }
}

static void router_unlock(InterruptRouter *router)
{
    atomic_flag_clear_explicit(&router->lock, memory_order_release);
}

void interrupt_controller_init(InterruptController *controller)
{
    if (controller == NULL) {
        return;
    }

    atomic_init(&controller->pending, UINT64_C(0));
}

int interrupt_controller_raise(InterruptController *controller,
                               unsigned int line)
{
    if (controller == NULL || line >= INTERRUPT_LINE_COUNT) {
        return 0;
    }

    atomic_fetch_or_explicit(&controller->pending,
                             UINT64_C(1) << line,
                             memory_order_release);
    return 1;
}

int interrupt_controller_clear(InterruptController *controller,
                               unsigned int line)
{
    if (controller == NULL || line >= INTERRUPT_LINE_COUNT) {
        return 0;
    }

    atomic_fetch_and_explicit(&controller->pending,
                              ~(UINT64_C(1) << line),
                              memory_order_acq_rel);
    return 1;
}

static unsigned int lowest_set_bit(uint64_t value)
{
    unsigned int bit = 0;

    while ((value & UINT64_C(1)) == 0) {
        value >>= 1;
        ++bit;
    }

    return bit;
}

int interrupt_controller_take_next(InterruptController *controller,
                                   unsigned int *line)
{
    if (controller == NULL || line == NULL) {
        return 0;
    }

    uint_fast64_t current = atomic_load_explicit(&controller->pending,
                                                 memory_order_acquire);

    for (;;) {
        uint64_t available = (uint64_t)current;
        if (available == 0) {
            return 0;
        }

        uint64_t ipi_available =
            available & ~INTERRUPT_EXTERNAL_LINE_MASK;
        unsigned int selected = lowest_set_bit(
            ipi_available != 0 ? ipi_available : available);
        uint_fast64_t desired = current & ~(UINT64_C(1) << selected);

        if (atomic_compare_exchange_weak_explicit(&controller->pending,
                                                  &current,
                                                  desired,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            *line = selected;
            return 1;
        }
    }
}

static int target_has_active_locked(const InterruptRouter *router,
                                    size_t target)
{
    for (unsigned int line = 0;
         line < INTERRUPT_EXTERNAL_LINE_COUNT;
         ++line) {
        if ((router->active & (UINT64_C(1) << line)) != 0 &&
            router->routes[line] == target) {
            return 1;
        }
    }
    return 0;
}

static void dispatch_target_locked(InterruptRouter *router, size_t target)
{
    if (target >= router->target_count ||
        target_has_active_locked(router, target)) {
        return;
    }

    uint64_t deliverable = router->pending & router->enabled &
                           INTERRUPT_EXTERNAL_LINE_MASK;
    for (unsigned int line = 0;
         line < INTERRUPT_EXTERNAL_LINE_COUNT;
         ++line) {
        uint64_t bit = UINT64_C(1) << line;
        if ((deliverable & bit) == 0 ||
            router->routes[line] != target) {
            continue;
        }
        router->pending &= ~bit;
        router->active |= bit;
        (void)interrupt_controller_raise(router->targets[target], line);
        return;
    }
}

static void dispatch_all_locked(InterruptRouter *router)
{
    for (size_t target = 0; target < router->target_count; ++target) {
        dispatch_target_locked(router, target);
    }
}

int interrupt_router_init(InterruptRouter *router,
                          InterruptController **targets,
                          size_t target_count)
{
    if (router == NULL || targets == NULL || target_count == 0) {
        return 0;
    }

    for (size_t i = 0; i < target_count; ++i) {
        if (targets[i] == NULL) {
            return 0;
        }
    }

    router->targets = targets;
    router->target_count = target_count;
    router->enabled = UINT64_MAX;
    router->pending = 0;
    router->active = 0;
    router->programmable = 0;
    atomic_flag_clear(&router->lock);
    for (size_t i = 0; i < INTERRUPT_LINE_COUNT; ++i) {
        router->routes[i] = 0;
    }
    return 1;
}

int interrupt_router_set_route(InterruptRouter *router,
                               unsigned int line,
                               size_t target_index)
{
    if (router == NULL || line >= INTERRUPT_LINE_COUNT ||
        target_index >= router->target_count) {
        return 0;
    }

    router_lock(router);
    if (router->programmable &&
        (line >= INTERRUPT_EXTERNAL_LINE_COUNT ||
         (router->active & (UINT64_C(1) << line)) != 0)) {
        router_unlock(router);
        return 0;
    }

    router->routes[line] = target_index;
    if (router->programmable) {
        dispatch_target_locked(router, target_index);
    }
    router_unlock(router);
    return 1;
}

int interrupt_router_raise(InterruptRouter *router, unsigned int line)
{
    if (router == NULL || line >= INTERRUPT_LINE_COUNT ||
        router->targets == NULL) {
        return 0;
    }

    router_lock(router);
    if (!router->programmable) {
        size_t target = router->routes[line];
        int result = target < router->target_count &&
                     interrupt_controller_raise(router->targets[target],
                                                line);
        router_unlock(router);
        return result;
    }
    if (line >= INTERRUPT_EXTERNAL_LINE_COUNT) {
        router_unlock(router);
        return 0;
    }

    uint64_t bit = UINT64_C(1) << line;
    router->pending |= bit;
    if ((router->active & bit) == 0) {
        dispatch_target_locked(router, router->routes[line]);
    }
    router_unlock(router);
    return 1;
}

int interrupt_router_clear(InterruptRouter *router, unsigned int line)
{
    if (router == NULL || line >= INTERRUPT_LINE_COUNT ||
        router->targets == NULL) {
        return 0;
    }

    router_lock(router);
    if (router->programmable) {
        if (line >= INTERRUPT_EXTERNAL_LINE_COUNT) {
            router_unlock(router);
            return 0;
        }
        uint64_t bit = UINT64_C(1) << line;
        size_t target = router->routes[line];
        router->pending &= ~bit;
        if ((router->active & bit) != 0) {
            router->active &= ~bit;
            if (target < router->target_count) {
                (void)interrupt_controller_clear(router->targets[target],
                                                 line);
                dispatch_target_locked(router, target);
            }
        }
        router_unlock(router);
        return 1;
    }

    size_t target = router->routes[line];
    int result = target < router->target_count &&
                 interrupt_controller_clear(router->targets[target], line);
    router_unlock(router);
    return result;
}

void interrupt_router_programmable_reset(InterruptRouter *router)
{
    if (router == NULL || router->targets == NULL) {
        return;
    }

    router_lock(router);
    router->programmable = 1;
    router->enabled = 0;
    router->pending = 0;
    router->active = 0;
    for (size_t line = 0; line < INTERRUPT_LINE_COUNT; ++line) {
        router->routes[line] = 0;
    }
    for (size_t target = 0; target < router->target_count; ++target) {
        interrupt_controller_init(router->targets[target]);
    }
    router_unlock(router);
}

int interrupt_router_set_enabled(InterruptRouter *router, uint64_t mask)
{
    if (router == NULL ||
        (mask & ~INTERRUPT_EXTERNAL_LINE_MASK) != 0) {
        return 0;
    }
    router_lock(router);
    if (!router->programmable) {
        router_unlock(router);
        return 0;
    }
    router->enabled = mask;
    dispatch_all_locked(router);
    router_unlock(router);
    return 1;
}

int interrupt_router_enable_lines(InterruptRouter *router, uint64_t mask)
{
    if (router == NULL ||
        (mask & ~INTERRUPT_EXTERNAL_LINE_MASK) != 0) {
        return 0;
    }
    router_lock(router);
    if (!router->programmable) {
        router_unlock(router);
        return 0;
    }
    router->enabled |= mask;
    dispatch_all_locked(router);
    router_unlock(router);
    return 1;
}

int interrupt_router_disable_lines(InterruptRouter *router, uint64_t mask)
{
    if (router == NULL ||
        (mask & ~INTERRUPT_EXTERNAL_LINE_MASK) != 0) {
        return 0;
    }
    router_lock(router);
    if (!router->programmable) {
        router_unlock(router);
        return 0;
    }
    router->enabled &= ~mask;
    router_unlock(router);
    return 1;
}

uint64_t interrupt_router_enabled(InterruptRouter *router)
{
    if (router == NULL) {
        return 0;
    }
    router_lock(router);
    uint64_t value = router->enabled;
    router_unlock(router);
    return value;
}

uint64_t interrupt_router_pending(InterruptRouter *router)
{
    if (router == NULL) {
        return 0;
    }
    router_lock(router);
    uint64_t value = router->pending;
    router_unlock(router);
    return value;
}

uint64_t interrupt_router_active(InterruptRouter *router)
{
    if (router == NULL) {
        return 0;
    }
    router_lock(router);
    uint64_t value = router->active;
    router_unlock(router);
    return value;
}

int interrupt_router_get_route(InterruptRouter *router,
                               unsigned int line,
                               size_t *target_index)
{
    if (router == NULL || target_index == NULL ||
        line >= INTERRUPT_EXTERNAL_LINE_COUNT) {
        return 0;
    }
    router_lock(router);
    *target_index = router->routes[line];
    router_unlock(router);
    return 1;
}

int interrupt_router_eoi(InterruptRouter *router, unsigned int line)
{
    if (router == NULL || line >= INTERRUPT_EXTERNAL_LINE_COUNT) {
        return 0;
    }

    router_lock(router);
    uint64_t bit = UINT64_C(1) << line;
    if (!router->programmable || (router->active & bit) == 0) {
        router_unlock(router);
        return 0;
    }
    size_t target = router->routes[line];
    router->active &= ~bit;
    dispatch_target_locked(router, target);
    router_unlock(router);
    return 1;
}
