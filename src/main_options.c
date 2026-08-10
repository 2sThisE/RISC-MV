#include "main_options.h"

#include "ram.h"
#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static MainOptionsResult option_error(MainOptionError *error,
                                      const char *format,
                                      ...)
{
    if (error != NULL) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(error->message,
                        sizeof(error->message),
                        format,
                        arguments);
        va_end(arguments);
    }
    return MAIN_OPTIONS_ERROR;
}

static int option_is(const char *argument,
                     const char *short_name,
                     const char *long_name)
{
    return strcmp(argument, short_name) == 0 ||
           strcmp(argument, long_name) == 0;
}

static const char *option_value(int argc,
                                char *argv[],
                                int *index,
                                MainOptionError *error)
{
    if (*index + 1 >= argc) {
        (void)option_error(error,
                           "option '%s' requires a value",
                           argv[*index]);
        return NULL;
    }
    return argv[++*index];
}

MainOptionsResult main_options_parse(int argc,
                                     char *argv[],
                                     int window_supported,
                                     MainOptions *options,
                                     MainOptionError *error)
{
    if (options == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
        return option_error(error, "invalid command-line parser input");
    }
    *options = (MainOptions){
        .core_count = 1,
        .threads_per_core = 1,
        .display_mode = MAIN_DISPLAY_HEADLESS
    };
    if (error != NULL) {
        *error = (MainOptionError){0};
    }

    int ram_size_given = 0;
    int core_count_given = 0;
    int thread_count_given = 0;
    int display_mode_given = 0;

    if (argc == 1) {
        return option_error(error, "no boot configuration was provided");
    }

    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (option_is(argument, "-h", "--help")) {
            return MAIN_OPTIONS_HELP;
        }

        if (option_is(argument, "-r", "--ram")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (ram_size_given) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            if (!parse_ram_size(value, &options->ram_size)) {
                return option_error(error,
                                    "invalid RAM size '%s'; expected a positive decimal byte count",
                                    value);
            }
            ram_size_given = 1;
        } else if (option_is(argument, "-l", "--load")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (options->program_path != NULL) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            options->program_path = value;
        } else if (option_is(argument, "-rom", "--rom")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (options->boot_rom_path != NULL) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            options->boot_rom_path = value;
        } else if (option_is(argument, "-c", "--cores")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (core_count_given) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            if (!parse_ram_size(value, &options->core_count) ||
                options->core_count > VM_MAX_CORES) {
                return option_error(error,
                                    "invalid core count '%s'; expected 1-%u",
                                    value,
                                    VM_MAX_CORES);
            }
            core_count_given = 1;
        } else if (option_is(argument, "-t", "--threads")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (thread_count_given) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            if (!parse_ram_size(value, &options->threads_per_core) ||
                options->threads_per_core > VM_MAX_THREADS_PER_CORE) {
                return option_error(
                    error,
                    "invalid hardware thread count '%s'; expected 1-%u",
                    value,
                    VM_MAX_THREADS_PER_CORE);
            }
            thread_count_given = 1;
        } else if (option_is(argument, "-d", "--device")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (options->device_path_count >= DEVICE_MANAGER_MAX_SLOTS) {
                return option_error(error,
                                    "too many device modules; maximum is %u",
                                    DEVICE_MANAGER_MAX_SLOTS);
            }
            options->device_paths[options->device_path_count++] = value;
        } else if (option_is(argument, "-dc", "--device-config")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (options->device_path_count == 0) {
                return option_error(
                    error,
                    "option '%s' requires a preceding -d/--device",
                    argument);
            }
            size_t device_index = options->device_path_count - 1;
            if (options->device_configurations[device_index] != NULL) {
                return option_error(
                    error,
                    "the most recent device already has a configuration");
            }
            options->device_configurations[device_index] = value;
        } else if (option_is(argument, "-display", "--display")) {
            const char *value = option_value(argc, argv, &i, error);
            if (value == NULL) return MAIN_OPTIONS_ERROR;
            if (display_mode_given) {
                return option_error(error,
                                    "option '%s' may only be specified once",
                                    argument);
            }
            if (strcmp(value, "headless") == 0) {
                options->display_mode = MAIN_DISPLAY_HEADLESS;
            } else if (strcmp(value, "window") == 0) {
                if (!window_supported) {
                    return option_error(
                        error,
                        "display mode 'window' is not supported on this platform");
                }
                options->display_mode = MAIN_DISPLAY_WINDOW;
            } else {
                return option_error(
                    error,
                    "invalid display mode '%s'; expected 'headless' or 'window'",
                    value);
            }
            display_mode_given = 1;
        } else {
            return option_error(error,
                                "unrecognized option '%s'",
                                argument);
        }
    }

    if (!ram_size_given) {
        return option_error(error,
                            "required option '-r/--ram' was not provided");
    }
    if (options->program_path == NULL && options->boot_rom_path == NULL) {
        return option_error(
            error,
            "at least one boot source is required: '-l/--load' or '-rom/--rom'");
    }
    return MAIN_OPTIONS_OK;
}

void main_options_print_help(FILE *stream,
                             const char *program_name,
                             int window_supported)
{
    if (stream == NULL) {
        return;
    }
    const char *program = program_name != NULL ? program_name : "main";
    fprintf(stream,
            "Usage:\n"
            "  %s -r BYTES -l FILE [OPTIONS]\n"
            "  %s -r BYTES -rom FILE [OPTIONS]\n"
            "\n"
            "Run the custom 64-bit VM from a RAM image, a Boot ROM, or both.\n"
            "\n"
            "Required:\n"
            "  -r, --ram BYTES           RAM size as a positive decimal byte count\n"
            "\n"
            "Boot sources (at least one):\n"
            "  -l, --load FILE           Load a raw binary at RAM address 1\n"
            "  -rom, --rom FILE          Map a Boot ROM and reset PC to 0x7FFFF00000\n"
            "\n"
            "CPU configuration:\n"
            "  -c, --cores COUNT         Virtual cores (default 1, maximum %u)\n"
            "  -t, --threads COUNT       Hardware threads per core (default 1, maximum %u)\n"
            "\n"
            "Devices and display:\n"
            "  -d, --device FILE         Attach a device module; may be repeated\n"
            "  -dc, --device-config TEXT Configure the most recent -d/--device\n"
            "  -display, --display MODE  headless (default) or window%s\n"
            "\n"
            "General:\n"
            "  -h, --help                 Show this help and exit\n"
            "\n"
            "Examples:\n"
            "  %s -r 4096 -l program.bin\n"
            "  %s -r 4096 -rom boot_rom.bin\n"
            "  %s -r 65536 -rom boot_rom.bin -l kernel.bin -c 2 -t 2\n",
            program,
            program,
            VM_MAX_CORES,
            VM_MAX_THREADS_PER_CORE,
            window_supported ? "" : " (unavailable on this platform)",
            program,
            program,
            program);
}
