#include "device_manager.h"

#include <limits.h>
#include <string.h>

static void manager_lock(DeviceManager *manager)
{
    while (atomic_flag_test_and_set_explicit(&manager->lock,
                                             memory_order_acquire)) {
    }
}

static void manager_unlock(DeviceManager *manager)
{
    atomic_flag_clear_explicit(&manager->lock, memory_order_release);
}

static int descriptor_valid(const VmDeviceDescriptor *descriptor)
{
    if (descriptor == NULL ||
        descriptor->abi_version != VM_DEVICE_ABI_VERSION ||
        descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->bar_count > VM_DEVICE_MAX_BARS ||
        descriptor->irq_count > VM_DEVICE_MAX_IRQS) {
        return 0;
    }

    for (uint32_t i = 0; i < descriptor->bar_count; ++i) {
        uint64_t alignment = descriptor->bar_alignments[i];
        if (descriptor->bar_sizes[i] == 0 || alignment == 0 ||
            (alignment & (alignment - 1)) != 0) {
            return 0;
        }
    }
    return 1;
}

static int module_valid(const VmDeviceModule *module)
{
    return module != NULL &&
           module->abi_version == VM_DEVICE_ABI_VERSION &&
           module->struct_size >= sizeof(*module) &&
           descriptor_valid(&module->descriptor) &&
           module->descriptor.bar_count != 0 &&
           module->create != NULL && module->destroy != NULL &&
           (module->read != NULL || module->write != NULL);
}

static size_t free_slot_index(const DeviceManager *manager)
{
    for (size_t i = 0; i < DEVICE_MANAGER_MAX_SLOTS; ++i) {
        if (!manager->slots[i].present) {
            return i;
        }
    }
    return DEVICE_MANAGER_MAX_SLOTS;
}

static int range_end(uint64_t base, uint64_t size, uint64_t *end)
{
    if (size == 0 || base > UINT64_MAX - (size - 1)) {
        return 0;
    }
    *end = base + size - 1;
    return 1;
}

static int align_address(uint64_t value,
                         uint64_t alignment,
                         uint64_t *aligned)
{
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) {
        return 0;
    }
    *aligned = (value + mask) & ~mask;
    return 1;
}

static int allocate_irq(DeviceManager *manager, uint32_t *irq)
{
    for (unsigned int line = 2;
         line < INTERRUPT_EXTERNAL_LINE_COUNT;
         ++line) {
        if (!manager->irq_used[line]) {
            manager->irq_used[line] = 1;
            *irq = line;
            return 1;
        }
    }
    return 0;
}

static void release_irq(DeviceManager *manager, uint32_t irq)
{
    if (manager == NULL || irq >= INTERRUPT_EXTERNAL_LINE_COUNT) {
        return;
    }
    (void)interrupt_router_clear(manager->interrupts, irq);
    manager->irq_used[irq] = 0;
}

static int ranges_overlap(uint64_t first_base,
                          uint64_t first_size,
                          uint64_t second_base,
                          uint64_t second_size)
{
    uint64_t first_end = first_base + first_size - 1;
    uint64_t second_end = second_base + second_size - 1;
    return first_base <= second_end && second_base <= first_end;
}

static int conflicting_bar_end(const DeviceManager *manager,
                               const VmDeviceResources *pending,
                               uint32_t pending_bar_count,
                               uint64_t base,
                               uint64_t size,
                               uint64_t *conflict_end)
{
    int found = 0;
    uint64_t greatest_end = 0;

    for (size_t i = 0; i < DEVICE_MANAGER_MAX_SLOTS; ++i) {
        const DeviceManagerSlot *slot = &manager->slots[i];
        if (!slot->present) {
            continue;
        }
        for (uint32_t bar = 0; bar < slot->resources.bar_count; ++bar) {
            uint64_t other_base = slot->resources.bar_bases[bar];
            uint64_t other_size = slot->resources.bar_sizes[bar];
            if (ranges_overlap(base, size, other_base, other_size)) {
                uint64_t other_end = other_base + other_size - 1;
                if (!found || other_end > greatest_end) {
                    greatest_end = other_end;
                }
                found = 1;
            }
        }
    }

    for (uint32_t bar = 0; bar < pending_bar_count; ++bar) {
        uint64_t other_base = pending->bar_bases[bar];
        uint64_t other_size = pending->bar_sizes[bar];
        if (ranges_overlap(base, size, other_base, other_size)) {
            uint64_t other_end = other_base + other_size - 1;
            if (!found || other_end > greatest_end) {
                greatest_end = other_end;
            }
            found = 1;
        }
    }

    if (found && conflict_end != NULL) {
        *conflict_end = greatest_end;
    }
    return found;
}

