#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "module_loader.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void set_error(char *error,
                      size_t error_capacity,
                      const char *message)
{
    if (error == NULL || error_capacity == 0) {
        return;
    }
    (void)snprintf(error,
                   error_capacity,
                   "%s",
                   message != NULL ? message : "unknown module error");
}

static int path_join(const char *directory,
                     const char *name,
                     char *path,
                     size_t path_capacity)
{
    if (directory == NULL || name == NULL || path == NULL ||
        path_capacity == 0) {
        return 0;
    }
    size_t directory_length = strlen(directory);
    int needs_separator = directory_length != 0 &&
                          directory[directory_length - 1] != '/' &&
                          directory[directory_length - 1] != '\\';
#ifdef _WIN32
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    int written;
    if (needs_separator) {
        written = snprintf(path,
                           path_capacity,
                           "%s%c%s",
                           directory,
                           separator,
                           name);
    } else {
        written = snprintf(path,
                           path_capacity,
                           "%s%s",
                           directory,
                           name);
    }
    return written >= 0 && (size_t)written < path_capacity;
}

static int executable_directory(const char *argv0,
                                char *directory,
                                size_t directory_capacity)
{
    if (directory == NULL || directory_capacity < 2) {
        return 0;
    }

#ifdef _WIN32
    (void)argv0;
    DWORD length = GetModuleFileNameA(NULL,
                                      directory,
                                      (DWORD)directory_capacity);
    if (length == 0 || (size_t)length >= directory_capacity) {
        return 0;
    }
#else
    ssize_t length = readlink("/proc/self/exe",
                              directory,
                              directory_capacity - 1);
    if (length < 0) {
        if (argv0 == NULL || *argv0 == '\0') {
            return 0;
        }
        if (argv0[0] == '/') {
            int written = snprintf(directory,
                                   directory_capacity,
                                   "%s",
                                   argv0);
            if (written < 0 || (size_t)written >= directory_capacity) {
                return 0;
            }
            length = written;
        } else {
            char current[VM_DEVICE_MODULE_PATH_CAPACITY];
            if (getcwd(current, sizeof(current)) == NULL ||
                !path_join(current,
                           argv0,
                           directory,
                           directory_capacity)) {
                return 0;
            }
            length = (ssize_t)strlen(directory);
        }
    } else {
        directory[length] = '\0';
    }
#endif

    char *separator = strrchr(directory, '/');
#ifdef _WIN32
    char *backslash = strrchr(directory, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
#endif
    if (separator == NULL) {
        directory[0] = '.';
        directory[1] = '\0';
    } else if (separator == directory) {
        separator[1] = '\0';
    } else {
        *separator = '\0';
    }
    return 1;
}

static int ascii_equal_ignore_case(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int module_file_name(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension == NULL) {
        return 0;
    }
#ifdef _WIN32
    return ascii_equal_ignore_case(extension, ".dll");
#else
    return ascii_equal_ignore_case(extension, ".so") ||
           ascii_equal_ignore_case(extension, ".dylib") ||
           ascii_equal_ignore_case(extension, ".dll");
#endif
}

static int configuration_path(const char *module_path,
                              char *path,
                              size_t path_capacity)
{
    const char *extension = strrchr(module_path, '.');
    if (extension == NULL) {
        return 0;
    }
    size_t prefix = (size_t)(extension - module_path);
    int written = snprintf(path,
                           path_capacity,
                           "%.*s.conf",
                           (int)prefix,
                           module_path);
    return written >= 0 && (size_t)written < path_capacity;
}

static int load_configuration(const char *module_path,
                              char *configuration,
                              size_t configuration_capacity,
                              char *error,
                              size_t error_capacity)
{
    char path[VM_DEVICE_MODULE_PATH_CAPACITY];
    if (!configuration_path(module_path, path, sizeof(path))) {
        set_error(error, error_capacity, "module configuration path is too long");
        return 0;
    }

    configuration[0] = '\0';
    errno = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }
        set_error(error, error_capacity, "failed to open module configuration");
        return 0;
    }

    size_t size = fread(configuration,
                        1,
                        configuration_capacity - 1,
                        file);
    if (ferror(file)) {
        (void)fclose(file);
        set_error(error, error_capacity, "failed to read module configuration");
        return 0;
    }
    if (size == configuration_capacity - 1 && fgetc(file) != EOF) {
        (void)fclose(file);
        set_error(error, error_capacity, "module configuration is too large");
        return 0;
    }
    if (fclose(file) != 0) {
        set_error(error, error_capacity, "failed to close module configuration");
        return 0;
    }

    configuration[size] = '\0';
    while (size != 0 && isspace((unsigned char)configuration[size - 1])) {
        configuration[--size] = '\0';
    }
    size_t start = 0;
    while (configuration[start] != '\0' &&
           isspace((unsigned char)configuration[start])) {
        ++start;
    }
    if (start != 0) {
        memmove(configuration,
                configuration + start,
                strlen(configuration + start) + 1);
    }
    return 1;
}

static int compare_discovered_modules(const void *left, const void *right)
{
    const VmDiscoveredDeviceModule *first = left;
    const VmDiscoveredDeviceModule *second = right;
    return strcmp(first->path, second->path);
}

