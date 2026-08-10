#ifndef MAIN_OPTIONS_H
#define MAIN_OPTIONS_H

#include <stddef.h>
#include <stdio.h>

#include "device_manager.h"

typedef enum {
    MAIN_DISPLAY_HEADLESS = 0,
    MAIN_DISPLAY_WINDOW = 1
} MainDisplayMode;

typedef struct {
    size_t ram_size;
    size_t core_count;
    size_t threads_per_core;
    const char *program_path;
    const char *boot_rom_path;
    MainDisplayMode display_mode;
    const char *device_paths[DEVICE_MANAGER_MAX_SLOTS];
    const char *device_configurations[DEVICE_MANAGER_MAX_SLOTS];
    size_t device_path_count;
} MainOptions;

typedef struct {
    char message[256];
} MainOptionError;

typedef enum {
    MAIN_OPTIONS_ERROR = 0,
    MAIN_OPTIONS_OK = 1,
    MAIN_OPTIONS_HELP = 2
} MainOptionsResult;

MainOptionsResult main_options_parse(int argc,
                                     char *argv[],
                                     int window_supported,
                                     MainOptions *options,
                                     MainOptionError *error);
void main_options_print_help(FILE *stream,
                             const char *program_name,
                             int window_supported);

#endif