static int allocate_bars(const DeviceManager *manager,
                         const VmDeviceDescriptor *descriptor,
                         VmDeviceResources *resources)
{
    for (uint32_t bar = 0; bar < descriptor->bar_count; ++bar) {
        uint64_t candidate = DEVICE_MANAGER_DYNAMIC_MMIO_BASE;
        uint64_t size = descriptor->bar_sizes[bar];
        uint64_t alignment = descriptor->bar_alignments[bar];

        for (;;) {
            uint64_t base;
            uint64_t end;
            if (!align_address(candidate, alignment, &base) ||
                !range_end(base, size, &end) ||
                end > DEVICE_MANAGER_DYNAMIC_MMIO_LIMIT) {
                return 0;
            }

            uint64_t conflict_end;
            if (!conflicting_bar_end(manager,
                                     resources,
                                     bar,
                                     base,
                                     size,
                                     &conflict_end)) {
                resources->bar_bases[bar] = base;
                resources->bar_sizes[bar] = size;
                break;
            }
            if (conflict_end == UINT64_MAX) {
                return 0;
            }
            candidate = conflict_end + 1;
        }
    }
    return 1;
}

static void publish_slot(DeviceManager *manager,
                         DeviceManagerSlot *slot,
                         size_t index,
                         size_t *slot_index)
{
    slot->present = 1;
    ++manager->present_count;
    ++manager->generation;
    manager->event_status |= VIO_HUB_EVENT_DEVICE_ADDED;
    if (slot_index != NULL) {
        *slot_index = index;
    }
}

static int host_dma_read(void *context,
                         uint64_t physical_address,
                         void *buffer,
                         uint64_t size)
{
    DeviceManager *manager = context;
    if (manager == NULL || manager->bus == NULL || size > SIZE_MAX) {
        return 0;
    }
    return ram_read_block(manager->bus->ram,
                          physical_address,
                          buffer,
                          (size_t)size);
}

static int host_dma_write(void *context,
                          uint64_t physical_address,
                          const void *buffer,
                          uint64_t size)
{
    DeviceManager *manager = context;
    if (manager == NULL || manager->bus == NULL || size > SIZE_MAX) {
        return 0;
    }
    return ram_write_block(manager->bus->ram,
                           physical_address,
                           buffer,
                           (size_t)size);
}

static int host_raise_irq(void *context, uint32_t irq)
{
    DeviceManager *manager = context;
    return manager != NULL && manager->interrupts != NULL &&
           interrupt_router_raise(manager->interrupts, irq);
}

static void host_log(void *context, uint32_t level, const char *message)
{
    DeviceManager *manager = context;
    if (manager != NULL && manager->log_callback != NULL) {
        manager->log_callback(manager->log_context, level, message);
    }
}

static void *host_get_service(void *context,
                              const char *service_name,
                              uint32_t service_version)
{
    DeviceManager *manager = context;
    if (manager == NULL || service_name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < manager->service_count; ++i) {
        DeviceManagerService *service = &manager->services[i];
        if (service->version == service_version &&
            strcmp(service->name, service_name) == 0) {
            return service->service;
        }
    }
    return NULL;
}

static int module_bar_read(void *context,
                           uint64_t offset,
                           size_t width,
                           uint64_t *value)
{
    DeviceBarAdapter *bar = context;
    if (bar == NULL || bar->slot == NULL || width > UINT32_MAX ||
        bar->slot->module == NULL || bar->slot->module->read == NULL) {
        return 0;
    }
    return bar->slot->module->read(bar->slot->device_context,
                                   bar->bar,
                                   offset,
                                   (uint32_t)width,
                                   value);
}

static int module_bar_write(void *context,
                            uint64_t offset,
                            size_t width,
                            uint64_t value)
{
    DeviceBarAdapter *bar = context;
    if (bar == NULL || bar->slot == NULL || width > UINT32_MAX ||
        bar->slot->module == NULL || bar->slot->module->write == NULL) {
        return 0;
    }
    return bar->slot->module->write(bar->slot->device_context,
                                    bar->bar,
                                    offset,
                                    (uint32_t)width,
                                    value);
}

