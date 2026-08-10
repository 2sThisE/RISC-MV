#include "preprocessor.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREPROCESS_MAX_SIZE ((size_t)16 * 1024 * 1024)
#define PREPROCESS_MAX_INCLUDE_DEPTH 16
#define PREPROCESS_MAX_MACRO_DEPTH 32
#define PREPROCESS_MAX_PARAMETERS 16

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} TextBuffer;

typedef struct {
    char *name;
    char *parameters[PREPROCESS_MAX_PARAMETERS];
    size_t parameter_count;
    char *body;
} Macro;

typedef struct {
    Macro *macros;
    size_t macro_count;
    size_t macro_capacity;
    uint64_t expansion_id;
    AssemblyError *error;
} MacroContext;

static int set_error(AssemblyError *error, size_t line, size_t column,
                     const char *message)
{
    if (error != NULL) {
        error->line = line;
        error->column = column;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return 0;
}

static char *duplicate_range(const char *text, size_t size)
{
    char *copy = malloc(size + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, size);
    copy[size] = '\0';
    return copy;
}

static int append(TextBuffer *buffer, const char *text, size_t size)
{
    if (size > PREPROCESS_MAX_SIZE - buffer->size) return 0;
    size_t required = buffer->size + size + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (capacity < required) {
            if (capacity > PREPROCESS_MAX_SIZE / 2) {
                capacity = PREPROCESS_MAX_SIZE + 1;
                break;
            }
            capacity *= 2;
        }
        if (capacity > PREPROCESS_MAX_SIZE + 1) return 0;
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int word_equal(const char *text, size_t size, const char *expected)
{
    if (strlen(expected) != size) return 0;
    for (size_t i = 0; i < size; ++i) {
        if (tolower((unsigned char)text[i]) !=
            tolower((unsigned char)expected[i])) return 0;
    }
    return 1;
}

static const char *skip_space(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                            *cursor == '\r')) ++cursor;
    return cursor;
}

static const char *word_end(const char *cursor, const char *end)
{
    while (cursor < end && (isalnum((unsigned char)*cursor) ||
                            *cursor == '_' || *cursor == '.' ||
                            *cursor == '$')) ++cursor;
    return cursor;
}

static int is_absolute_path(const char *path)
{
    return path[0] == '/' || path[0] == '\\' ||
           (isalpha((unsigned char)path[0]) && path[1] == ':');
}

static char *join_include_path(const char *parent, const char *child)
{
    if (is_absolute_path(child)) return duplicate_range(child, strlen(child));
    const char *slash = strrchr(parent, '/');
    const char *backslash = strrchr(parent, '\\');
    const char *separator = slash;
    if (backslash != NULL &&
        (separator == NULL || backslash > separator)) separator = backslash;
    size_t directory = separator != NULL ? (size_t)(separator - parent + 1) : 0;
    size_t child_size = strlen(child);
    if (directory > SIZE_MAX - child_size - 1) return NULL;
    char *path = malloc(directory + child_size + 1);
    if (path == NULL) return NULL;
    memcpy(path, parent, directory);
    memcpy(path + directory, child, child_size + 1);
    return path;
}

