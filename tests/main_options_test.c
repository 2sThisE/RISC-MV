#include "main_options.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static MainOptionsResult parse_arguments(size_t count,
                                         char *arguments[],
                                         int window_supported,
                                         MainOptions *options,
                                         MainOptionError *error)
{
    return main_options_parse((int)count,
                              arguments,
                              window_supported,
                              options,
                              error);
}

static void test_valid_short_options(void)
{
    char *arguments[] = {
        "main", "-r", "64k", "-l", "kernel.bin",
        "-rom", "boot.bin", "-c", "2", "-t", "3",
        "-d", "disk.dll", "-dc", "path=disk.img",
        "-display", "window"
    };
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(sizeof(arguments) / sizeof(arguments[0]),
                           arguments,
                           1,
                           &options,
                           &error) == MAIN_OPTIONS_OK);
    assert(options.ram_size == 65536);
    assert(options.core_count == 2);
    assert(options.threads_per_core == 3);
    assert(strcmp(options.program_path, "kernel.bin") == 0);
    assert(strcmp(options.boot_rom_path, "boot.bin") == 0);
    assert(options.display_mode == MAIN_DISPLAY_WINDOW);
    assert(options.device_path_count == 1);
    assert(strcmp(options.device_paths[0], "disk.dll") == 0);
    assert(strcmp(options.device_configurations[0],
                  "path=disk.img") == 0);
}

static void test_valid_long_options_and_defaults(void)
{
    char *arguments[] = {
        "main", "--ram", "4K", "--rom", "boot.bin"
    };
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(sizeof(arguments) / sizeof(arguments[0]),
                           arguments,
                           0,
                           &options,
                           &error) == MAIN_OPTIONS_OK);
    assert(options.ram_size == 4096);
    assert(options.core_count == 1);
    assert(options.threads_per_core == 1);
    assert(options.program_path == NULL);
    assert(options.display_mode == MAIN_DISPLAY_HEADLESS);
}

static void expect_error(size_t count,
                         char *arguments[],
                         const char *message_part)
{
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(count,
                           arguments,
                           1,
                           &options,
                           &error) == MAIN_OPTIONS_ERROR);
    assert(error.message[0] != '\0');
    assert(strstr(error.message, message_part) != NULL);
}

static void expect_ram_size(const char *value, size_t expected)
{
    char *arguments[] = {
        "main", "-r", (char *)value, "-l", "a.bin"
    };
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(sizeof(arguments) / sizeof(arguments[0]),
                           arguments,
                           1,
                           &options,
                           &error) == MAIN_OPTIONS_OK);
    assert(options.ram_size == expected);
}

static void test_ram_units(void)
{
    expect_ram_size("1", 1);
    expect_ram_size("1b", 1);
    expect_ram_size("1B", 1);
    expect_ram_size("1k", (size_t)1024);
    expect_ram_size("2M", (size_t)2 * 1024 * 1024);
    expect_ram_size("1g", (size_t)1024 * 1024 * 1024);
}

static void test_errors(void)
{
    char *no_arguments[] = {"main"};
    expect_error(1, no_arguments, "no boot configuration");

    char *missing_value[] = {"main", "-r"};
    expect_error(2, missing_value, "requires a value");

    char *invalid_ram[] = {"main", "-r", "4kb", "-l", "a.bin"};
    expect_error(5, invalid_ram, "B/K/M/G");

    char *fractional_ram[] = {"main", "-r", "1.5m", "-l", "a.bin"};
    expect_error(5, fractional_ram, "B/K/M/G");

    char *zero_ram[] = {"main", "-r", "0k", "-l", "a.bin"};
    expect_error(5, zero_ram, "positive integer");

    char *negative_ram[] = {"main", "-r", "-1g", "-l", "a.bin"};
    expect_error(5, negative_ram, "positive integer");

    char *overflow_ram[] = {
        "main", "-r", "18446744073709551615g", "-l", "a.bin"
    };
    expect_error(5, overflow_ram, "B/K/M/G");

    char *unit_core_count[] = {
        "main", "-r", "4k", "-l", "a.bin", "-c", "1k"
    };
    expect_error(7, unit_core_count, "expected 1-");

    char *missing_ram[] = {"main", "-l", "a.bin"};
    expect_error(3, missing_ram, "-r/--ram");

    char *missing_source[] = {"main", "-r", "4096"};
    expect_error(3, missing_source, "boot source");

    char *duplicate[] = {
        "main", "-r", "4096", "--ram", "8192", "-l", "a.bin"
    };
    expect_error(7, duplicate, "only be specified once");

    char *unknown[] = {
        "main", "-r", "4096", "-l", "a.bin", "--unknown"
    };
    expect_error(6, unknown, "unrecognized option");

    char *config_without_device[] = {
        "main", "-r", "4096", "-l", "a.bin", "-dc", "x"
    };
    expect_error(7, config_without_device, "preceding -d/--device");

    char *bad_display[] = {
        "main", "-r", "4096", "-l", "a.bin", "-display", "gui"
    };
    expect_error(7, bad_display, "headless");

    char *unsupported_window[] = {
        "main", "-r", "4096", "-l", "a.bin", "--display", "window"
    };
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(7,
                           unsupported_window,
                           0,
                           &options,
                           &error) == MAIN_OPTIONS_ERROR);
    assert(strstr(error.message, "not supported") != NULL);
}

static void test_help(void)
{
    char *arguments[] = {"main", "--help"};
    MainOptions options;
    MainOptionError error;
    assert(parse_arguments(2,
                           arguments,
                           1,
                           &options,
                           &error) == MAIN_OPTIONS_HELP);

    FILE *stream = tmpfile();
    assert(stream != NULL);
    main_options_print_help(stream, "vm", 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    char text[4096] = {0};
    size_t size = fread(text, 1, sizeof(text) - 1, stream);
    assert(!ferror(stream));
    assert(size != 0);
    assert(strstr(text, "Usage:") != NULL);
    assert(strstr(text, "--help") != NULL);
    assert(strstr(text, "B/KiB/MiB/GiB") != NULL);
    assert(strstr(text, "1k = 1024 bytes") != NULL);
    assert(strstr(text, "unavailable on this platform") != NULL);
    assert(fclose(stream) == 0);
}

int test_main_options(void)
{
    test_valid_short_options();
    test_valid_long_options_and_defaults();
    test_ram_units();
    test_errors();
    test_help();
    return 0;
}