static void module_tick(void *context,
                        uint64_t ticks,
                        InterruptRouter *interrupts)
{
    (void)interrupts;
    DeviceBarAdapter *bar = context;
    if (bar != NULL && bar->slot != NULL && bar->bar == 0 &&
        bar->slot->module != NULL &&
        bar->slot->module->tick != NULL) {
        bar->slot->module->tick(bar->slot->device_context, ticks);
    }
}

static void module_reset(void *context)
{
    DeviceBarAdapter *bar = context;
    if (bar != NULL && bar->slot != NULL && bar->bar == 0 &&
        bar->slot->module != NULL &&
        bar->slot->module->reset != NULL) {
        bar->slot->module->reset(bar->slot->device_context);
    }
}

int device_manager_init(DeviceManager *manager,
                        Bus *bus,
                        InterruptRouter *interrupts)
{
    if (manager == NULL || bus == NULL || bus->ram == NULL ||
        interrupts == NULL) {
        return 0;
    }

    *manager = (DeviceManager){0};
    atomic_flag_clear(&manager->lock);
    manager->bus = bus;
    manager->interrupts = interrupts;
    manager->host_api = (VmDeviceHostApi){
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceHostApi),
        .context = manager,
        .dma_read = host_dma_read,
        .dma_write = host_dma_write,
        .raise_irq = host_raise_irq,
        .log = host_log,
        .get_service = host_get_service
    };
    return 1;
}

static int register_service_locked(DeviceManager *manager,
                                   const char *name,
                                   uint32_t version,
                                   void *service)
{
    if (manager == NULL || name == NULL || *name == '\0' ||
        strlen(name) >= VM_DEVICE_NAME_CAPACITY || version == 0 ||
        service == NULL ||
        manager->service_count >= DEVICE_MANAGER_MAX_SERVICES) {
        return 0;
    }
    for (size_t i = 0; i < manager->service_count; ++i) {
        if (manager->services[i].version == version &&
            strcmp(manager->services[i].name, name) == 0) {
            return 0;
        }
    }

    DeviceManagerService *entry =
        &manager->services[manager->service_count++];
    (void)strcpy(entry->name, name);
    entry->version = version;
    entry->service = service;
    return 1;
}

int device_manager_register_service(DeviceManager *manager,
                                    const char *name,
                                    uint32_t version,
                                    void *service)
{
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = register_service_locked(manager, name, version, service);
    manager_unlock(manager);
    return result;
}

void device_manager_set_log_callback(DeviceManager *manager,
                                     DeviceManagerLogCallback callback,
                                     void *context)
{
    if (manager == NULL) {
        return;
    }
    manager_lock(manager);
    manager->log_callback = callback;
    manager->log_context = context;
    manager_unlock(manager);
}

static int publish_fixed_locked(DeviceManager *manager,
                                const VmDeviceDescriptor *descriptor,
                                const VmDeviceResources *resources,
                                size_t *slot_index)
{
    if (manager == NULL || !descriptor_valid(descriptor) ||
        resources == NULL ||
        resources->struct_size < sizeof(*resources) ||
        resources->bar_count != descriptor->bar_count ||
        resources->irq_count != descriptor->irq_count) {
        return 0;
    }

    size_t index = free_slot_index(manager);
    if (index == DEVICE_MANAGER_MAX_SLOTS) {
        return 0;
    }

    for (uint32_t i = 0; i < resources->irq_count; ++i) {
        uint32_t irq = resources->irqs[i];
        if (irq >= INTERRUPT_EXTERNAL_LINE_COUNT ||
            manager->irq_used[irq]) {
            return 0;
        }
        for (uint32_t previous = 0; previous < i; ++previous) {
            if (resources->irqs[previous] == irq) {
                return 0;
            }
        }
    }
    for (uint32_t i = 0; i < resources->bar_count; ++i) {
        uint64_t end;
        if (resources->bar_sizes[i] != descriptor->bar_sizes[i] ||
            (resources->bar_bases[i] &
             (descriptor->bar_alignments[i] - 1)) != 0 ||
            !range_end(resources->bar_bases[i],
                       resources->bar_sizes[i],
                       &end)) {
            return 0;
        }
    }

    DeviceManagerSlot *slot = &manager->slots[index];
    *slot = (DeviceManagerSlot){0};
    slot->descriptor = *descriptor;
    slot->descriptor.name[VM_DEVICE_NAME_CAPACITY - 1] = '\0';
    slot->resources = *resources;
    for (uint32_t i = 0; i < resources->irq_count; ++i) {
        manager->irq_used[resources->irqs[i]] = 1;
    }
    publish_slot(manager, slot, index, slot_index);
    return 1;
}