static int load_includes(const char *path, TextBuffer *output,
                         const char *active[PREPROCESS_MAX_INCLUDE_DEPTH],
                         size_t depth, AssemblyError *error)
{
    if (depth == PREPROCESS_MAX_INCLUDE_DEPTH)
        return set_error(error, 1, 1, "maximum .include depth exceeded");
    for (size_t i = 0; i < depth; ++i) {
        if (strcmp(active[i], path) == 0)
            return set_error(error, 1, 1, "recursive .include detected");
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return set_error(error, 1, 1, "cannot open assembly source or include");
    }
    long measured = ftell(file);
    if (measured < 0 || (unsigned long)measured > PREPROCESS_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return set_error(error, 1, 1, "assembly source is too large");
    }
    size_t size = (size_t)measured;
    char *source = malloc(size + 1);
    if (source == NULL || (size != 0 && fread(source, 1, size, file) != size) ||
        fclose(file) != 0) {
        free(source);
        return set_error(error, 1, 1, "cannot read assembly source");
    }
    source[size] = '\0';
    active[depth] = path;
    const char *cursor = source;
    size_t line = 1;
    int okay = 1;
    while (*cursor != '\0' && okay) {
        const char *end = strchr(cursor, '\n');
        if (end == NULL) end = cursor + strlen(cursor);
        const char *start = skip_space(cursor, end);
        const char *directive_end = word_end(start, end);
        if (word_equal(start, (size_t)(directive_end - start), ".include")) {
            const char *argument = skip_space(directive_end, end);
            if (argument == end || *argument != '"') {
                okay = set_error(error, line,
                                 (size_t)(argument - cursor + 1),
                                 ".include requires a quoted path");
            } else {
                const char *close = ++argument;
                while (close < end && *close != '"') ++close;
                const char *tail = close < end ? skip_space(close + 1, end) : end;
                if (close == end || (tail < end && *tail != ';')) {
                    okay = set_error(error, line, 1,
                                     "invalid .include syntax");
                } else {
                    char *child = duplicate_range(argument,
                                                   (size_t)(close - argument));
                    char *joined = child != NULL
                                       ? join_include_path(path, child) : NULL;
                    if (joined == NULL) okay = set_error(error, line, 1,
                                                         "cannot allocate include path");
                    else okay = load_includes(joined, output, active, depth + 1,
                                              error);
                    free(joined);
                    free(child);
                }
            }
        } else if (!append(output, cursor, (size_t)(end - cursor)) ||
                   !append(output, "\n", 1)) {
            okay = set_error(error, line, 1,
                             "preprocessed source exceeds size limit");
        }
        if (*end == '\0') break;
        cursor = end + 1;
        ++line;
    }
    free(source);
    return okay;
}

static void macro_destroy(Macro *macro)
{
    free(macro->name);
    for (size_t i = 0; i < macro->parameter_count; ++i)
        free(macro->parameters[i]);
    free(macro->body);
    *macro = (Macro){0};
}

static Macro *find_macro(MacroContext *context, const char *name, size_t size)
{
    for (size_t i = 0; i < context->macro_count; ++i) {
        if (strlen(context->macros[i].name) == size &&
            memcmp(context->macros[i].name, name, size) == 0)
            return &context->macros[i];
    }
    return NULL;
}

static int add_macro(MacroContext *context, Macro *macro, size_t line)
{
    if (find_macro(context, macro->name, strlen(macro->name)) != NULL)
        return set_error(context->error, line, 1, "duplicate macro definition");
    if (context->macro_count == context->macro_capacity) {
        size_t capacity = context->macro_capacity == 0
                              ? 16 : context->macro_capacity * 2;
        Macro *grown = realloc(context->macros, capacity * sizeof(*grown));
        if (grown == NULL)
            return set_error(context->error, line, 1,
                             "cannot allocate macro table");
        context->macros = grown;
        context->macro_capacity = capacity;
    }
    context->macros[context->macro_count++] = *macro;
    *macro = (Macro){0};
    return 1;
}

static int parse_macro_header(const char *cursor, const char *end,
                              Macro *macro, AssemblyError *error, size_t line)
{
    cursor = skip_space(cursor, end);
    const char *name_end = word_end(cursor, end);
    if (name_end == cursor)
        return set_error(error, line, 1, ".macro requires a name");
    macro->name = duplicate_range(cursor, (size_t)(name_end - cursor));
    if (macro->name == NULL)
        return set_error(error, line, 1, "cannot allocate macro name");
    cursor = name_end;
    while ((cursor = skip_space(cursor, end)) < end && *cursor != ';') {
        if (*cursor == ',') {
            ++cursor;
            continue;
        }
        const char *parameter_end = word_end(cursor, end);
        if (parameter_end == cursor ||
            macro->parameter_count == PREPROCESS_MAX_PARAMETERS)
            return set_error(error, line, 1,
                             "invalid or excessive macro parameter");
        macro->parameters[macro->parameter_count] =
            duplicate_range(cursor, (size_t)(parameter_end - cursor));
        if (macro->parameters[macro->parameter_count] == NULL)
            return set_error(error, line, 1,
                             "cannot allocate macro parameter");
        ++macro->parameter_count;
        cursor = parameter_end;
    }
    return 1;
}