int vm_device_module_discover(const char *argv0,
                              const char *directory_name,
                              VmDiscoveredDeviceModule *modules,
                              size_t capacity,
                              size_t *count,
                              char *error,
                              size_t error_capacity)
{
    if (directory_name == NULL || *directory_name == '\0' ||
        modules == NULL || capacity == 0 || count == NULL) {
        set_error(error, error_capacity, "invalid module discovery arguments");
        return 0;
    }
    *count = 0;

    char executable_dir[VM_DEVICE_MODULE_PATH_CAPACITY];
    char module_dir[VM_DEVICE_MODULE_PATH_CAPACITY];
    if (!executable_directory(argv0,
                              executable_dir,
                              sizeof(executable_dir)) ||
        !path_join(executable_dir,
                   directory_name,
                   module_dir,
                   sizeof(module_dir))) {
        set_error(error, error_capacity, "failed to resolve module directory");
        return 0;
    }

#ifdef _WIN32
    char pattern[VM_DEVICE_MODULE_PATH_CAPACITY];
    if (!path_join(module_dir, "*", pattern, sizeof(pattern))) {
        set_error(error, error_capacity, "module search path is too long");
        return 0;
    }
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return 1;
        }
        set_error(error, error_capacity, "failed to enumerate module directory");
        return 0;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !module_file_name(entry.cFileName)) {
            continue;
        }
        if (*count >= capacity ||
            !path_join(module_dir,
                       entry.cFileName,
                       modules[*count].path,
                       sizeof(modules[*count].path))) {
            (void)FindClose(search);
            set_error(error, error_capacity, "too many modules or path too long");
            return 0;
        }
        ++*count;
    } while (FindNextFileA(search, &entry));
    DWORD enumeration_error = GetLastError();
    (void)FindClose(search);
    if (enumeration_error != ERROR_NO_MORE_FILES) {
        set_error(error, error_capacity, "failed while enumerating modules");
        return 0;
    }
#else
    DIR *directory = opendir(module_dir);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return 1;
        }
        set_error(error, error_capacity, "failed to enumerate module directory");
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!module_file_name(entry->d_name)) {
            continue;
        }
        if (*count >= capacity ||
            !path_join(module_dir,
                       entry->d_name,
                       modules[*count].path,
                       sizeof(modules[*count].path))) {
            (void)closedir(directory);
            set_error(error, error_capacity, "too many modules or path too long");
            return 0;
        }
        struct stat status;
        if (stat(modules[*count].path, &status) == 0 &&
            S_ISREG(status.st_mode)) {
            ++*count;
        }
    }
    if (closedir(directory) != 0) {
        set_error(error, error_capacity, "failed to close module directory");
        return 0;
    }
#endif

    qsort(modules,
          *count,
          sizeof(VmDiscoveredDeviceModule),
          compare_discovered_modules);
    for (size_t i = 0; i < *count; ++i) {
        if (!load_configuration(modules[i].path,
                                modules[i].configuration,
                                sizeof(modules[i].configuration),
                                error,
                                error_capacity)) {
            return 0;
        }
    }
    return 1;
}

int vm_device_module_load(const char *path,
                          VmLoadedDeviceModule *loaded,
                          char *error,
                          size_t error_capacity)
{
    if (path == NULL || loaded == NULL) {
        set_error(error, error_capacity, "invalid module load arguments");
        return 0;
    }

    *loaded = (VmLoadedDeviceModule){0};
    VmDeviceQuery query = NULL;

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (handle == NULL) {
        char message[128];
        (void)snprintf(message,
                       sizeof(message),
                       "LoadLibrary failed with error %lu",
                       (unsigned long)GetLastError());
        set_error(error, error_capacity, message);
        return 0;
    }

    FARPROC symbol = GetProcAddress(handle, VM_DEVICE_QUERY_SYMBOL);
    if (symbol == NULL) {
        set_error(error,
                  error_capacity,
                  "module does not export vm_device_query");
        (void)FreeLibrary(handle);
        return 0;
    }
    if (sizeof(query) != sizeof(symbol)) {
        set_error(error, error_capacity, "incompatible function pointer size");
        (void)FreeLibrary(handle);
        return 0;
    }
    memcpy(&query, &symbol, sizeof(query));
    loaded->native_handle = handle;
#else
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        set_error(error, error_capacity, dlerror());
        return 0;
    }

    void *symbol = dlsym(handle, VM_DEVICE_QUERY_SYMBOL);
    const char *symbol_error = dlerror();
    if (symbol_error != NULL || symbol == NULL) {
        set_error(error,
                  error_capacity,
                  symbol_error != NULL
                      ? symbol_error
                      : "module does not export vm_device_query");
        (void)dlclose(handle);
        return 0;
    }
    if (sizeof(query) != sizeof(symbol)) {
        set_error(error, error_capacity, "incompatible function pointer size");
        (void)dlclose(handle);
        return 0;
    }
    memcpy(&query, &symbol, sizeof(query));
    loaded->native_handle = handle;
#endif

    loaded->module = query(VM_DEVICE_ABI_VERSION);
    if (loaded->module == NULL) {
        set_error(error, error_capacity, "module rejected host ABI version");
        vm_device_module_unload(loaded);
        return 0;
    }
    return 1;
}

void vm_device_module_unload(VmLoadedDeviceModule *loaded)
{
    if (loaded == NULL || loaded->native_handle == NULL) {
        return;
    }

#ifdef _WIN32
    (void)FreeLibrary((HMODULE)loaded->native_handle);
#else
    (void)dlclose(loaded->native_handle);
#endif
    *loaded = (VmLoadedDeviceModule){0};
}