int device_manager_publish_fixed(DeviceManager *manager,
                                 const VmDeviceDescriptor *descriptor,
                                 const VmDeviceResources *resources,
                                 size_t *slot_index)
{
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = publish_fixed_locked(manager,
                                      descriptor,
                                      resources,
                                      slot_index);
    manager_unlock(manager);
    return result;
}

static int attach_module_locked(DeviceManager *manager,
                                const VmDeviceModule *module,
                                const char *configuration,
                                size_t *slot_index)
{
    if (manager == NULL || !module_valid(module)) {
        return 0;
    }

    size_t index = free_slot_index(manager);
    if (index == DEVICE_MANAGER_MAX_SLOTS) {
        return 0;
    }

    VmDeviceResources resources = {
        .struct_size = sizeof(VmDeviceResources),
        .bar_count = module->descriptor.bar_count,
        .irq_count = module->descriptor.irq_count
    };
    for (uint32_t irq = 0; irq < VM_DEVICE_MAX_IRQS; ++irq) {
        resources.irqs[irq] = UINT32_MAX;
    }
    if (!allocate_bars(manager, &module->descriptor, &resources)) {
        return 0;
    }
    uint32_t allocated_irqs = 0;
    for (; allocated_irqs < resources.irq_count; ++allocated_irqs) {
        if (!allocate_irq(manager, &resources.irqs[allocated_irqs])) {
            for (uint32_t irq = 0; irq < allocated_irqs; ++irq) {
                release_irq(manager, resources.irqs[irq]);
            }
            return 0;
        }
    }

    DeviceManagerSlot *slot = &manager->slots[index];
    *slot = (DeviceManagerSlot){0};
    slot->external = 1;
    slot->descriptor = module->descriptor;
    slot->descriptor.name[VM_DEVICE_NAME_CAPACITY - 1] = '\0';
    slot->resources = resources;
    slot->module = module;
    for (uint32_t bar = 0; bar < resources.bar_count; ++bar) {
        slot->bars[bar].slot = slot;
        slot->bars[bar].bar = bar;
    }

    if (!module->create(&manager->host_api,
                        &slot->resources,
                        configuration != NULL ? configuration : "",
                        &slot->device_context) ||
        slot->device_context == NULL) {
        for (uint32_t irq = 0; irq < resources.irq_count; ++irq) {
            release_irq(manager, resources.irqs[irq]);
        }
        *slot = (DeviceManagerSlot){0};
        return 0;
    }

    uint32_t mapped_bars = 0;
    for (; mapped_bars < resources.bar_count; ++mapped_bars) {
        BusDevice bus_device = {
            .context = &slot->bars[mapped_bars],
            .read = module_bar_read,
            .write = module_bar_write,
            .tick = mapped_bars == 0 ? module_tick : NULL,
            .reset = mapped_bars == 0 ? module_reset : NULL
        };
        if (!bus_map_device(manager->bus,
                            resources.bar_bases[mapped_bars],
                            resources.bar_sizes[mapped_bars],
                            bus_device)) {
            break;
        }
    }
    if (mapped_bars != resources.bar_count) {
        for (uint32_t bar = 0; bar < mapped_bars; ++bar) {
            (void)bus_unmap_device(manager->bus,
                                   resources.bar_bases[bar],
                                   resources.bar_sizes[bar]);
        }
        module->destroy(slot->device_context);
        for (uint32_t irq = 0; irq < resources.irq_count; ++irq) {
            release_irq(manager, resources.irqs[irq]);
        }
        *slot = (DeviceManagerSlot){0};
        return 0;
    }

    publish_slot(manager, slot, index, slot_index);
    return 1;
}

int device_manager_attach_module(DeviceManager *manager,
                                 const VmDeviceModule *module,
                                 const char *configuration,
                                 size_t *slot_index)
{
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = attach_module_locked(manager,
                                      module,
                                      configuration,
                                      slot_index);
    manager_unlock(manager);
    return result;
}

