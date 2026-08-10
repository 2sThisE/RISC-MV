#include "object_assembler.h"

#include "encoder.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} TextBuffer;

typedef struct {
    char *name;
    uint16_t section;
} Definition;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} NameList;

static int fail(AssemblyError *error, size_t line, size_t column,
                const char *message)
{
    if (error != NULL) {
        error->line = line;
        error->column = column;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return 0;
}

static char *copy_text(const char *text)
{
    size_t size = strlen(text);
    char *copy = malloc(size + 1);
    if (copy != NULL) memcpy(copy, text, size + 1);
    return copy;
}

static int text_append(TextBuffer *buffer, const char *text, size_t size)
{
    if (size > SIZE_MAX - buffer->size - 1) return 0;
    size_t needed = buffer->size + size + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) return 0;
            capacity *= 2;
        }
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

static int text_line(TextBuffer *buffer, const char *line, size_t size)
{
    return text_append(buffer, line, size) && text_append(buffer, "\n", 1);
}

static int name_index(const NameList *list, const char *name)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], name) == 0) return (int)i;
    }
    return -1;
}

static int name_add(NameList *list, const char *name)
{
    if (name_index(list, name) >= 0) return 1;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        char **grown = realloc(list->items, capacity * sizeof(*grown));
        if (grown == NULL) return 0;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count] = copy_text(name);
    if (list->items[list->count] == NULL) return 0;
    ++list->count;
    return 1;
}

static void name_destroy(NameList *list)
{
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    *list = (NameList){0};
}

