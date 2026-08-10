#include "bus.h"
#include "boot.h"
#include "core_control.h"
#include "cpu.h"
#include "device_manager.h"
#include "headless_display.h"
#include "interrupt.h"
#include "irq_controller.h"
#include "keyboard_host.h"
#include "keyboard_input.h"
#include "main_options.h"
#include "module_loader.h"
#include "mmu.h"
#include "ram.h"
#include "system_control.h"
#include "system_info.h"
#include "timer.h"
#include "uart.h"
#include "vm.h"
#include "window_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMER_MMIO_BASE UINT64_C(0xFFFFFFFFFFFF0000)
#define UART_MMIO_BASE UINT64_C(0xFFFFFFFFFFFD0000)
#define VIO_VENDOR_ID UINT32_C(0x564D)

static uint8_t BOOT_ROM_STORAGE[VM_BOOT_ROM_MAX_SIZE];

static const char *main_program_name(const char *path)
{
    if (path == NULL || *path == '\0') {
        return "main";
    }
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *separator = slash;
    if (backslash != NULL &&
        (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    return separator != NULL ? separator + 1 : path;
}

typedef struct {
    MainDisplayMode mode;
    HeadlessDisplay headless;
    WindowDisplay window;
    KeyboardInput keyboard_input;
    int keyboard_initialized;
} MainDisplayFrontend;

static int main_display_init(MainDisplayFrontend *frontend,
                             MainDisplayMode mode)
{
    if (frontend == NULL) {
        return 0;
    }
    *frontend = (MainDisplayFrontend){0};
    frontend->mode = mode;
    if (!keyboard_input_init(&frontend->keyboard_input)) {
        return 0;
    }
    frontend->keyboard_initialized = 1;
    int initialized;
    if (mode == MAIN_DISPLAY_WINDOW) {
        initialized = window_display_init(&frontend->window,
                                          "Custom VM Display",
                                          &frontend->keyboard_input);
    } else {
        initialized = headless_display_init(&frontend->headless);
    }
    if (!initialized) {
        keyboard_input_destroy(&frontend->keyboard_input);
        frontend->keyboard_initialized = 0;
    }
    return initialized;
}

static void main_display_destroy(MainDisplayFrontend *frontend)
{
    if (frontend == NULL) {
        return;
    }
    if (frontend->mode == MAIN_DISPLAY_WINDOW) {
        window_display_destroy(&frontend->window);
    } else {
        headless_display_destroy(&frontend->headless);
    }
    if (frontend->keyboard_initialized) {
        keyboard_input_destroy(&frontend->keyboard_input);
        frontend->keyboard_initialized = 0;
    }
}

static VmDisplayHost *main_display_host(MainDisplayFrontend *frontend)
{
    if (frontend == NULL) {
        return NULL;
    }
    return frontend->mode == MAIN_DISPLAY_WINDOW
               ? window_display_host(&frontend->window)
               : headless_display_host(&frontend->headless);
}

static VmKeyboardHost *main_keyboard_host(MainDisplayFrontend *frontend)
{
    return frontend != NULL
               ? keyboard_input_host(&frontend->keyboard_input)
               : NULL;
}

static int main_display_frame_info(MainDisplayFrontend *frontend,
                                   size_t *frame_size,
                                   uint32_t *width,
                                   uint32_t *height,
                                   uint64_t *frame_number,
                                   uint64_t *dropped_frames)
{
    if (frontend == NULL) {
        return 0;
    }
    if (frontend->mode == MAIN_DISPLAY_WINDOW) {
        return window_display_frame_info(&frontend->window,
                                         frame_size,
                                         width,
                                         height,
                                         NULL,
                                         NULL,
                                         frame_number,
                                         dropped_frames);
    }
    if (dropped_frames != NULL) {
        *dropped_frames = 0;
    }
    return headless_display_frame_info(&frontend->headless,
                                       frame_size,
                                       width,
                                       height,
                                       NULL,
                                       NULL,
                                       frame_number);
}

static VmDeviceDescriptor fixed_device_descriptor(
    const char *name,
    uint32_t device_class,
    uint32_t device_id,
    uint64_t mmio_size,
    uint32_t irq_count)
{
    VmDeviceDescriptor descriptor = {
        .abi_version = VM_DEVICE_ABI_VERSION,
        .struct_size = sizeof(VmDeviceDescriptor),
        .device_class = device_class,
        .vendor_id = VIO_VENDOR_ID,
        .device_id = device_id,
        .device_version = 1,
        .bar_count = 1,
        .irq_count = irq_count,
        .bar_sizes = { mmio_size },
        .bar_alignments = { UINT64_C(8) }
    };
    (void)snprintf(descriptor.name,
                   sizeof(descriptor.name),
                   "%s",
                   name);
    return descriptor;
}

static VmDeviceResources fixed_device_resources(uint64_t base,
                                                uint64_t size,
                                                int has_irq,
                                                uint32_t irq)
{
    VmDeviceResources resources = {
        .struct_size = sizeof(VmDeviceResources),
        .bar_count = 1,
        .irq_count = has_irq ? 1U : 0U,
        .bar_bases = { base },
        .bar_sizes = { size },
        .irqs = { has_irq ? irq : UINT32_MAX }
    };
    return resources;
}

static void device_log(void *context,
                       uint32_t level,
                       const char *message)
{
    FILE *stream = context;
    if (stream != NULL && message != NULL) {
        (void)fprintf(stream,
                      "Device[%u]: %s\n",
                      (unsigned int)level,
                      message);
    }
}

static void unload_device_modules(VmLoadedDeviceModule *modules,
                                  size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        vm_device_module_unload(&modules[i]);
    }
}

static int load_and_attach_device(DeviceManager *manager,
                                  const char *path,
                                  const char *configuration,
                                  VmLoadedDeviceModule *loaded_modules,
                                  size_t loaded_capacity,
                                  size_t *loaded_count)
{
    if (manager == NULL || path == NULL || loaded_modules == NULL ||
        loaded_count == NULL || *loaded_count >= loaded_capacity) {
        return 0;
    }

    VmLoadedDeviceModule loaded;
    char error[256];
    if (!vm_device_module_load(path,
                               &loaded,
                               error,
                               sizeof(error))) {
        fprintf(stderr,
                "RunError: failed to load device %s: %s\n",
                path,
                error);
        return 0;
    }

    size_t slot_index;
    if (!device_manager_attach_module(manager,
                                      loaded.module,
                                      configuration != NULL
                                          ? configuration
                                          : "",
                                      &slot_index)) {
        fprintf(stderr,
                "RunError: failed to attach device module %s\n",
                path);
        vm_device_module_unload(&loaded);
        return 0;
    }
    loaded_modules[(*loaded_count)++] = loaded;

    const DeviceManagerSlot *slot = device_manager_slot(manager,
                                                        slot_index);
    printf("Device module: %s, slot %zu",
           slot != NULL ? slot->descriptor.name : path,
           slot_index);
    if (slot != NULL) {
        for (uint32_t bar = 0; bar < slot->resources.bar_count; ++bar) {
            printf(", BAR%u 0x%llX+0x%llX",
                   bar,
                   (unsigned long long)slot->resources.bar_bases[bar],
                   (unsigned long long)slot->resources.bar_sizes[bar]);
        }
        for (uint32_t irq = 0; irq < slot->resources.irq_count; ++irq) {
            printf(", IRQ%u %u", irq, slot->resources.irqs[irq]);
        }
    }
    putchar('\n');
    return 1;
}

static void uart_stdout_tx(void *context, uint8_t value)
{
    FILE *stream = context;
    if (stream == NULL) {
        return;
    }

    (void)fputc((int)value, stream);
    if (value == (uint8_t)'\n') {
        (void)fflush(stream);
    }
}

static int load_binary(RAM *ram,
                       const char *path,
                       size_t load_address,
                       size_t *loaded_size)
{
    if (ram == NULL || ram->data == NULL || path == NULL ||
        loaded_size == NULL || load_address > ram->size) {
        return 0;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror("RunError: cannot open program file");
        return 0;
    }

    size_t capacity = ram->size - load_address;
    size_t size = fread(ram->data + load_address, 1, capacity, file);

    if (ferror(file)) {
        perror("RunError: failed to read program file");
        fclose(file);
        return 0;
    }

    /* fread가 RAM 끝에서 멈춘 경우 파일 데이터가 더 남았는지 확인한다. */
    if (size == capacity) {
        int next_byte = fgetc(file);

        if (next_byte != EOF) {
            fprintf(stderr,
                    "RunError: program file is larger than available RAM\n");
            fclose(file);
            return 0;
        }
        if (ferror(file)) {
            perror("RunError: failed to check program file size");
            fclose(file);
            return 0;
        }
    }

    if (fclose(file) != 0) {
        perror("RunError: failed to close program file");
        return 0;
    }

    if (size == 0) {
        fputs("RunError: program file is empty\n", stderr);
        return 0;
    }

    *loaded_size = size;
    return 1;
}

static int load_boot_rom(const char *path, size_t *loaded_size)
{
    if (path == NULL || loaded_size == NULL) {
        return 0;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror("RunError: cannot open boot ROM file");
        return 0;
    }

    size_t size = fread(BOOT_ROM_STORAGE,
                        1,
                        sizeof(BOOT_ROM_STORAGE),
                        file);
    if (ferror(file)) {
        perror("RunError: failed to read boot ROM file");
        fclose(file);
        return 0;
    }
    if (size == sizeof(BOOT_ROM_STORAGE)) {
        int next_byte = fgetc(file);
        if (next_byte != EOF) {
            fputs("RunError: boot ROM image exceeds 1 MiB\n", stderr);
            fclose(file);
            return 0;
        }
        if (ferror(file)) {
            perror("RunError: failed to check boot ROM size");
            fclose(file);
            return 0;
        }
    }
    if (fclose(file) != 0) {
        perror("RunError: failed to close boot ROM file");
        return 0;
    }
    if (size == 0) {
        fputs("RunError: boot ROM file is empty\n", stderr);
        return 0;
    }
    *loaded_size = size;
    return 1;
}

int main(int argc, char *argv[])
{
    const char *program_name = main_program_name(argv[0]);
    MainOptions options;
    MainOptionError option_error;
    int window_supported = window_display_supported();
    MainOptionsResult option_result = main_options_parse(argc,
                                                         argv,
                                                         window_supported,
                                                         &options,
                                                         &option_error);
    if (option_result == MAIN_OPTIONS_HELP) {
        main_options_print_help(stdout, program_name, window_supported);
        return EXIT_SUCCESS;
    }
    if (option_result == MAIN_OPTIONS_ERROR) {
        fprintf(stderr,
                "%s: error: %s\n"
                "Try '%s --help' for more information.\n",
                program_name,
                option_error.message,
                program_name);
        return 2;
    }

    size_t ram_size = options.ram_size;
    size_t core_count = options.core_count;
    size_t threads_per_core = options.threads_per_core;
    const char *program_path = options.program_path;
    const char *boot_rom_path = options.boot_rom_path;
    MainDisplayMode display_mode = options.display_mode;
    const char *const *device_paths = options.device_paths;
    const char *const *device_configurations =
        options.device_configurations;
    size_t device_path_count = options.device_path_count;

    RAM ram = {
        .data = malloc(ram_size),
        .size = ram_size
    };

    if (ram.data == NULL) {
        fprintf(stderr, "RunError: failed to allocate %zu bytes\n", ram_size);
        return EXIT_FAILURE;
    }

    size_t program_size = 0;
    if (program_path != NULL) {
        if (!load_binary(&ram,
                         program_path,
                         VM_DIRECT_LOAD_ADDRESS,
                         &program_size)) {
            free(ram.data);
            return EXIT_FAILURE;
        }
    }

    size_t boot_rom_size = 0;
    if (boot_rom_path != NULL &&
        !load_boot_rom(boot_rom_path, &boot_rom_size)) {
        free(ram.data);
        return EXIT_FAILURE;
    }
    uint64_t reset_vector = boot_rom_path != NULL
                                ? VM_BOOT_ROM_BASE
                                : VM_DIRECT_LOAD_ADDRESS;

    printf("RAM Size: %zu bytes\n", ram_size);
    if (program_path != NULL) {
        printf("Loaded: %zu bytes at 0x%zX\n",
               program_size,
               VM_DIRECT_LOAD_ADDRESS);
    }

    Bus bus;
    if (!bus_init(&bus, &ram)) {
        fputs("RunError: failed to initialize bus\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }
    if (boot_rom_path != NULL &&
        !bus_map_rom(&bus,
                     VM_BOOT_ROM_BASE,
                     BOOT_ROM_STORAGE,
                     boot_rom_size)) {
        fputs("RunError: failed to map boot ROM\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }
    if (boot_rom_path != NULL) {
        printf("Boot ROM: %zu bytes at 0x%llX\n",
               boot_rom_size,
               (unsigned long long)VM_BOOT_ROM_BASE);
    }

    TimerDevice timer;
    if (!timer_device_init(&timer, TIMER_INTERRUPT_LINE) ||
        !bus_map_device(&bus,
                        TIMER_MMIO_BASE,
                        TIMER_MMIO_SIZE,
                        timer_device_as_bus_device(&timer))) {
        fputs("RunError: failed to initialize timer device\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }

    UartDevice uart;
    if (!uart_device_init(&uart, UART_INTERRUPT_LINE)) {
        fputs("RunError: failed to initialize UART device\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }
    uart_device_set_tx_callback(&uart, uart_stdout_tx, stdout);
    if (!bus_map_device(&bus,
                        UART_MMIO_BASE,
                        UART_MMIO_SIZE,
                        uart_device_as_bus_device(&uart))) {
        fputs("RunError: failed to map UART device\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }

    VirtualMachine vm;
    if (!vm_init(&vm,
                 &ram,
                 &bus,
                 core_count,
                 threads_per_core)) {
        fputs("RunError: failed to initialize virtual machine\n", stderr);
        free(ram.data);
        return EXIT_FAILURE;
    }

    IrqControllerDevice irq_controller;
    if (!irq_controller_device_init(&irq_controller,
                                    &vm.interrupt_router) ||
        !bus_map_device(
            &bus,
            IRQ_CONTROLLER_MMIO_BASE,
            IRQ_CONTROLLER_MMIO_SIZE,
            irq_controller_device_as_bus_device(&irq_controller))) {
        fputs("RunError: failed to initialize IRQ controller\n", stderr);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    CoreControlDevice core_control;
    if (!core_control_device_init(&core_control, &vm) ||
        !bus_map_device(&bus,
                        CORE_CONTROL_MMIO_BASE,
                        CORE_CONTROL_MMIO_SIZE,
                        core_control_device_as_bus_device(&core_control))) {
        fputs("RunError: failed to initialize core control device\n",
              stderr);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    SystemInfoDevice system_info_configuration = {
        .features = SYSTEM_INFO_FEATURE_MMU |
                    SYSTEM_INFO_FEATURE_MULTICORE |
                    SYSTEM_INFO_FEATURE_INTERRUPTS |
                    SYSTEM_INFO_FEATURE_ATOMICS |
                    SYSTEM_INFO_FEATURE_SIMD |
                    SYSTEM_INFO_FEATURE_FLOATING_POINT |
                    SYSTEM_INFO_FEATURE_VIO |
                    SYSTEM_INFO_FEATURE_SYSTEM_CONTROL |
                    SYSTEM_INFO_FEATURE_IRQ_CONTROLLER |
                    (boot_rom_path != NULL
                         ? SYSTEM_INFO_FEATURE_BOOT_ROM
                         : 0),
        .ram_base = 0,
        .ram_size = (uint64_t)ram_size,
        .rom_base = boot_rom_path != NULL ? VM_BOOT_ROM_BASE : 0,
        .rom_size = (uint64_t)boot_rom_size,
        .reset_vector = reset_vector,
        .core_count = (uint64_t)core_count,
        .threads_per_core = (uint64_t)threads_per_core,
        .logical_processor_count =
            (uint64_t)(core_count * threads_per_core),
        .physical_address_bits = 64,
        .page_size = MMU_PAGE_SIZE,
        .virtual_address_bits = MMU_VIRTUAL_ADDRESS_BITS,
        .timer_frequency = TIMER_TICKS_PER_SECOND,
        .interrupt_line_count = INTERRUPT_LINE_COUNT,
        .vio_hub_base = VIO_HUB_MMIO_BASE,
        .vio_slot_count = DEVICE_MANAGER_MAX_SLOTS,
        .dynamic_mmio_base = DEVICE_MANAGER_DYNAMIC_MMIO_BASE,
        .dynamic_mmio_size = DEVICE_MANAGER_DYNAMIC_MMIO_LIMIT -
                             DEVICE_MANAGER_DYNAMIC_MMIO_BASE + 1,
        .system_control_base = SYSTEM_CONTROL_MMIO_BASE,
        .external_interrupt_line_count =
            INTERRUPT_EXTERNAL_LINE_COUNT,
        .ipi_line_base = INTERRUPT_IPI_LINE_BASE,
        .irq_controller_base = IRQ_CONTROLLER_MMIO_BASE
    };
    SystemInfoDevice system_info;
    SystemControlDevice system_control;
    if (!system_info_device_init(&system_info,
                                 &system_info_configuration) ||
        !bus_map_device(&bus,
                        SYSTEM_INFO_MMIO_BASE,
                        SYSTEM_INFO_MMIO_SIZE,
                        system_info_device_as_bus_device(&system_info)) ||
        !system_control_device_init(&system_control, &vm) ||
        !bus_map_device(
            &bus,
            SYSTEM_CONTROL_MMIO_BASE,
            SYSTEM_CONTROL_MMIO_SIZE,
            system_control_device_as_bus_device(&system_control))) {
        fputs("RunError: failed to initialize system devices\n", stderr);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    DeviceManager device_manager;
    VmLoadedDeviceModule loaded_modules[DEVICE_MANAGER_MAX_SLOTS] = {0};
    size_t loaded_module_count = 0;
    if (!device_manager_init(&device_manager,
                             &bus,
                             &vm.interrupt_router) ||
        !bus_map_device(&bus,
                        VIO_HUB_MMIO_BASE,
                        VIO_HUB_MMIO_SIZE,
                        device_manager_hub_as_bus_device(&device_manager))) {
        fputs("RunError: failed to initialize VIO device manager\n",
              stderr);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }
    device_manager_set_log_callback(&device_manager, device_log, stderr);

    MainDisplayFrontend display_frontend = {0};
    if (!main_display_init(&display_frontend, display_mode) ||
        !device_manager_register_service(
            &device_manager,
            VM_DISPLAY_SERVICE_NAME,
            VM_DISPLAY_HOST_VERSION,
            main_display_host(&display_frontend)) ||
        !device_manager_register_service(
            &device_manager,
            VM_KEYBOARD_SERVICE_NAME,
            VM_KEYBOARD_HOST_VERSION,
            main_keyboard_host(&display_frontend))) {
        fputs("RunError: failed to initialize display frontend\n",
              stderr);
        device_manager_destroy(&device_manager);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    VmDeviceDescriptor timer_descriptor = fixed_device_descriptor(
        "timer",
        VM_DEVICE_CLASS_TIMER,
        1,
        TIMER_MMIO_SIZE,
        1);
    VmDeviceResources timer_resources = fixed_device_resources(
        TIMER_MMIO_BASE,
        TIMER_MMIO_SIZE,
        1,
        TIMER_INTERRUPT_LINE);
    VmDeviceDescriptor uart_descriptor = fixed_device_descriptor(
        "uart",
        VM_DEVICE_CLASS_SERIAL,
        2,
        UART_MMIO_SIZE,
        1);
    VmDeviceResources uart_resources = fixed_device_resources(
        UART_MMIO_BASE,
        UART_MMIO_SIZE,
        1,
        UART_INTERRUPT_LINE);
    VmDeviceDescriptor core_descriptor = fixed_device_descriptor(
        "core-control",
        VM_DEVICE_CLASS_SYSTEM,
        3,
        CORE_CONTROL_MMIO_SIZE,
        0);
    VmDeviceResources core_resources = fixed_device_resources(
        CORE_CONTROL_MMIO_BASE,
        CORE_CONTROL_MMIO_SIZE,
        0,
        0);
    VmDeviceDescriptor irq_controller_descriptor =
        fixed_device_descriptor("irq-controller",
                                VM_DEVICE_CLASS_INTERRUPT_CONTROLLER,
                                6,
                                IRQ_CONTROLLER_MMIO_SIZE,
                                0);
    irq_controller_descriptor.features =
        IRQ_CONTROLLER_FEATURE_MASKING |
        IRQ_CONTROLLER_FEATURE_ROUTING |
        IRQ_CONTROLLER_FEATURE_EOI |
        IRQ_CONTROLLER_FEATURE_FIXED_PRIORITY |
        IRQ_CONTROLLER_FEATURE_NO_REENTRY;
    VmDeviceResources irq_controller_resources = fixed_device_resources(
        IRQ_CONTROLLER_MMIO_BASE,
        IRQ_CONTROLLER_MMIO_SIZE,
        0,
        0);
    VmDeviceDescriptor system_info_descriptor = fixed_device_descriptor(
        "system-information",
        VM_DEVICE_CLASS_SYSTEM,
        4,
        SYSTEM_INFO_MMIO_SIZE,
        0);
    VmDeviceResources system_info_resources = fixed_device_resources(
        SYSTEM_INFO_MMIO_BASE,
        SYSTEM_INFO_MMIO_SIZE,
        0,
        0);
    system_info_descriptor.features = system_info.features;
    VmDeviceDescriptor system_control_descriptor =
        fixed_device_descriptor("system-control",
                                VM_DEVICE_CLASS_SYSTEM,
                                5,
                                SYSTEM_CONTROL_MMIO_SIZE,
                                0);
    VmDeviceResources system_control_resources = fixed_device_resources(
        SYSTEM_CONTROL_MMIO_BASE,
        SYSTEM_CONTROL_MMIO_SIZE,
        0,
        0);
    system_control_descriptor.features =
        SYSTEM_CONTROL_FEATURE_SHUTDOWN |
        SYSTEM_CONTROL_FEATURE_WARM_RESET;
    if (!device_manager_publish_fixed(&device_manager,
                                      &timer_descriptor,
                                      &timer_resources,
                                      NULL) ||
        !device_manager_publish_fixed(&device_manager,
                                      &uart_descriptor,
                                      &uart_resources,
                                      NULL) ||
        !device_manager_publish_fixed(&device_manager,
                                      &irq_controller_descriptor,
                                      &irq_controller_resources,
                                      NULL) ||
        !device_manager_publish_fixed(&device_manager,
                                      &core_descriptor,
                                      &core_resources,
                                      NULL) ||
        !device_manager_publish_fixed(&device_manager,
                                      &system_info_descriptor,
                                      &system_info_resources,
                                      NULL) ||
        !device_manager_publish_fixed(&device_manager,
                                      &system_control_descriptor,
                                      &system_control_resources,
                                      NULL)) {
        fputs("RunError: failed to publish built-in VIO devices\n",
              stderr);
        device_manager_destroy(&device_manager);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    VmDiscoveredDeviceModule *discovered_modules =
        calloc(DEVICE_MANAGER_MAX_SLOTS,
               sizeof(VmDiscoveredDeviceModule));
    size_t discovered_count = 0;
    char discovery_error[256];
    if (discovered_modules == NULL ||
        !vm_device_module_discover(argv[0],
                                   "modules",
                                   discovered_modules,
                                   DEVICE_MANAGER_MAX_SLOTS,
                                   &discovered_count,
                                   discovery_error,
                                   sizeof(discovery_error))) {
        fprintf(stderr,
                "RunError: failed to discover device modules: %s\n",
                discovered_modules != NULL
                    ? discovery_error
                    : "out of memory");
        free(discovered_modules);
        device_manager_destroy(&device_manager);
        unload_device_modules(loaded_modules, loaded_module_count);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < discovered_count; ++i) {
        if (!load_and_attach_device(
                &device_manager,
                discovered_modules[i].path,
                discovered_modules[i].configuration,
                loaded_modules,
                DEVICE_MANAGER_MAX_SLOTS,
                &loaded_module_count)) {
            free(discovered_modules);
            device_manager_destroy(&device_manager);
            unload_device_modules(loaded_modules,
                                  loaded_module_count);
            main_display_destroy(&display_frontend);
            vm_destroy(&vm);
            free(ram.data);
            return EXIT_FAILURE;
        }
    }
    free(discovered_modules);

    for (size_t i = 0; i < device_path_count; ++i) {
        if (!load_and_attach_device(
                &device_manager,
                device_paths[i],
                device_configurations[i] != NULL
                    ? device_configurations[i]
                    : "",
                loaded_modules,
                DEVICE_MANAGER_MAX_SLOTS,
                &loaded_module_count)) {
            device_manager_destroy(&device_manager);
            unload_device_modules(loaded_modules,
                                  loaded_module_count);
            main_display_destroy(&display_frontend);
            vm_destroy(&vm);
            free(ram.data);
            return EXIT_FAILURE;
        }
    }

    HardwareThread *boot_thread = vm_hardware_thread(&vm, 0, 0);
    if (boot_thread == NULL) {
        fputs("RunError: failed to find boot hardware thread\n", stderr);
        device_manager_destroy(&device_manager);
        unload_device_modules(loaded_modules, loaded_module_count);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }
    if (!vm_set_reset_vector(&vm, reset_vector)) {
        fputs("RunError: invalid reset vector\n", stderr);
        device_manager_destroy(&device_manager);
        unload_device_modules(loaded_modules, loaded_module_count);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }

    puts("MMU: OFF (guest must configure PTBR and execute MMUON)");
    printf("Cores: %zu, hardware threads/core: %zu\n",
           core_count,
           threads_per_core);
    printf("Timer MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)TIMER_MMIO_BASE,
           (unsigned long long)(TIMER_MMIO_BASE + TIMER_MMIO_SIZE - 1));
    printf("Core Control MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)CORE_CONTROL_MMIO_BASE,
           (unsigned long long)(CORE_CONTROL_MMIO_BASE +
                                CORE_CONTROL_MMIO_SIZE - 1));
    printf("UART MMIO: 0x%llX - 0x%llX (IRQ %u)\n",
           (unsigned long long)UART_MMIO_BASE,
           (unsigned long long)(UART_MMIO_BASE + UART_MMIO_SIZE - 1),
           UART_INTERRUPT_LINE);
    printf("VIO Hub MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)VIO_HUB_MMIO_BASE,
           (unsigned long long)(VIO_HUB_MMIO_BASE +
                                VIO_HUB_MMIO_SIZE - 1));
    printf("IRQ Controller MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)IRQ_CONTROLLER_MMIO_BASE,
           (unsigned long long)(IRQ_CONTROLLER_MMIO_BASE +
                                IRQ_CONTROLLER_MMIO_SIZE - 1));
    printf("System Information MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)SYSTEM_INFO_MMIO_BASE,
           (unsigned long long)(SYSTEM_INFO_MMIO_BASE +
                                SYSTEM_INFO_MMIO_SIZE - 1));
    printf("System Control MMIO: 0x%llX - 0x%llX\n",
           (unsigned long long)SYSTEM_CONTROL_MMIO_BASE,
           (unsigned long long)(SYSTEM_CONTROL_MMIO_BASE +
                                SYSTEM_CONTROL_MMIO_SIZE - 1));

    int vm_succeeded = vm_run(&vm);
    VmSystemAction final_action = vm_system_action(&vm);
    (void)uart_device_flush_tx(&uart);
    size_t displayed_bytes;
    uint32_t displayed_width;
    uint32_t displayed_height;
    uint64_t displayed_frame;
    uint64_t dropped_frames;
    if (main_display_frame_info(&display_frontend,
                                &displayed_bytes,
                                &displayed_width,
                                &displayed_height,
                                &displayed_frame,
                                &dropped_frames) &&
        displayed_frame != 0) {
        printf("%s display: %ux%u, frame %llu, %zu bytes, "
               "dropped %llu\n",
               display_mode == MAIN_DISPLAY_WINDOW ? "Window" : "Headless",
               displayed_width,
               displayed_height,
               (unsigned long long)displayed_frame,
               displayed_bytes,
               (unsigned long long)dropped_frames);
        if (display_mode == MAIN_DISPLAY_WINDOW && vm_succeeded &&
            final_action != VM_SYSTEM_ACTION_SHUTDOWN) {
            puts("VM halted; close the display window to exit.");
            if (!window_display_wait_until_closed(
                    &display_frontend.window)) {
                fputs("RunError: display window thread failed\n", stderr);
                vm_succeeded = 0;
            }
        }
    }
    if (!vm_succeeded) {
        fputs("RunError: virtual machine execution failed\n", stderr);
        device_manager_destroy(&device_manager);
        unload_device_modules(loaded_modules, loaded_module_count);
        main_display_destroy(&display_frontend);
        vm_destroy(&vm);
        free(ram.data);
        return EXIT_FAILURE;
    }
    if (final_action == VM_SYSTEM_ACTION_SHUTDOWN) {
        puts("System shutdown requested.");
    }
    if (system_control.reset_count != 0) {
        printf("Warm resets: %llu\n",
               (unsigned long long)system_control.reset_count);
    }

    device_manager_destroy(&device_manager);
    unload_device_modules(loaded_modules, loaded_module_count);
    main_display_destroy(&display_frontend);
    vm_destroy(&vm);
    /* RAM 사용이 끝나면 할당한 메모리를 반환한다. */
    free(ram.data);
    return EXIT_SUCCESS;
}