static int detach_module_locked(DeviceManager *manager,
                                size_t slot_index)
{
    if (manager == NULL || slot_index >= DEVICE_MANAGER_MAX_SLOTS) {
        return 0;
    }

    DeviceManagerSlot *slot = &manager->slots[slot_index];
    if (!slot->present || !slot->external || slot->module == NULL) {
        return 0;
    }

    uint32_t unmapped_bars = 0;
    for (; unmapped_bars < slot->resources.bar_count; ++unmapped_bars) {
        if (!bus_unmap_device(manager->bus,
                              slot->resources.bar_bases[unmapped_bars],
                              slot->resources.bar_sizes[unmapped_bars])) {
            break;
        }
    }
    if (unmapped_bars != slot->resources.bar_count) {
        for (uint32_t bar = 0; bar < unmapped_bars; ++bar) {
            BusDevice bus_device = {
                .context = &slot->bars[bar],
                .read = module_bar_read,
                .write = module_bar_write,
                .tick = bar == 0 ? module_tick : NULL,
                .reset = bar == 0 ? module_reset : NULL
            };
            (void)bus_map_device(manager->bus,
                                 slot->resources.bar_bases[bar],
                                 slot->resources.bar_sizes[bar],
                                 bus_device);
        }
        return 0;
    }

    for (uint32_t irq = 0; irq < slot->resources.irq_count; ++irq) {
        release_irq(manager, slot->resources.irqs[irq]);
    }
    if (slot->module->destroy != NULL && slot->device_context != NULL) {
        slot->module->destroy(slot->device_context);
    }

    *slot = (DeviceManagerSlot){0};
    --manager->present_count;
    ++manager->generation;
    manager->event_status |= VIO_HUB_EVENT_DEVICE_REMOVED;
    return 1;
}

int device_manager_detach_module(DeviceManager *manager,
                                 size_t slot_index)
{
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = detach_module_locked(manager, slot_index);
    manager_unlock(manager);
    return result;
}

const DeviceManagerSlot *device_manager_slot(const DeviceManager *manager,
                                             size_t slot_index)
{
    if (manager == NULL || slot_index >= DEVICE_MANAGER_MAX_SLOTS ||
        !manager->slots[slot_index].present) {
        return NULL;
    }
    return &manager->slots[slot_index];
}

void device_manager_destroy(DeviceManager *manager)
{
    if (manager == NULL) {
        return;
    }

    for (size_t i = 0; i < DEVICE_MANAGER_MAX_SLOTS; ++i) {
        DeviceManagerSlot *slot = &manager->slots[i];
        if (slot->present && slot->external) {
            (void)device_manager_detach_module(manager, i);
        }
    }
    *manager = (DeviceManager){0};
}

static int bar_base_field(uint64_t field, uint32_t *bar)
{
    static const uint64_t offsets[VM_DEVICE_MAX_BARS] = {
        VIO_SLOT_BAR0_BASE_OFFSET,
        VIO_SLOT_BAR1_BASE_OFFSET,
        VIO_SLOT_BAR2_BASE_OFFSET,
        VIO_SLOT_BAR3_BASE_OFFSET
    };
    for (uint32_t i = 0; i < VM_DEVICE_MAX_BARS; ++i) {
        if (field == offsets[i]) {
            *bar = i;
            return 1;
        }
    }
    return 0;
}

static int bar_size_field(uint64_t field, uint32_t *bar)
{
    static const uint64_t offsets[VM_DEVICE_MAX_BARS] = {
        VIO_SLOT_BAR0_SIZE_OFFSET,
        VIO_SLOT_BAR1_SIZE_OFFSET,
        VIO_SLOT_BAR2_SIZE_OFFSET,
        VIO_SLOT_BAR3_SIZE_OFFSET
    };
    for (uint32_t i = 0; i < VM_DEVICE_MAX_BARS; ++i) {
        if (field == offsets[i]) {
            *bar = i;
            return 1;
        }
    }
    return 0;
}

static int irq_field(uint64_t field, uint32_t *irq)
{
    static const uint64_t offsets[VM_DEVICE_MAX_IRQS] = {
        VIO_SLOT_IRQ0_OFFSET,
        VIO_SLOT_IRQ1_OFFSET,
        VIO_SLOT_IRQ2_OFFSET,
        VIO_SLOT_IRQ3_OFFSET
    };
    for (uint32_t i = 0; i < VM_DEVICE_MAX_IRQS; ++i) {
        if (field == offsets[i]) {
            *irq = i;
            return 1;
        }
    }
    return 0;
}