static int parameter_index(const Macro *macro, const char *name, size_t size)
{
    for (size_t i = 0; i < macro->parameter_count; ++i) {
        if (strlen(macro->parameters[i]) == size &&
            memcmp(macro->parameters[i], name, size) == 0) return (int)i;
    }
    return -1;
}

static int split_arguments(const char *cursor, const char *end,
                           char *arguments[PREPROCESS_MAX_PARAMETERS],
                           size_t *count)
{
    *count = 0;
    cursor = skip_space(cursor, end);
    if (cursor == end || *cursor == ';') return 1;
    while (cursor < end && *cursor != ';') {
        const char *start = cursor;
        int quoted = 0, escaped = 0, parentheses = 0;
        while (cursor < end) {
            char value = *cursor;
            if (quoted) {
                if (escaped) escaped = 0;
                else if (value == '\\') escaped = 1;
                else if (value == '"') quoted = 0;
            } else if (value == '"') quoted = 1;
            else if (value == '(') ++parentheses;
            else if (value == ')' && parentheses > 0) --parentheses;
            else if ((value == ',' && parentheses == 0) || value == ';') break;
            ++cursor;
        }
        const char *trimmed_end = cursor;
        while (trimmed_end > start && isspace((unsigned char)trimmed_end[-1]))
            --trimmed_end;
        start = skip_space(start, trimmed_end);
        if (*count == PREPROCESS_MAX_PARAMETERS || start == trimmed_end)
            return 0;
        arguments[*count] = duplicate_range(start,
                                             (size_t)(trimmed_end - start));
        if (arguments[(*count)++] == NULL) return 0;
        if (cursor == end || *cursor == ';') break;
        ++cursor;
        cursor = skip_space(cursor, end);
    }
    return 1;
}

static int substitute_macro(const Macro *macro, char *const *arguments,
                            uint64_t id, TextBuffer *output)
{
    const char *cursor = macro->body;
    while (*cursor != '\0') {
        if (*cursor != '\\') {
            if (!append(output, cursor++, 1)) return 0;
            continue;
        }
        ++cursor;
        if (*cursor == '@') {
            char number[32];
            int size = snprintf(number, sizeof(number), "%" PRIu64, id);
            if (size < 0 || (size_t)size >= sizeof(number) ||
                !append(output, number, (size_t)size)) return 0;
            ++cursor;
            continue;
        }
        const char *end = word_end(cursor, cursor + strlen(cursor));
        int index = parameter_index(macro, cursor, (size_t)(end - cursor));
        if (index < 0) {
            if (!append(output, "\\", 1)) return 0;
        } else {
            if (!append(output, arguments[(size_t)index],
                        strlen(arguments[(size_t)index]))) return 0;
            cursor = end;
        }
    }
    return 1;
}

