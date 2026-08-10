#ifndef TEST_SUITES_H
#define TEST_SUITES_H

/* 새 suite는 구현 함수와 이 목록의 한 줄만 추가하면 runner에 등록된다. */
#define TEST_SUITE_LIST(X)                         \
    X("assembler", test_assembler)               \
    X("atomic", test_atomic)                     \
    X("block_device", test_block_device)         \
    X("boot_format", test_boot_format)           \
    X("boot_rom", test_boot_rom)                 \
    X("bus_timer", test_bus_timer)               \
    X("core_control", test_core_control)         \
    X("device_manager", test_device_manager)     \
    X("disk_image", test_disk_image)             \
    X("display", test_display)                   \
    X("display_queue", test_display_queue)       \
    X("exception", test_exception)               \
    X("firmware", test_firmware)                 \
    X("interrupt", test_interrupt)               \
    X("irq_controller", test_irq_controller)     \
    X("ipi", test_ipi)                           \
    X("isa_extension", test_isa_extension)       \
    X("keyboard", test_keyboard)                 \
    X("keyboard_guest", test_keyboard_guest)     \
    X("main_options", test_main_options)         \
    X("mmu", test_mmu)                           \
    X("multicore", test_multicore)               \
    X("object_assembler", test_object_assembler) \
    X("privilege", test_privilege)               \
    X("stack", test_stack)                       \
    X("syscall", test_syscall)                   \
    X("simd_float", test_simd_float)             \
    X("system_devices", test_system_devices)     \
    X("uart", test_uart)                         \
    X("vm_interrupt", test_vm_interrupt)         \
    X("wait_interrupt", test_wait_interrupt)

typedef int (*TestSuiteFunction)(void);

#define DECLARE_TEST_SUITE(name, function) int function(void);
TEST_SUITE_LIST(DECLARE_TEST_SUITE)
#undef DECLARE_TEST_SUITE

#endif