static int definition_index(const Definition *items, size_t count,
                            const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int section_index(const char *name)
{
    if (asm_text_equal_ignore_case(name, ".text")) return 0;
    if (asm_text_equal_ignore_case(name, ".rodata")) return 1;
    if (asm_text_equal_ignore_case(name, ".data")) return 2;
    if (asm_text_equal_ignore_case(name, ".bss")) return 3;
    return -1;
}

static int parse_name_directive(const AsmToken *tokens, size_t count,
                                NameList *names, size_t line,
                                AssemblyError *error)
{
    size_t index = 1;
    int have = 0;
    while (index < count && tokens[index].type != ASM_TOKEN_END) {
        if (tokens[index].type != ASM_TOKEN_IDENTIFIER ||
            strncmp(tokens[index].text, "__cvmlink_", 10) == 0) {
            return fail(error, line, tokens[index].column,
                        "expected non-reserved symbol name");
        }
        if (!name_add(names, tokens[index].text)) {
            return fail(error, line, tokens[index].column,
                        "cannot allocate symbol declaration");
        }
        have = 1;
        ++index;
        if (tokens[index].type == ASM_TOKEN_END) break;
        if (tokens[index].type != ASM_TOKEN_COMMA) {
            return fail(error, line, tokens[index].column,
                        "expected ',' between symbol names");
        }
        ++index;
    }
    return have ? 1 : fail(error, line, 1, "symbol directive is empty");
}

static int bss_line_valid(const AsmToken *tokens, size_t count)
{
    size_t index = 0;
    if (tokens[index].type == ASM_TOKEN_IDENTIFIER && index + 1 < count &&
        tokens[index + 1].type == ASM_TOKEN_COLON) index += 2;
    if (tokens[index].type == ASM_TOKEN_END) return 1;
    if (tokens[index].type != ASM_TOKEN_IDENTIFIER) return 0;
    return asm_text_equal_ignore_case(tokens[index].text, ".space") ||
           asm_text_equal_ignore_case(tokens[index].text, ".align") ||
           asm_text_equal_ignore_case(tokens[index].text, ".equ");
}

static int append_validation_source(TextBuffer *validation,
                                    const TextBuffer sections[4],
                                    const NameList *externs)
{
    for (size_t i = 0; i < 4; ++i) {
        if (!text_append(validation, sections[i].data != NULL
                                      ? sections[i].data : "",
                         sections[i].size) ||
            !text_append(validation, "\n.align 16\n", 11)) return 0;
    }
    for (size_t i = 0; i < externs->count; ++i) {
        if (!text_append(validation, externs->items[i],
                         strlen(externs->items[i])) ||
            !text_append(validation, ": .byte 0\n", 10)) return 0;
    }
    return 1;
}

int assembler_assemble_object(const char *source,
                              CvmObjectFile *object,
                              AssemblyError *error)
{
    if (object == NULL || source == NULL) {
        return fail(error, 1, 1, "invalid object assembly input");
    }
    *object = (CvmObjectFile){0};
    if (error != NULL) *error = (AssemblyError){0};
    TextBuffer sections[4] = {{0}};
    NameList globals = {0}, externs = {0};
    Definition *definitions = NULL;
    size_t definition_count = 0, definition_capacity = 0;
    char *entry = NULL;
    int current_section = 0;
    const char *cursor = source;
    size_t line_number = 1;
    int okay = 1;

    while (*cursor != '\0' && okay) {
        const char *end = strchr(cursor, '\n');
        size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (length != 0 && cursor[length - 1] == '\r') --length;
        char *line = malloc(length + 1);
        AsmToken *tokens = malloc(sizeof(*tokens) * ASM_MAX_TOKENS_PER_LINE);
        size_t count = 0;
        if (line == NULL || tokens == NULL) {
            free(line); free(tokens);
            okay = fail(error, line_number, 1, "cannot allocate source line");
            break;
        }
        memcpy(line, cursor, length); line[length] = '\0';
        if (!asm_lex_line(line, line_number, tokens, &count, error)) {
            free(line); free(tokens); okay = 0; break;
        }
        if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
            asm_text_equal_ignore_case(tokens[0].text, ".section")) {
            if (tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_END ||
                (current_section = section_index(tokens[1].text)) < 0) {
                okay = fail(error, line_number, tokens[1].column,
                            "expected .text, .rodata, .data, or .bss");
            }
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".global")) {
            okay = parse_name_directive(tokens, count, &globals,
                                        line_number, error);
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".extern")) {
            okay = parse_name_directive(tokens, count, &externs,
                                        line_number, error);
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".entry")) {
            if (entry != NULL || tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_END) {
                okay = fail(error, line_number, tokens[0].column,
                            "object .entry requires one unique symbol");
            } else {
                entry = copy_text(tokens[1].text);
                if (entry == NULL) okay = fail(error, line_number, 1,
                                               "cannot allocate entry symbol");
            }
        } else {
            if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                asm_text_equal_ignore_case(tokens[0].text, ".org")) {
                okay = fail(error, line_number, tokens[0].column,
                            ".org is not allowed in relocatable objects");
            }
            if (okay && current_section == 3 && !bss_line_valid(tokens, count)) {
                okay = fail(error, line_number, 1,
                            ".bss accepts only labels, .space, .align, and .equ");
            }
            int is_label = tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                           tokens[1].type == ASM_TOKEN_COLON;
            int is_equ = tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                         (asm_text_equal_ignore_case(tokens[0].text, ".equ") ||
                          asm_text_equal_ignore_case(tokens[0].text, ".set")) &&
                         tokens[1].type == ASM_TOKEN_IDENTIFIER;
            const AsmToken *definition_token = is_label ? &tokens[0] :
                                                is_equ ? &tokens[1] : NULL;
            if (okay && definition_token != NULL) {
                if (strncmp(definition_token->text, "__cvmlink_", 10) == 0 ||
                    definition_index(definitions, definition_count,
                                     definition_token->text) >= 0) {
                    okay = fail(error, line_number, definition_token->column,
                                "duplicate or reserved label");
                } else {
                    if (definition_count == definition_capacity) {
                        size_t capacity = definition_capacity == 0
                                              ? 16 : definition_capacity * 2;
                        Definition *grown = realloc(
                            definitions, capacity * sizeof(*grown));
                        if (grown == NULL) {
                            okay = fail(error, line_number, 1,
                                        "cannot allocate label table");
                        } else {
                            definitions = grown;
                            definition_capacity = capacity;
                        }
                    }
                    if (okay) {
                        definitions[definition_count].name =
                            copy_text(definition_token->text);
                        definitions[definition_count].section =
                            (uint16_t)current_section;
                        if (definitions[definition_count].name == NULL) {
                            okay = fail(error, line_number, 1,
                                        "cannot allocate label name");
                        } else {
                            ++definition_count;
                        }
                    }
                }
            }
            if (okay && !text_line(&sections[current_section], line, length)) {
                okay = fail(error, line_number, 1,
                            "cannot allocate section source");
            }
        }
        free(line); free(tokens);
        if (end == NULL) break;
        cursor = end + 1;
        ++line_number;
    }

    for (size_t i = 0; okay && i < globals.count; ++i) {
        if (definition_index(definitions, definition_count,
                             globals.items[i]) < 0) {
            okay = fail(error, 1, 1, ".global symbol is not defined");
        }
    }
    for (size_t i = 0; okay && i < externs.count; ++i) {
        if (definition_index(definitions, definition_count,
                             externs.items[i]) >= 0) {
            okay = fail(error, 1, 1, ".extern symbol is also defined");
        }
    }
    if (okay && entry != NULL) {
        if (definition_index(definitions, definition_count, entry) < 0) {
            okay = fail(error, 1, 1, ".entry symbol is not defined");
        } else if (!name_add(&globals, entry)) {
            okay = fail(error, 1, 1, "cannot export entry symbol");
        }
    }

    TextBuffer validation = {0};
    if (okay && !append_validation_source(&validation, sections, &externs)) {
        okay = fail(error, 1, 1, "cannot build validation source");
    }
    if (okay) {
        AssemblyResult result;
        AssemblyError validation_error;
        if (!assembler_assemble(validation.data, 0, &result,
                                &validation_error)) {
            if (error != NULL) *error = validation_error;
            okay = 0;
        } else {
            assembly_result_destroy(&result);
        }
    }
    free(validation.data);

    if (okay) {
        static const char *names[4] = {".text", ".rodata", ".data", ".bss"};
        static const uint32_t flags[4] = {
            CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_EXECUTE,
            CVM_OBJECT_SECTION_ALLOC,
            CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_WRITE,
            CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_WRITE |
                CVM_OBJECT_SECTION_NOBITS
        };
        object->sections = calloc(4, sizeof(*object->sections));
        object->symbols = calloc(definition_count + externs.count,
                                 sizeof(*object->symbols));
        if (object->sections == NULL ||
            (definition_count + externs.count != 0 && object->symbols == NULL)) {
            okay = fail(error, 1, 1, "cannot allocate object result");
        } else {
            object->section_count = 4;
            object->symbol_count = definition_count + externs.count;
            for (size_t i = 0; i < 4 && okay; ++i) {
                object->sections[i].name = copy_text(names[i]);
                object->sections[i].flags = flags[i];
                object->sections[i].alignment = 4096;
                object->sections[i].source = sections[i].data != NULL
                                                  ? sections[i].data
                                                  : copy_text("");
                object->sections[i].source_size = sections[i].size;
                sections[i].data = NULL;
                if (object->sections[i].name == NULL ||
                    object->sections[i].source == NULL) okay = 0;
            }
            for (size_t i = 0; i < definition_count && okay; ++i) {
                CvmObjectSymbol *symbol = &object->symbols[i];
                symbol->name = copy_text(definitions[i].name);
                symbol->section_index = definitions[i].section;
                symbol->flags = CVM_OBJECT_SYMBOL_DEFINED;
                if (symbol->name == NULL) {
                    okay = 0;
                    continue;
                }
                if (name_index(&globals, symbol->name) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_GLOBAL;
                if (entry != NULL && strcmp(entry, symbol->name) == 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_ENTRY;
            }
            for (size_t i = 0; i < externs.count && okay; ++i) {
                CvmObjectSymbol *symbol =
                    &object->symbols[definition_count + i];
                symbol->name = copy_text(externs.items[i]);
                symbol->section_index = CVM_OBJECT_UNDEFINED_SECTION;
                symbol->flags = CVM_OBJECT_SYMBOL_GLOBAL;
                if (symbol->name == NULL) okay = 0;
            }
            if (!okay && error != NULL && error->message[0] == '\0') {
                (void)fail(error, 1, 1, "cannot allocate object contents");
            }
        }
    }

    for (size_t i = 0; i < 4; ++i) free(sections[i].data);
    for (size_t i = 0; i < definition_count; ++i) free(definitions[i].name);
    free(definitions); free(entry);
    name_destroy(&globals); name_destroy(&externs);
    if (!okay) cvm_object_destroy(object);
    return okay;
}
