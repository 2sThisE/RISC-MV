#include "test_suites.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    TestSuiteFunction function;
} TestSuite;

#define MAKE_TEST_SUITE(name, function) { name, function },
static const TestSuite TEST_SUITES[] = {
    TEST_SUITE_LIST(MAKE_TEST_SUITE)
};
#undef MAKE_TEST_SUITE

#define TEST_SUITE_COUNT (sizeof(TEST_SUITES) / sizeof(TEST_SUITES[0]))

static const TestSuite *find_suite(const char *name)
{
    for (size_t i = 0; i < TEST_SUITE_COUNT; ++i) {
        if (strcmp(TEST_SUITES[i].name, name) == 0) {
            return &TEST_SUITES[i];
        }
    }
    return NULL;
}

static int parse_repeat(const char *text, unsigned int *repeat)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value == 0 || value > UINT_MAX) {
        return 0;
    }

    *repeat = (unsigned int)value;
    return 1;
}

static void print_usage(const char *program)
{
    printf("Usage: %s [--list] [--run NAME] [--filter TEXT] [--repeat N]\n",
           program);
}

static int run_child(const char *program, const char *suite_name)
{
    size_t required = strlen(program) + strlen(suite_name) + 18;
    char *command = malloc(required);
    if (command == NULL) {
        fputs("TestRunnerError: cannot allocate child command\n", stderr);
        return 0;
    }

    int written = snprintf(command,
                           required,
                           "\"\"%s\" --run \"%s\"\"",
                           program,
                           suite_name);
    if (written < 0 || (size_t)written >= required) {
        free(command);
        fputs("TestRunnerError: child command is too long\n", stderr);
        return 0;
    }

    int status = system(command);
    free(command);
    return status == 0;
}

int main(int argc, char **argv)
{
    const char *direct_name = NULL;
    const char *filter = NULL;
    unsigned int repeat = 1;
    int list_only = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            direct_name = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            if (!parse_repeat(argv[++i], &repeat)) {
                fputs("TestRunnerError: --repeat must be a positive integer\n",
                      stderr);
                return 2;
            }
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (list_only) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; ++i) {
            puts(TEST_SUITES[i].name);
        }
        return 0;
    }

    if (direct_name != NULL) {
        const TestSuite *suite = find_suite(direct_name);
        if (suite == NULL) {
            fprintf(stderr, "TestRunnerError: unknown suite '%s'\n", direct_name);
            return 2;
        }
        return suite->function();
    }

    unsigned int passed = 0;
    unsigned int failed = 0;
    unsigned int selected = 0;

    for (unsigned int iteration = 1; iteration <= repeat; ++iteration) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; ++i) {
            const TestSuite *suite = &TEST_SUITES[i];
            if (filter != NULL && strstr(suite->name, filter) == NULL) {
                continue;
            }

            ++selected;
            printf("[ RUN  ] %s (%u/%u)\n",
                   suite->name,
                   iteration,
                   repeat);
            fflush(stdout);

            if (run_child(argv[0], suite->name)) {
                ++passed;
                printf("[ PASS ] %s\n", suite->name);
            } else {
                ++failed;
                printf("[ FAIL ] %s\n", suite->name);
            }
            fflush(stdout);
        }
    }

    if (selected == 0) {
        fputs("TestRunnerError: no suites matched the filter\n", stderr);
        return 2;
    }

    printf("\nTest summary: %u passed, %u failed, %u total\n",
           passed,
           failed,
           selected);
    return failed == 0 ? 0 : 1;
}