static uint64_t slot_value(const DeviceManagerSlot *slot,
                           uint64_t field)
{
    uint32_t resource_index;
    if (slot == NULL || !slot->present) {
        return irq_field(field, &resource_index) ? UINT64_MAX : 0;
    }
    if (bar_base_field(field, &resource_index)) {
        return resource_index < slot->resources.bar_count
                   ? slot->resources.bar_bases[resource_index]
                   : 0;
    }
    if (bar_size_field(field, &resource_index)) {
        return resource_index < slot->resources.bar_count
                   ? slot->resources.bar_sizes[resource_index]
                   : 0;
    }
    if (irq_field(field, &resource_index)) {
        return resource_index < slot->resources.irq_count
                   ? slot->resources.irqs[resource_index]
                   : UINT64_MAX;
    }

    switch (field) {
        case VIO_SLOT_STATUS_OFFSET:
            return VIO_SLOT_STATUS_PRESENT |
                   (slot->external ? VIO_SLOT_STATUS_EXTERNAL : 0);
        case VIO_SLOT_CLASS_OFFSET:
            return slot->descriptor.device_class;
        case VIO_SLOT_VENDOR_OFFSET:
            return slot->descriptor.vendor_id;
        case VIO_SLOT_DEVICE_OFFSET:
            return slot->descriptor.device_id;
        case VIO_SLOT_VERSION_OFFSET:
            return slot->descriptor.device_version;
        case VIO_SLOT_FEATURES_OFFSET:
            return slot->descriptor.features;
        case VIO_SLOT_DEVICE_ABI_OFFSET:
            return slot->descriptor.abi_version;
        case VIO_SLOT_BAR_COUNT_OFFSET:
            return slot->resources.bar_count;
        case VIO_SLOT_IRQ_COUNT_OFFSET:
            return slot->resources.irq_count;
        default:
            return 0;
    }
}

static int hub_read_locked(void *context,
                           uint64_t offset,
                           size_t width,
                           uint64_t *value)
{
    DeviceManager *manager = context;
    if (manager == NULL || value == NULL ||
        width != sizeof(uint64_t) || (offset & UINT64_C(7)) != 0) {
        return 0;
    }

    switch (offset) {
        case VIO_HUB_MAGIC_OFFSET:
            *value = VIO_HUB_MAGIC;
            return 1;
        case VIO_HUB_ABI_OFFSET:
            *value = VIO_HUB_ABI_VERSION;
            return 1;
        case VIO_HUB_SLOT_COUNT_OFFSET:
            *value = DEVICE_MANAGER_MAX_SLOTS;
            return 1;
        case VIO_HUB_GENERATION_OFFSET:
            *value = manager->generation;
            return 1;
        case VIO_HUB_EVENT_STATUS_OFFSET:
            *value = manager->event_status;
            return 1;
        default:
            break;
    }

    if (offset < VIO_HUB_SLOT_BASE) {
        return 0;
    }
    uint64_t relative = offset - VIO_HUB_SLOT_BASE;
    size_t slot_index = (size_t)(relative / VIO_HUB_SLOT_STRIDE);
    uint64_t field = relative % VIO_HUB_SLOT_STRIDE;
    if (slot_index >= DEVICE_MANAGER_MAX_SLOTS) {
        return 0;
    }
    *value = slot_value(&manager->slots[slot_index], field);
    return 1;
}

static int hub_write_locked(void *context,
                            uint64_t offset,
                            size_t width,
                            uint64_t value)
{
    DeviceManager *manager = context;
    if (manager == NULL || width != sizeof(uint64_t) ||
        offset != VIO_HUB_EVENT_ACK_OFFSET) {
        return 0;
    }
    manager->event_status &= ~value;
    return 1;
}

static int hub_read(void *context,
                    uint64_t offset,
                    size_t width,
                    uint64_t *value)
{
    DeviceManager *manager = context;
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = hub_read_locked(context, offset, width, value);
    manager_unlock(manager);
    return result;
}

static int hub_write(void *context,
                     uint64_t offset,
                     size_t width,
                     uint64_t value)
{
    DeviceManager *manager = context;
    if (manager == NULL) {
        return 0;
    }
    manager_lock(manager);
    int result = hub_write_locked(context, offset, width, value);
    manager_unlock(manager);
    return result;
}

BusDevice device_manager_hub_as_bus_device(DeviceManager *manager)
{
    BusDevice device = {
        .context = manager,
        .read = hub_read,
        .write = hub_write,
        .tick = NULL,
        .reset = NULL
    };
    return device;
}
