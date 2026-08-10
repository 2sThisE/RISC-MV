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
        "main", "-r", "65536", "-l", "kernel.bin",
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
        "main", "--ram", "4096", "--rom", "boot.bin"
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

static void test_errors(void)
{
    char *no_arguments[] = {"main"};
    expect_error(1, no_arguments, "no boot configuration");

    char *missing_value[] = {"main", "-r"};
    expect_error(2, missing_value, "requires a value");

    char *invalid_ram[] = {"main", "-r", "4K", "-l", "a.bin"};
    expect_error(5, invalid_ram, "positive decimal");

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
    assert(strstr(text, "unavailable on this platform") != NULL);
    assert(fclose(stream) == 0);
}

int test_main_options(void)
{
    test_valid_short_options();
    test_valid_long_options_and_defaults();
    test_errors();
    test_help();
    return 0;
}
