#include "object_assembler.h"

#include "encoder.h"
#include "lexer.h"

#include <inttypes.h>
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
    int absolute;
    int common;
    uint64_t common_size;
    uint64_t common_alignment;
} Definition;

typedef struct {
    char *name;
    size_t marker;
    uint16_t section;
} SymbolSize;

typedef struct {
    SymbolSize *items;
    size_t count;
    size_t capacity;
} SymbolSizes;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} NameList;

typedef struct {
    char *text;
    uint16_t section;
    size_t source_line;
    size_t marker;
} SourceLine;

typedef struct {
    SourceLine *items;
    size_t count;
    size_t capacity;
} SourceLines;

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

static char *copy_text_n(const char *text, size_t size)
{
    if (size == SIZE_MAX) return NULL;
    char *copy = malloc(size + 1);
    if (copy != NULL) {
        memcpy(copy, text, size);
        copy[size] = '\0';
    }
    return copy;
}

static char *copy_text(const char *text)
{
    return copy_text_n(text, strlen(text));
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

static int text_string(TextBuffer *buffer, const char *text)
{
    return text_append(buffer, text, strlen(text));
}

static int text_line(TextBuffer *buffer, const char *line, size_t size)
{
    return text_append(buffer, line, size) && text_append(buffer, "\n", 1);
}

static int name_index(const NameList *list, const char *name)
{
    for (size_t i = 0; i < list->count; ++i)
        if (strcmp(list->items[i], name) == 0) return (int)i;
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
    for (size_t i = 0; i < count; ++i)
        if (strcmp(items[i].name, name) == 0) return (int)i;
    return -1;
}

static int definition_add(Definition **items, size_t *count,
                          size_t *capacity, const char *name,
                          uint16_t section, int absolute, int common,
                          uint64_t common_size, uint64_t common_alignment)
{
    if (*count == *capacity) {
        size_t next = *capacity == 0 ? 16 : *capacity * 2;
        Definition *grown = realloc(*items, next * sizeof(*grown));
        if (grown == NULL) return 0;
        *items = grown;
        *capacity = next;
    }
    Definition *definition = &(*items)[(*count)++];
    *definition = (Definition){
        .name = copy_text(name),
        .section = section,
        .absolute = absolute,
        .common = common,
        .common_size = common_size,
        .common_alignment = common_alignment
    };
    return definition->name != NULL;
}

static int symbol_size_add(SymbolSizes *sizes, const char *name,
                           uint16_t section)
{
    for (size_t i = 0; i < sizes->count; ++i)
        if (strcmp(sizes->items[i].name, name) == 0) return 0;
    if (sizes->count == sizes->capacity) {
        size_t capacity = sizes->capacity == 0 ? 8 : sizes->capacity * 2;
        SymbolSize *grown = realloc(sizes->items,
                                    capacity * sizeof(*grown));
        if (grown == NULL) return 0;
        sizes->items = grown;
        sizes->capacity = capacity;
    }
    sizes->items[sizes->count] = (SymbolSize){
        .name = copy_text(name),
        .marker = sizes->count,
        .section = section
    };
    if (sizes->items[sizes->count].name == NULL) return 0;
    ++sizes->count;
    return 1;
}

static void symbol_sizes_destroy(SymbolSizes *sizes)
{
    for (size_t i = 0; i < sizes->count; ++i) free(sizes->items[i].name);
    free(sizes->items);
    *sizes = (SymbolSizes){0};
}

static int result_symbol(const AssemblyResult *result, const char *name,
                         uint64_t *value)
{
    for (size_t i = 0; i < result->symbol_count; ++i) {
        if (strcmp(result->symbols[i].name, name) == 0) {
            *value = result->symbols[i].address;
            return 1;
        }
    }
    return 0;
}

static int section_index(const char *name)
{
    if (asm_text_equal_ignore_case(name, ".text")) return 0;
    if (asm_text_equal_ignore_case(name, ".rodata")) return 1;
    if (asm_text_equal_ignore_case(name, ".data")) return 2;
    if (asm_text_equal_ignore_case(name, ".bss")) return 3;
    return -1;
}

static int power_of_two_u64(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static int parse_name_directive(const AsmToken *tokens, size_t count,
                                NameList *names, size_t line,
                                AssemblyError *error)
{
    size_t index = 1;
    int have = 0;
    while (index < count && tokens[index].type != ASM_TOKEN_END) {
        if (tokens[index].type != ASM_TOKEN_IDENTIFIER ||
            strncmp(tokens[index].text, "__cvmobj_", 9) == 0) {
            return fail(error, line, tokens[index].column,
                        "expected non-reserved symbol name");
        }
        if (!name_add(names, tokens[index].text))
            return fail(error, line, tokens[index].column,
                        "cannot allocate symbol declaration");
        have = 1;
        ++index;
        if (tokens[index].type == ASM_TOKEN_END) break;
        if (tokens[index].type != ASM_TOKEN_COMMA)
            return fail(error, line, tokens[index].column,
                        "expected ',' between symbol names");
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
           asm_text_equal_ignore_case(tokens[index].text, ".zero") ||
           asm_text_equal_ignore_case(tokens[index].text, ".align") ||
           asm_text_equal_ignore_case(tokens[index].text, ".equ") ||
           asm_text_equal_ignore_case(tokens[index].text, ".set");
}

static int source_line_add(SourceLines *lines, const char *text, size_t size,
                           uint16_t section, size_t source_line)
{
    if (lines->count == lines->capacity) {
        size_t capacity = lines->capacity == 0 ? 64 : lines->capacity * 2;
        SourceLine *grown = realloc(lines->items, capacity * sizeof(*grown));
        if (grown == NULL) return 0;
        lines->items = grown;
        lines->capacity = capacity;
    }
    SourceLine *line = &lines->items[lines->count];
    *line = (SourceLine){
        .text = copy_text_n(text, size),
        .section = section,
        .source_line = source_line,
        .marker = lines->count
    };
    if (line->text == NULL) return 0;
    ++lines->count;
    return 1;
}

static void source_lines_destroy(SourceLines *lines)
{
    for (size_t i = 0; i < lines->count; ++i) free(lines->items[i].text);
    free(lines->items);
    *lines = (SourceLines){0};
}

static int append_validation_source(TextBuffer *validation,
                                    const TextBuffer sections[4],
                                    const NameList *externs,
                                    const Definition *definitions,
                                    size_t definition_count)
{
    for (size_t i = 0; i < 4; ++i) {
        if (!text_append(validation, sections[i].data != NULL
                                      ? sections[i].data : "",
                         sections[i].size) ||
            !text_string(validation, "\n.align 16\n")) return 0;
    }
    for (size_t i = 0; i < definition_count; ++i) {
        if (!definitions[i].common) continue;
        if (!text_string(validation, definitions[i].name) ||
            !text_string(validation, ": .byte 0\n")) return 0;
    }
    for (size_t i = 0; i < externs->count; ++i) {
        if (!text_string(validation, externs->items[i]) ||
            !text_string(validation, ": .byte 0\n")) return 0;
    }
    return 1;
}

static int build_section_source(TextBuffer *out, uint16_t section,
                                const SourceLines *lines,
                                const Definition *definitions,
                                size_t definition_count,
                                const NameList *externs,
                                const AssemblyResult *validation)
{
    char marker[96];
    for (size_t i = 0; i < lines->count; ++i) {
        if (lines->items[i].section != section) continue;
        int length = snprintf(marker, sizeof(marker),
                              "__cvmobj_line_%zu:\n", lines->items[i].marker);
        if (length < 0 || (size_t)length >= sizeof(marker) ||
            !text_append(out, marker, (size_t)length) ||
            !text_line(out, lines->items[i].text,
                       strlen(lines->items[i].text))) return 0;
    }
    if (!text_string(out, "__cvmobj_payload_end:\n")) return 0;
    for (size_t i = 0; i < definition_count; ++i) {
        if (definitions[i].section == section) continue;
        if (definitions[i].absolute) {
            uint64_t value;
            if (!result_symbol(validation, definitions[i].name, &value))
                return 0;
            int length = snprintf(marker, sizeof(marker), ".equ %s, 0x%016"
                                  PRIx64 "\n", definitions[i].name, value);
            if (length < 0 || (size_t)length >= sizeof(marker) ||
                !text_append(out, marker, (size_t)length)) return 0;
        } else {
            if (!text_string(out, definitions[i].name) ||
                !text_string(out, ": .byte 0\n")) return 0;
        }
    }
    for (size_t i = 0; i < externs->count; ++i) {
        if (!text_string(out, externs->items[i]) ||
            !text_string(out, ": .byte 0\n")) return 0;
    }
    return 1;
}

static uint64_t read_le(const uint8_t *data, size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i)
        value |= (uint64_t)data[i] << (i * 8);
    return value;
}

static int object_symbol_index(const CvmObjectFile *object, const char *name)
{
    for (size_t i = 0; i < object->symbol_count; ++i)
        if (strcmp(object->symbols[i].name, name) == 0) return (int)i;
    return -1;
}

typedef struct {
    const AsmToken *tokens;
    size_t index;
    size_t end;
    int target;
    const CvmObjectFile *object;
} RelocExpression;

static int relocation_coefficient(RelocExpression *expression,
                                  int *coefficient);

static int relocation_atom(RelocExpression *expression, int *coefficient)
{
    if (expression->index >= expression->end) return 0;
    const AsmToken *token = &expression->tokens[expression->index];
    if (token->type == ASM_TOKEN_PLUS || token->type == ASM_TOKEN_MINUS) {
        int negative = token->type == ASM_TOKEN_MINUS;
        ++expression->index;
        if (!relocation_atom(expression, coefficient)) return 0;
        if (negative) *coefficient = -*coefficient;
        return 1;
    }
    if (token->type == ASM_TOKEN_LPAREN) {
        ++expression->index;
        if (!relocation_coefficient(expression, coefficient) ||
            expression->index >= expression->end ||
            expression->tokens[expression->index].type != ASM_TOKEN_RPAREN)
            return 0;
        ++expression->index;
        return 1;
    }
    if (token->type == ASM_TOKEN_NUMBER) {
        *coefficient = 0;
        ++expression->index;
        return 1;
    }
    if (token->type == ASM_TOKEN_IDENTIFIER &&
        strcmp(token->text, "$") != 0) {
        int symbol = object_symbol_index(expression->object, token->text);
        if (symbol < 0) return 0;
        *coefficient = symbol == expression->target ? 1 : 0;
        ++expression->index;
        return 1;
    }
    return 0;
}

static int relocation_coefficient(RelocExpression *expression,
                                  int *coefficient)
{
    if (!relocation_atom(expression, coefficient)) return 0;
    while (expression->index < expression->end &&
           (expression->tokens[expression->index].type == ASM_TOKEN_PLUS ||
            expression->tokens[expression->index].type == ASM_TOKEN_MINUS)) {
        int subtract = expression->tokens[expression->index].type ==
                       ASM_TOKEN_MINUS;
        int right;
        ++expression->index;
        if (!relocation_atom(expression, &right)) return 0;
        *coefficient += subtract ? -right : right;
    }
    return 1;
}

static int expression_symbol(const AsmToken *tokens, size_t begin, size_t end,
                             const CvmObjectFile *object, int *symbol_index,
                             AssemblyError *error, size_t line)
{
    int found = -1;
    for (size_t i = begin; i < end; ++i) {
        if (tokens[i].type != ASM_TOKEN_IDENTIFIER) continue;
        if (strcmp(tokens[i].text, "$") == 0)
            return fail(error, line, tokens[i].column,
                        "'$' is not supported in relocatable operands");
        int index = object_symbol_index(object, tokens[i].text);
        if (index < 0)
            return fail(error, line, tokens[i].column,
                        "unknown symbol in relocatable expression");
        if (object->symbols[index].section_index == CVM_OBJECT_ABSOLUTE_SECTION)
            continue;
        if (found >= 0)
            return fail(error, line, tokens[i].column,
                        "relocation expression may contain one symbol");
        found = index;
    }
    if (found >= 0) {
        RelocExpression expression = {
            .tokens = tokens,
            .index = begin,
            .end = end,
            .target = found,
            .object = object
        };
        int coefficient;
        if (!relocation_coefficient(&expression, &coefficient) ||
            expression.index != end || coefficient != 1)
            return fail(error, line, tokens[begin].column,
                        "relocation must be symbol plus a constant addend");
    }
    *symbol_index = found;
    return 1;
}

static int add_relocation(CvmObjectFile *object, uint16_t section,
                          uint16_t type, uint32_t symbol, uint64_t offset,
                          int64_t addend, AssemblyError *error, size_t line)
{
    if (object->relocation_count == CVM_OBJECT_MAX_RELOCATIONS)
        return fail(error, line, 1, "too many relocations");
    size_t count = object->relocation_count + 1;
    CvmObjectRelocation *grown = realloc(object->relocations,
                                         count * sizeof(*grown));
    if (grown == NULL)
        return fail(error, line, 1, "cannot allocate relocation table");
    object->relocations = grown;
    object->relocations[object->relocation_count++] = (CvmObjectRelocation){
        .section_index = section,
        .type = type,
        .symbol_index = symbol,
        .offset = offset,
        .addend = addend
    };
    return 1;
}

static int relocate_field(CvmObjectFile *object, uint16_t section,
                          uint16_t type, size_t field, size_t next,
                          const AsmToken *tokens, size_t begin, size_t end,
                          const AssemblyResult *assembled,
                          AssemblyError *error, size_t source_line)
{
    int symbol_index;
    if (!expression_symbol(tokens, begin, end, object, &symbol_index,
                           error, source_line)) return 0;
    if (symbol_index < 0) return 1;
    CvmObjectSymbol *symbol = &object->symbols[symbol_index];
    if (type == CVM_OBJECT_RELOCATION_REL32 &&
        symbol->section_index == section) return 1;
    size_t width = cvm_object_relocation_width(type);
    CvmObjectSection *output = &object->sections[section];
    if (field > output->file_size || width > output->file_size - field)
        return fail(error, source_line, tokens[begin].column,
                    "relocation field is outside section");
    uint64_t dummy;
    if (symbol->section_index == CVM_OBJECT_ABSOLUTE_SECTION) return 1;
    if (!result_symbol(assembled, symbol->name, &dummy))
        return fail(error, source_line, tokens[begin].column,
                    "internal relocation symbol is missing");
    uint64_t raw = read_le(output->data + field, width);
    int64_t addend;
    if (type == CVM_OBJECT_RELOCATION_REL32) {
        int64_t displacement = (int64_t)(int32_t)(uint32_t)raw;
        uint64_t target = (uint64_t)((int64_t)next + displacement);
        addend = (int64_t)(target - dummy);
    } else if (width == 4) {
        addend = (int64_t)(int32_t)((uint32_t)raw - (uint32_t)dummy);
    } else {
        addend = (int64_t)(raw - dummy);
    }
    memset(output->data + field, 0, width);
    return add_relocation(object, section, type, (uint32_t)symbol_index,
                          field, addend, error, source_line);
}

static size_t statement_index(const AsmToken *tokens, size_t count)
{
    if (count > 1 && tokens[0].type == ASM_TOKEN_IDENTIFIER &&
        tokens[1].type == ASM_TOKEN_COLON) return 2;
    return 0;
}

static int scan_line_relocations(CvmObjectFile *object,
                                 const SourceLine *line,
                                 const AssemblyResult *assembled,
                                 AssemblyError *error)
{
    AsmToken tokens[ASM_MAX_TOKENS_PER_LINE];
    size_t count = 0;
    if (!asm_lex_line(line->text, line->source_line, tokens, &count, error))
        return 0;
    size_t index = statement_index(tokens, count);
    if (tokens[index].type == ASM_TOKEN_END) return 1;
    char marker[64];
    (void)snprintf(marker, sizeof(marker), "__cvmobj_line_%zu", line->marker);
    uint64_t start64;
    if (!result_symbol(assembled, marker, &start64) || start64 > SIZE_MAX)
        return fail(error, line->source_line, 1,
                    "internal line marker is missing");
    size_t start = (size_t)start64;
    const AsmInstructionSpec *instruction =
        asm_instruction_find(tokens[index].text);
    if (instruction != NULL) {
        size_t begin = 0, field = 0;
        uint16_t type = 0;
        switch (instruction->format) {
        case ASM_FORMAT_R_IMM64:
            begin = index + 3; field = start + 2;
            type = CVM_OBJECT_RELOCATION_ABS64; break;
        case ASM_FORMAT_R_IMM32U:
        case ASM_FORMAT_R_IMM32S:
            begin = index + 3; field = start + 2;
            type = CVM_OBJECT_RELOCATION_ABS32; break;
        case ASM_FORMAT_TARGET64:
            begin = index + 1; field = start + 1;
            type = CVM_OBJECT_RELOCATION_ABS64; break;
        case ASM_FORMAT_RR_DISP32:
            begin = index + 5; field = start + 3;
            type = CVM_OBJECT_RELOCATION_ABS32; break;
        case ASM_FORMAT_REL32:
            begin = index + 1; field = start + 1;
            type = CVM_OBJECT_RELOCATION_REL32; break;
        case ASM_FORMAT_CC_REL32:
            begin = index + 3; field = start + 2;
            type = CVM_OBJECT_RELOCATION_REL32; break;
        case ASM_FORMAT_R_U8: {
            int symbolic;
            if (!expression_symbol(tokens, index + 3, count - 1, object,
                                   &symbolic, error, line->source_line)) return 0;
            if (symbolic >= 0)
                return fail(error, line->source_line, tokens[index + 3].column,
                            "8-bit operands cannot contain relocations");
            return 1;
        }
        default:
            return 1;
        }
        return relocate_field(object, line->section, type, field,
                              start + asm_instruction_size(instruction->format),
                              tokens, begin, count - 1, assembled, error,
                              line->source_line);
    }
    if (tokens[index].type != ASM_TOKEN_IDENTIFIER) return 1;
    size_t width = 0;
    uint16_t type = 0;
    if (asm_text_equal_ignore_case(tokens[index].text, ".qword")) {
        width = 8; type = CVM_OBJECT_RELOCATION_ABS64;
    } else if (asm_text_equal_ignore_case(tokens[index].text, ".dword")) {
        width = 4; type = CVM_OBJECT_RELOCATION_ABS32;
    } else if (asm_text_equal_ignore_case(tokens[index].text, ".word")) {
        width = 2;
    } else if (asm_text_equal_ignore_case(tokens[index].text, ".byte")) {
        width = 1;
    } else {
        return 1;
    }
    size_t begin = index + 1;
    size_t field = start;
    int depth = 0;
    for (size_t i = begin; i < count; ++i) {
        if (tokens[i].type == ASM_TOKEN_LPAREN) ++depth;
        if (tokens[i].type == ASM_TOKEN_RPAREN) --depth;
        if ((tokens[i].type == ASM_TOKEN_COMMA && depth == 0) ||
            tokens[i].type == ASM_TOKEN_END) {
            int symbolic;
            if (!expression_symbol(tokens, begin, i, object, &symbolic,
                                   error, line->source_line)) return 0;
            if (symbolic >= 0) {
                if (width < 4)
                    return fail(error, line->source_line,
                                tokens[begin].column,
                                "8/16-bit data cannot contain relocations");
                if (!relocate_field(object, line->section, type, field,
                                    field + width, tokens, begin, i,
                                    assembled, error,
                                    line->source_line)) return 0;
            }
            field += width;
            begin = i + 1;
        }
    }
    return 1;
}

int assembler_assemble_object(const char *source, CvmObjectFile *object,
                              AssemblyError *error)
{
    if (object == NULL || source == NULL)
        return fail(error, 1, 1, "invalid object assembly input");
    *object = (CvmObjectFile){0};
    if (error != NULL) *error = (AssemblyError){0};
    TextBuffer sections[4] = {{0}};
    SourceLines lines = {0};
    NameList globals = {0}, externs = {0}, weak = {0};
    NameList function_types = {0}, object_types = {0};
    SymbolSizes sizes = {0};
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
        char *line = copy_text_n(cursor, length);
        AsmToken tokens[ASM_MAX_TOKENS_PER_LINE];
        size_t count = 0;
        if (line == NULL) {
            okay = fail(error, line_number, 1, "cannot allocate source line");
            break;
        }
        if (!asm_lex_line(line, line_number, tokens, &count, error)) {
            free(line); okay = 0; break;
        }
        if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
            asm_text_equal_ignore_case(tokens[0].text, ".section")) {
            if (tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_END ||
                (current_section = section_index(tokens[1].text)) < 0)
                okay = fail(error, line_number, tokens[1].column,
                            "expected .text, .rodata, .data, or .bss");
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   (asm_text_equal_ignore_case(tokens[0].text, ".global") ||
                    asm_text_equal_ignore_case(tokens[0].text, ".globl"))) {
            okay = parse_name_directive(tokens, count, &globals,
                                        line_number, error);
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".extern")) {
            okay = parse_name_directive(tokens, count, &externs,
                                        line_number, error);
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".weak")) {
            okay = parse_name_directive(tokens, count, &weak,
                                        line_number, error);
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".type")) {
            if (tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_COMMA ||
                tokens[3].type != ASM_TOKEN_IDENTIFIER ||
                tokens[4].type != ASM_TOKEN_END ||
                (!asm_text_equal_ignore_case(tokens[3].text, "function") &&
                 !asm_text_equal_ignore_case(tokens[3].text, "object"))) {
                okay = fail(error, line_number, tokens[0].column,
                            ".type requires name, function|object");
            } else {
                NameList *types = asm_text_equal_ignore_case(
                                      tokens[3].text, "function")
                                      ? &function_types : &object_types;
                NameList *other = types == &function_types
                                      ? &object_types : &function_types;
                if (name_index(other, tokens[1].text) >= 0)
                    okay = fail(error, line_number, tokens[1].column,
                                "symbol has conflicting .type directives");
                else if (!name_add(types, tokens[1].text))
                    okay = fail(error, line_number, 1,
                                "cannot allocate symbol type");
            }
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".size")) {
            if (tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_COMMA ||
                tokens[3].type == ASM_TOKEN_END) {
                okay = fail(error, line_number, tokens[0].column,
                            ".size requires name, expression");
            } else if (!symbol_size_add(&sizes, tokens[1].text,
                                        (uint16_t)current_section)) {
                okay = fail(error, line_number, 1,
                            "cannot allocate symbol size metadata");
            } else {
                char prefix[96];
                size_t marker = sizes.count - 1;
                int prefix_size = snprintf(prefix, sizeof(prefix),
                    ".equ __cvmobj_size_%zu, ", marker);
                size_t expression = tokens[3].column - 1;
                TextBuffer transformed = {0};
                if (prefix_size < 0 || (size_t)prefix_size >= sizeof(prefix) ||
                    expression > length ||
                    !text_append(&transformed, prefix, (size_t)prefix_size) ||
                    !text_append(&transformed, line + expression,
                                 length - expression) ||
                    !text_line(&sections[current_section], transformed.data,
                               transformed.size) ||
                    !source_line_add(&lines, transformed.data,
                                     transformed.size,
                                     (uint16_t)current_section,
                                     line_number))
                    okay = fail(error, line_number, 1,
                                "cannot encode .size metadata");
                free(transformed.data);
            }
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".comm")) {
            uint64_t alignment = 8;
            if (tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_COMMA ||
                tokens[3].type != ASM_TOKEN_NUMBER ||
                (tokens[4].type != ASM_TOKEN_END &&
                 (tokens[4].type != ASM_TOKEN_COMMA ||
                  tokens[5].type != ASM_TOKEN_NUMBER ||
                  tokens[6].type != ASM_TOKEN_END))) {
                okay = fail(error, line_number, tokens[0].column,
                            ".comm requires name, size[, alignment]");
            } else {
                if (tokens[4].type == ASM_TOKEN_COMMA)
                    alignment = tokens[5].number;
                if (tokens[3].number == 0 ||
                    tokens[3].number > CVM_OBJECT_MAX_SIZE ||
                    !power_of_two_u64(alignment) || alignment > UINT32_MAX) {
                    okay = fail(error, line_number, tokens[3].column,
                                "invalid .comm size or alignment");
                } else if (definition_index(definitions, definition_count,
                                            tokens[1].text) >= 0) {
                    okay = fail(error, line_number, tokens[1].column,
                                "duplicate common symbol");
                } else if (!definition_add(&definitions, &definition_count,
                                           &definition_capacity,
                                           tokens[1].text,
                                           CVM_OBJECT_COMMON_SECTION, 0, 1,
                                           tokens[3].number, alignment) ||
                           !name_add(&globals, tokens[1].text) ||
                           !name_add(&object_types, tokens[1].text)) {
                    okay = fail(error, line_number, 1,
                                "cannot allocate common symbol");
                }
            }
        } else if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                   asm_text_equal_ignore_case(tokens[0].text, ".entry")) {
            if (entry != NULL || tokens[1].type != ASM_TOKEN_IDENTIFIER ||
                tokens[2].type != ASM_TOKEN_END)
                okay = fail(error, line_number, tokens[0].column,
                            "object .entry requires one unique symbol");
            else if ((entry = copy_text(tokens[1].text)) == NULL)
                okay = fail(error, line_number, 1,
                            "cannot allocate entry symbol");
        } else {
            if (tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                asm_text_equal_ignore_case(tokens[0].text, ".org"))
                okay = fail(error, line_number, tokens[0].column,
                            ".org is not allowed in relocatable objects");
            if (okay && current_section == 3 && !bss_line_valid(tokens, count))
                okay = fail(error, line_number, 1,
                            ".bss accepts labels, .space, .align, and .equ");
            int is_label = tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                           tokens[1].type == ASM_TOKEN_COLON;
            int is_equ = tokens[0].type == ASM_TOKEN_IDENTIFIER &&
                         (asm_text_equal_ignore_case(tokens[0].text, ".equ") ||
                          asm_text_equal_ignore_case(tokens[0].text, ".set")) &&
                         tokens[1].type == ASM_TOKEN_IDENTIFIER;
            const AsmToken *defined = is_label ? &tokens[0] :
                                      is_equ ? &tokens[1] : NULL;
            if (okay && defined != NULL) {
                if (strncmp(defined->text, "__cvmobj_", 9) == 0 ||
                    definition_index(definitions, definition_count,
                                     defined->text) >= 0) {
                    okay = fail(error, line_number, defined->column,
                                "duplicate or reserved symbol");
                } else if (!definition_add(&definitions, &definition_count,
                                           &definition_capacity,
                                           defined->text,
                                           (uint16_t)current_section,
                                           is_equ, 0, 0, 0))
                    okay = fail(error, line_number, 1,
                                "cannot allocate symbol table");
            }
            if (okay && (!text_line(&sections[current_section], line, length) ||
                         !source_line_add(&lines, line, length,
                                          (uint16_t)current_section,
                                          line_number)))
                okay = fail(error, line_number, 1,
                            "cannot allocate section source");
        }
        free(line);
        if (end == NULL) break;
        cursor = end + 1;
        ++line_number;
    }

    for (size_t i = 0; okay && i < weak.count; ++i) {
        if (definition_index(definitions, definition_count,
                             weak.items[i]) < 0 &&
            !name_add(&externs, weak.items[i]))
            okay = fail(error, 1, 1, "cannot declare weak symbol");
    }
    for (size_t i = 0; okay && i < globals.count; ++i)
        if (definition_index(definitions, definition_count,
                             globals.items[i]) < 0)
            okay = fail(error, 1, 1, ".global symbol is not defined");
    for (size_t i = 0; okay && i < externs.count; ++i)
        if (definition_index(definitions, definition_count,
                             externs.items[i]) >= 0)
            okay = fail(error, 1, 1, ".extern symbol is also defined");
    for (size_t i = 0; okay && i < function_types.count; ++i)
        if (definition_index(definitions, definition_count,
                             function_types.items[i]) < 0 &&
            name_index(&externs, function_types.items[i]) < 0)
            okay = fail(error, 1, 1, ".type symbol is not declared");
    for (size_t i = 0; okay && i < object_types.count; ++i)
        if (definition_index(definitions, definition_count,
                             object_types.items[i]) < 0 &&
            name_index(&externs, object_types.items[i]) < 0)
            okay = fail(error, 1, 1, ".type symbol is not declared");
    for (size_t i = 0; okay && i < sizes.count; ++i) {
        int definition = definition_index(definitions, definition_count,
                                          sizes.items[i].name);
        if (definition < 0 || definitions[definition].common)
            okay = fail(error, 1, 1,
                        ".size requires a non-common defined symbol");
    }
    int entry_definition = entry != NULL
                               ? definition_index(definitions,
                                                  definition_count, entry)
                               : -1;
    if (okay && entry != NULL && entry_definition < 0)
        okay = fail(error, 1, 1, ".entry symbol is not defined");
    if (okay && entry_definition >= 0 &&
        (definitions[entry_definition].absolute ||
         definitions[entry_definition].section != 0))
        okay = fail(error, 1, 1, ".entry must name a .text label");
    if (okay && entry != NULL && !name_add(&globals, entry))
        okay = fail(error, 1, 1, "cannot export entry symbol");

    TextBuffer validation_source = {0};
    AssemblyResult validation = {0};
    if (okay && !append_validation_source(&validation_source, sections,
                                           &externs, definitions,
                                           definition_count))
        okay = fail(error, 1, 1, "cannot build validation source");
    if (okay) {
        AssemblyError inner;
        if (!assembler_assemble(validation_source.data, 0, &validation,
                                &inner)) {
            if (error != NULL) *error = inner;
            okay = 0;
        }
    }

    static const char *section_names[4] = {
        ".text", ".rodata", ".data", ".bss"
    };
    static const uint32_t section_flags[4] = {
        CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_EXECUTE,
        CVM_OBJECT_SECTION_ALLOC,
        CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_WRITE,
        CVM_OBJECT_SECTION_ALLOC | CVM_OBJECT_SECTION_WRITE |
            CVM_OBJECT_SECTION_NOBITS
    };
    AssemblyResult assembled[4] = {{0}};
    if (okay) {
        object->sections = calloc(4, sizeof(*object->sections));
        object->symbols = calloc(definition_count + externs.count,
                                 sizeof(*object->symbols));
        if (object->sections == NULL ||
            (definition_count + externs.count != 0 && object->symbols == NULL))
            okay = fail(error, 1, 1, "cannot allocate object contents");
        else {
            object->section_count = 4;
            object->symbol_count = definition_count + externs.count;
            for (size_t i = 0; i < 4; ++i) {
                object->sections[i].name = copy_text(section_names[i]);
                object->sections[i].flags = section_flags[i];
                object->sections[i].alignment = 16;
                if (object->sections[i].name == NULL) okay = 0;
            }
            for (size_t i = 0; i < definition_count && okay; ++i) {
                CvmObjectSymbol *symbol = &object->symbols[i];
                symbol->name = copy_text(definitions[i].name);
                if (definitions[i].common) {
                    symbol->section_index = CVM_OBJECT_COMMON_SECTION;
                    symbol->flags = CVM_OBJECT_SYMBOL_GLOBAL |
                                    CVM_OBJECT_SYMBOL_COMMON;
                    symbol->value = definitions[i].common_alignment;
                    symbol->size = definitions[i].common_size;
                } else {
                    symbol->section_index = definitions[i].absolute
                                                ? CVM_OBJECT_ABSOLUTE_SECTION
                                                : definitions[i].section;
                    symbol->flags = CVM_OBJECT_SYMBOL_DEFINED;
                }
                if (name_index(&globals, definitions[i].name) >= 0 ||
                    name_index(&weak, definitions[i].name) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_GLOBAL;
                if (name_index(&weak, definitions[i].name) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_WEAK;
                if (name_index(&function_types, definitions[i].name) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_FUNCTION;
                if (name_index(&object_types, definitions[i].name) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_OBJECT;
                if (entry != NULL && strcmp(entry, definitions[i].name) == 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_ENTRY;
                if (symbol->name == NULL) okay = 0;
            }
            for (size_t i = 0; i < externs.count && okay; ++i) {
                CvmObjectSymbol *symbol =
                    &object->symbols[definition_count + i];
                symbol->name = copy_text(externs.items[i]);
                symbol->section_index = CVM_OBJECT_UNDEFINED_SECTION;
                symbol->flags = CVM_OBJECT_SYMBOL_GLOBAL;
                if (name_index(&weak, externs.items[i]) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_WEAK;
                if (name_index(&function_types, externs.items[i]) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_FUNCTION;
                if (name_index(&object_types, externs.items[i]) >= 0)
                    symbol->flags |= CVM_OBJECT_SYMBOL_OBJECT;
                if (symbol->name == NULL) okay = 0;
            }
            if (!okay && error != NULL && error->message[0] == '\0')
                (void)fail(error, 1, 1, "cannot allocate object contents");
        }
    }

    for (uint16_t section = 0; okay && section < 4; ++section) {
        TextBuffer section_source = {0};
        if (!build_section_source(&section_source, section, &lines,
                                  definitions, definition_count, &externs,
                                  &validation))
            okay = fail(error, 1, 1, "cannot build section assembly");
        if (okay) {
            AssemblyError inner;
            if (!assembler_assemble(section_source.data, 0,
                                    &assembled[section], &inner)) {
                if (error != NULL) *error = inner;
                okay = 0;
            }
        }
        free(section_source.data);
        if (!okay) break;
        uint64_t payload_size;
        if (!result_symbol(&assembled[section], "__cvmobj_payload_end",
                           &payload_size) || payload_size > SIZE_MAX) {
            okay = fail(error, 1, 1, "cannot determine section size");
            break;
        }
        CvmObjectSection *output = &object->sections[section];
        output->memory_size = payload_size;
        output->file_size = section == 3 ? 0 : (size_t)payload_size;
        if (output->file_size != 0) {
            output->data = malloc(output->file_size);
            if (output->data == NULL) {
                okay = fail(error, 1, 1, "cannot allocate section bytes");
                break;
            }
            memcpy(output->data, assembled[section].data, output->file_size);
        }
        for (size_t i = 0; i < definition_count; ++i) {
            if (definitions[i].absolute) {
                if (!result_symbol(&validation, definitions[i].name,
                                   &object->symbols[i].value)) {
                    okay = fail(error, 1, 1,
                                "cannot resolve absolute symbol");
                    break;
                }
            } else if (definitions[i].section == section &&
                       !result_symbol(&assembled[section], definitions[i].name,
                                      &object->symbols[i].value)) {
                okay = fail(error, 1, 1, "cannot resolve section symbol");
                break;
            }
        }
    }
    for (size_t i = 0; okay && i < sizes.count; ++i) {
        char marker[64];
        (void)snprintf(marker, sizeof(marker), "__cvmobj_size_%zu",
                       sizes.items[i].marker);
        uint64_t value;
        int symbol = object_symbol_index(object, sizes.items[i].name);
        if (symbol < 0 ||
            !result_symbol(&assembled[sizes.items[i].section], marker,
                           &value))
            okay = fail(error, 1, 1, "cannot resolve .size expression");
        else
            object->symbols[symbol].size = value;
    }
    for (size_t i = 0; okay && i < lines.count; ++i) {
        if (lines.items[i].section == 3) continue;
        okay = scan_line_relocations(object, &lines.items[i],
                                     &assembled[lines.items[i].section], error);
    }

    for (size_t i = 0; i < 4; ++i) assembly_result_destroy(&assembled[i]);
    assembly_result_destroy(&validation);
    free(validation_source.data);
    for (size_t i = 0; i < 4; ++i) free(sections[i].data);
    for (size_t i = 0; i < definition_count; ++i) free(definitions[i].name);
    free(definitions);
    free(entry);
    name_destroy(&globals);
    name_destroy(&externs);
    name_destroy(&weak);
    name_destroy(&function_types);
    name_destroy(&object_types);
    symbol_sizes_destroy(&sizes);
    source_lines_destroy(&lines);
    if (!okay) cvm_object_destroy(object);
    return okay;
}