static int expand_macros(MacroContext *context, const char *source,
                         TextBuffer *output, unsigned int depth)
{
    if (depth == PREPROCESS_MAX_MACRO_DEPTH)
        return set_error(context->error, 1, 1,
                         "maximum macro expansion depth exceeded");
    const char *cursor = source;
    size_t line = 1;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        if (end == NULL) end = cursor + strlen(cursor);
        const char *start = skip_space(cursor, end);
        const char *first_end = word_end(start, end);
        if (word_equal(start, (size_t)(first_end - start), ".macro")) {
            Macro macro = {0};
            if (!parse_macro_header(first_end, end, &macro, context->error,
                                    line)) {
                macro_destroy(&macro);
                return 0;
            }
            TextBuffer body = {0};
            int found_end = 0;
            while (*end != '\0') {
                cursor = end + 1;
                ++line;
                end = strchr(cursor, '\n');
                if (end == NULL) end = cursor + strlen(cursor);
                const char *body_start = skip_space(cursor, end);
                const char *body_word = word_end(body_start, end);
                if (word_equal(body_start, (size_t)(body_word - body_start),
                               ".endm")) {
                    found_end = 1;
                    break;
                }
                if (word_equal(body_start, (size_t)(body_word - body_start),
                               ".macro")) {
                    macro_destroy(&macro);
                    free(body.data);
                    return set_error(context->error, line, 1,
                                     "nested .macro definitions are not allowed");
                }
                if (!append(&body, cursor, (size_t)(end - cursor)) ||
                    !append(&body, "\n", 1)) {
                    macro_destroy(&macro);
                    free(body.data);
                    return set_error(context->error, line, 1,
                                     "macro body exceeds size limit");
                }
            }
            if (!found_end) {
                macro_destroy(&macro);
                free(body.data);
                return set_error(context->error, line, 1,
                                 "unterminated .macro definition");
            }
            macro.body = body.data != NULL ? body.data : duplicate_range("", 0);
            if (macro.body == NULL || !add_macro(context, &macro, line)) {
                macro_destroy(&macro);
                return 0;
            }
        } else if (word_equal(start, (size_t)(first_end - start), ".endm")) {
            return set_error(context->error, line, 1, "unexpected .endm");
        } else {
            Macro *macro = find_macro(context, start,
                                      (size_t)(first_end - start));
            if (macro == NULL) {
                if (!append(output, cursor, (size_t)(end - cursor)) ||
                    !append(output, "\n", 1))
                    return set_error(context->error, line, 1,
                                     "expanded source exceeds size limit");
            } else {
                char *arguments[PREPROCESS_MAX_PARAMETERS] = {0};
                size_t argument_count = 0;
                if (!split_arguments(first_end, end, arguments,
                                     &argument_count) ||
                    argument_count != macro->parameter_count) {
                    for (size_t i = 0; i < argument_count; ++i)
                        free(arguments[i]);
                    return set_error(context->error, line, 1,
                                     "macro argument count mismatch");
                }
                TextBuffer expanded = {0};
                int okay = substitute_macro(macro, arguments,
                                             context->expansion_id++, &expanded);
                for (size_t i = 0; i < argument_count; ++i) free(arguments[i]);
                if (!okay || !expand_macros(context,
                                            expanded.data != NULL
                                                ? expanded.data : "",
                                            output, depth + 1)) {
                    free(expanded.data);
                    if (context->error != NULL &&
                        context->error->message[0] == '\0')
                        set_error(context->error, line, 1,
                                  "cannot expand macro");
                    return 0;
                }
                free(expanded.data);
            }
        }
        if (*end == '\0') break;
        cursor = end + 1;
        ++line;
    }
    return 1;
}

int asm_preprocess_file(const char *path, char **output, AssemblyError *error)
{
    if (output == NULL || path == NULL)
        return set_error(error, 1, 1, "invalid preprocessor input");
    *output = NULL;
    if (error != NULL) *error = (AssemblyError){0};
    TextBuffer included = {0};
    const char *active[PREPROCESS_MAX_INCLUDE_DEPTH] = {0};
    if (!load_includes(path, &included, active, 0, error)) {
        free(included.data);
        return 0;
    }
    MacroContext context = {.error = error};
    TextBuffer expanded = {0};
    int okay = expand_macros(&context,
                             included.data != NULL ? included.data : "",
                             &expanded, 0);
    for (size_t i = 0; i < context.macro_count; ++i)
        macro_destroy(&context.macros[i]);
    free(context.macros);
    free(included.data);
    if (!okay) {
        free(expanded.data);
        return 0;
    }
    if (expanded.data == NULL) expanded.data = duplicate_range("", 0);
    if (expanded.data == NULL)
        return set_error(error, 1, 1, "cannot allocate preprocessed source");
    *output = expanded.data;
    return 1;
}
