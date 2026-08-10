#include "cvmir.h"

/*
 * Legacy bootstrap parser retained as historical reference only.
 * build.ps1 and the test runner use cvmir_llvm.c.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CVMIR_NAME_MAX 96
#define CVMIR_MAX_TOKENS 128
#define CVMIR_MAX_ARGS 8
#define CVMIR_MAX_VALUES 4096
#define CVMIR_MAX_INSTRUCTIONS 8192
#define CVMIR_MAX_BLOCKS 1024

typedef struct {
    char text[CVMIR_NAME_MAX];
    size_t column;
} IrToken;

typedef struct {
    IrToken items[CVMIR_MAX_TOKENS];
    size_t count;
} IrTokens;

typedef enum {
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_SDIV,
    IR_UDIV,
    IR_SREM,
    IR_UREM,
    IR_AND,
    IR_OR,
    IR_XOR,
    IR_SHL,
    IR_LSHR,
    IR_ASHR,
    IR_ICMP,
    IR_SELECT,
    IR_CALL,
    IR_RET,
    IR_BR,
    IR_BR_COND
} IrOpcode;

typedef struct {
    char name[CVMIR_NAME_MAX];
    unsigned bits;
    int offset;
    int signext;
    int zeroext;
} IrValue;

typedef struct {
    IrOpcode opcode;
    size_t line;
    unsigned bits;
    int result;
    char first[CVMIR_NAME_MAX];
    char second[CVMIR_NAME_MAX];
    char third[CVMIR_NAME_MAX];
    char predicate[8];
    char callee[CVMIR_NAME_MAX];
    char arguments[CVMIR_MAX_ARGS][CVMIR_NAME_MAX];
    size_t argument_count;
    char true_block[CVMIR_NAME_MAX];
    char false_block[CVMIR_NAME_MAX];
} IrInstruction;

typedef struct {
    char name[CVMIR_NAME_MAX];
    size_t first_instruction;
    unsigned label;
} IrBlock;

typedef struct {
    char name[CVMIR_NAME_MAX];
    unsigned return_bits;
    int return_signext;
    int return_zeroext;
    IrValue *values;
    size_t value_count;
    size_t parameter_count;
    IrInstruction *instructions;
    size_t instruction_count;
    IrBlock *blocks;
    size_t block_count;
    size_t frame_size;
} IrFunction;

typedef struct {
    IrFunction *functions;
    size_t function_count;
    char (*declarations)[CVMIR_NAME_MAX];
    size_t declaration_count;
    int saw_target_triple;
    char target_triple[CVMIR_NAME_MAX];
    int saw_data_layout;
    char data_layout[256];
} IrModule;

typedef struct {
    const CvmIrOptions *options;
    CvmIrError *error;
    int failed;
} IrParser;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} TextBuffer;

typedef struct {
    TextBuffer output;
    unsigned next_label;
} IrGenerator;

static void set_error(IrParser *parser, size_t line, size_t column,
                      const char *format, ...)
{
    if (parser->failed) {
        return;
    }
    parser->failed = 1;
    parser->error->line = line;
    parser->error->column = column;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(parser->error->message, sizeof(parser->error->message),
              format, arguments);
    va_end(arguments);
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    size_t length = strlen(text);
    while (length != 0 && isspace((unsigned char)text[length - 1])) {
        text[--length] = '\0';
    }
    return text;
}

static int is_punctuation(char character)
{
    return strchr("(){}[],=:*", character) != NULL;
}

static int tokenize_line(IrParser *parser, const char *line_text,
                         size_t line_number, IrTokens *tokens)
{
    memset(tokens, 0, sizeof(*tokens));
    const char *cursor = line_text;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == ';') {
            break;
        }
        if (tokens->count == CVMIR_MAX_TOKENS) {
            set_error(parser, line_number, (size_t)(cursor - line_text + 1),
                      "too many tokens on one IR line");
            return 0;
        }
        IrToken *token = &tokens->items[tokens->count++];
        token->column = (size_t)(cursor - line_text + 1);
        size_t length = 0;
        if (is_punctuation(*cursor)) {
            token->text[length++] = *cursor++;
        } else if (*cursor == '"') {
            token->text[length++] = *cursor++;
            while (*cursor != '\0' && *cursor != '"') {
                if (length + 2 >= sizeof(token->text)) {
                    set_error(parser, line_number, token->column,
                              "IR string token is too long");
                    return 0;
                }
                token->text[length++] = *cursor++;
            }
            if (*cursor != '"') {
                set_error(parser, line_number, token->column,
                          "unterminated IR string");
                return 0;
            }
            token->text[length++] = *cursor++;
        } else {
            while (*cursor != '\0' &&
                   !isspace((unsigned char)*cursor) &&
                   !is_punctuation(*cursor) && *cursor != ';') {
                if (length + 1 >= sizeof(token->text)) {
                    set_error(parser, line_number, token->column,
                              "IR token is too long");
                    return 0;
                }
                token->text[length++] = *cursor++;
            }
        }
        token->text[length] = '\0';
    }
    return !parser->failed;
}

static int token_is(const IrTokens *tokens, size_t index, const char *text)
{
    return index < tokens->count && strcmp(tokens->items[index].text, text) == 0;
}

static int parse_integer_type(const char *text, unsigned *bits)
{
    if (text == NULL || text[0] != 'i' || !isdigit((unsigned char)text[1])) {
        return 0;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text + 1, &end, 10);
    if (errno == ERANGE || end == text + 1 || *end != '\0' ||
        (value != 1 && value != 8 && value != 16 && value != 32 &&
         value != 64)) {
        return 0;
    }
    *bits = (unsigned)value;
    return 1;
}

static int is_value_operand(const char *text)
{
    if (text == NULL || *text == '\0') {
        return 0;
    }
    if (*text == '%') {
        return text[1] != '\0';
    }
    if (*text == '-') {
        ++text;
    }
    if (!isdigit((unsigned char)*text)) {
        return 0;
    }
    while (isdigit((unsigned char)*text)) {
        ++text;
    }
    return *text == '\0';
}

static int copy_name(IrParser *parser, char destination[CVMIR_NAME_MAX],
                     const char *source, size_t line, size_t column,
                     char required_prefix)
{
    if (source == NULL || (required_prefix != '\0' &&
                           source[0] != required_prefix) ||
        source[required_prefix != '\0' ? 1 : 0] == '\0') {
        set_error(parser, line, column, "expected %c-prefixed name",
                  required_prefix);
        return 0;
    }
    source += required_prefix != '\0' ? 1 : 0;
    if (strlen(source) >= CVMIR_NAME_MAX) {
        set_error(parser, line, column, "IR name is too long");
        return 0;
    }
    strcpy(destination, source);
    return 1;
}

static int reserve_array(IrParser *parser, void **array, size_t element_size,
                         size_t old_count, size_t new_count, size_t line)
{
    if (new_count > SIZE_MAX / element_size) {
        set_error(parser, line, 1, "IR table is too large");
        return 0;
    }
    void *grown = realloc(*array, new_count * element_size);
    if (grown == NULL) {
        set_error(parser, line, 1, "out of memory");
        return 0;
    }
    memset((unsigned char *)grown + old_count * element_size, 0,
           (new_count - old_count) * element_size);
    *array = grown;
    return 1;
}

static int find_value(const IrFunction *function, const char *name)
{
    const char *plain = name != NULL && name[0] == '%' ? name + 1 : name;
    for (size_t i = 0; i < function->value_count; ++i) {
        if (strcmp(function->values[i].name, plain) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int add_value(IrParser *parser, IrFunction *function,
                     const char *name, unsigned bits, size_t line,
                     size_t column)
{
    if (function->value_count == CVMIR_MAX_VALUES) {
        set_error(parser, line, column, "too many SSA values in function");
        return -1;
    }
    char plain[CVMIR_NAME_MAX];
    if (!copy_name(parser, plain, name, line, column, '%')) {
        return -1;
    }
    if (find_value(function, plain) >= 0) {
        set_error(parser, line, column, "duplicate SSA value '%%%s'", plain);
        return -1;
    }
    size_t old_count = function->value_count;
    if (!reserve_array(parser, (void **)&function->values,
                       sizeof(*function->values), old_count, old_count + 1,
                       line)) {
        return -1;
    }
    IrValue *value = &function->values[old_count];
    strcpy(value->name, plain);
    value->bits = bits;
    value->offset = -(int)((old_count + 1) * 8);
    function->value_count = old_count + 1;
    return (int)old_count;
}

static int add_instruction(IrParser *parser, IrFunction *function,
                           IrInstruction instruction)
{
    if (function->instruction_count == CVMIR_MAX_INSTRUCTIONS) {
        set_error(parser, instruction.line, 1,
                  "too many instructions in function");
        return 0;
    }
    size_t old_count = function->instruction_count;
    if (!reserve_array(parser, (void **)&function->instructions,
                       sizeof(*function->instructions), old_count,
                       old_count + 1, instruction.line)) {
        return 0;
    }
    function->instructions[old_count] = instruction;
    function->instruction_count = old_count + 1;
    return 1;
}

static int add_block(IrParser *parser, IrFunction *function,
                     const char *name, size_t line, size_t column)
{
    if (function->block_count == CVMIR_MAX_BLOCKS) {
        set_error(parser, line, column, "too many basic blocks in function");
        return 0;
    }
    for (size_t i = 0; i < function->block_count; ++i) {
        if (strcmp(function->blocks[i].name, name) == 0) {
            set_error(parser, line, column, "duplicate basic block '%s'", name);
            return 0;
        }
    }
    size_t old_count = function->block_count;
    if (!reserve_array(parser, (void **)&function->blocks,
                       sizeof(*function->blocks), old_count, old_count + 1,
                       line)) {
        return 0;
    }
    IrBlock *block = &function->blocks[old_count];
    if (strlen(name) >= sizeof(block->name)) {
        set_error(parser, line, column, "basic block name is too long");
        return 0;
    }
    strcpy(block->name, name);
    block->first_instruction = function->instruction_count;
    function->block_count = old_count + 1;
    return 1;
}

static IrOpcode binary_opcode(IrParser *parser, const IrToken *token,
                              size_t line)
{
    struct OpcodeName { const char *name; IrOpcode opcode; };
    static const struct OpcodeName names[] = {
        {"add", IR_ADD}, {"sub", IR_SUB}, {"mul", IR_MUL},
        {"sdiv", IR_SDIV}, {"udiv", IR_UDIV},
        {"srem", IR_SREM}, {"urem", IR_UREM},
        {"and", IR_AND}, {"or", IR_OR}, {"xor", IR_XOR},
        {"shl", IR_SHL}, {"lshr", IR_LSHR}, {"ashr", IR_ASHR}
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(token->text, names[i].name) == 0) {
            return names[i].opcode;
        }
    }
    set_error(parser, line, token->column, "unsupported LLVM instruction '%s'",
              token->text);
    return IR_RET;
}

static int is_binary_name(const char *name)
{
    static const char *const names[] = {
        "add", "sub", "mul", "sdiv", "udiv", "srem", "urem",
        "and", "or", "xor", "shl", "lshr", "ashr"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t skip_ir_flags(const IrTokens *tokens, size_t index)
{
    while (index < tokens->count &&
           (token_is(tokens, index, "nsw") || token_is(tokens, index, "nuw") ||
            token_is(tokens, index, "exact"))) {
        ++index;
    }
    return index;
}

static int parse_binary_instruction(IrParser *parser, IrFunction *function,
                                    const IrTokens *tokens, size_t line,
                                    const char *result_name, size_t op_index)
{
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.line = line;
    instruction.opcode = binary_opcode(parser, &tokens->items[op_index], line);
    size_t index = skip_ir_flags(tokens, op_index + 1);
    if (parser->failed || index >= tokens->count ||
        !parse_integer_type(tokens->items[index].text, &instruction.bits)) {
        set_error(parser, line,
                  index < tokens->count ? tokens->items[index].column : 1,
                  "expected supported integer type after '%s'",
                  tokens->items[op_index].text);
        return 0;
    }
    ++index;
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected first integer operand");
        return 0;
    }
    strcpy(instruction.first, tokens->items[index++].text);
    if (token_is(tokens, index, ",")) {
        ++index;
    }
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected second integer operand");
        return 0;
    }
    strcpy(instruction.second, tokens->items[index].text);
    instruction.result = add_value(parser, function, result_name,
                                   instruction.bits, line,
                                   tokens->items[0].column);
    return instruction.result >= 0 &&
           add_instruction(parser, function, instruction);
}

static int parse_icmp_instruction(IrParser *parser, IrFunction *function,
                                  const IrTokens *tokens, size_t line,
                                  const char *result_name, size_t op_index)
{
    if (op_index + 4 >= tokens->count) {
        set_error(parser, line, tokens->items[op_index].column,
                  "incomplete icmp instruction");
        return 0;
    }
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = IR_ICMP;
    instruction.line = line;
    if (strlen(tokens->items[op_index + 1].text) >=
        sizeof(instruction.predicate)) {
        set_error(parser, line, tokens->items[op_index + 1].column,
                  "icmp predicate is too long");
        return 0;
    }
    strcpy(instruction.predicate, tokens->items[op_index + 1].text);
    static const char *const predicates[] = {
        "eq", "ne", "slt", "sle", "sgt", "sge",
        "ult", "ule", "ugt", "uge"
    };
    int valid = 0;
    for (size_t i = 0; i < sizeof(predicates) / sizeof(predicates[0]); ++i) {
        valid |= strcmp(instruction.predicate, predicates[i]) == 0;
    }
    size_t index = op_index + 2;
    if (!valid || !parse_integer_type(tokens->items[index].text,
                                      &instruction.bits)) {
        set_error(parser, line, tokens->items[index].column,
                  "unsupported icmp predicate or integer type");
        return 0;
    }
    ++index;
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, tokens->items[index].column,
                  "expected first icmp operand");
        return 0;
    }
    strcpy(instruction.first, tokens->items[index++].text);
    if (token_is(tokens, index, ",")) {
        ++index;
    }
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected second icmp operand");
        return 0;
    }
    strcpy(instruction.second, tokens->items[index].text);
    instruction.result = add_value(parser, function, result_name, 1, line,
                                   tokens->items[0].column);
    return instruction.result >= 0 &&
           add_instruction(parser, function, instruction);
}

static int parse_call_instruction(IrParser *parser, IrFunction *function,
                                  const IrTokens *tokens, size_t line,
                                  const char *result_name, size_t op_index)
{
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = IR_CALL;
    instruction.line = line;
    size_t index = op_index + 1;
    while (index < tokens->count &&
           (token_is(tokens, index, "signext") ||
            token_is(tokens, index, "zeroext") ||
            token_is(tokens, index, "noundef"))) {
        ++index;
    }
    if (index >= tokens->count ||
        !parse_integer_type(tokens->items[index].text, &instruction.bits)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "cvmir currently supports only integer call results");
        return 0;
    }
    ++index;
    while (index < tokens->count && tokens->items[index].text[0] != '@') {
        ++index;
    }
    if (index >= tokens->count ||
        !copy_name(parser, instruction.callee, tokens->items[index].text,
                   line, tokens->items[index].column, '@')) {
        return 0;
    }
    ++index;
    if (!token_is(tokens, index, "(")) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected call argument list");
        return 0;
    }
    ++index;
    while (index < tokens->count && !token_is(tokens, index, ")")) {
        unsigned argument_bits = 0;
        if (!parse_integer_type(tokens->items[index].text, &argument_bits)) {
            set_error(parser, line, tokens->items[index].column,
                      "only integer call arguments are currently supported");
            return 0;
        }
        ++index;
        while (index < tokens->count &&
               !is_value_operand(tokens->items[index].text)) {
            ++index;
        }
        if (index >= tokens->count || instruction.argument_count == CVMIR_MAX_ARGS) {
            set_error(parser, line, index < tokens->count
                                         ? tokens->items[index].column : 1,
                      "call requires more than 8 register arguments");
            return 0;
        }
        (void)argument_bits;
        strcpy(instruction.arguments[instruction.argument_count++],
               tokens->items[index++].text);
        if (token_is(tokens, index, ",")) {
            ++index;
        }
    }
    if (result_name == NULL) {
        set_error(parser, line, tokens->items[op_index].column,
                  "void calls are not supported in this first cvmir stage");
        return 0;
    }
    instruction.result = add_value(parser, function, result_name,
                                   instruction.bits, line,
                                   tokens->items[0].column);
    return instruction.result >= 0 &&
           add_instruction(parser, function, instruction);
}

static int parse_select_instruction(IrParser *parser, IrFunction *function,
                                    const IrTokens *tokens, size_t line,
                                    const char *result_name, size_t op_index)
{
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = IR_SELECT;
    instruction.line = line;
    size_t index = op_index + 1;
    if (!token_is(tokens, index, "i1") || index + 1 >= tokens->count ||
        !is_value_operand(tokens->items[index + 1].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "select requires an i1 condition");
        return 0;
    }
    strcpy(instruction.first, tokens->items[index + 1].text);
    index += 2;
    if (token_is(tokens, index, ",")) ++index;
    if (index >= tokens->count ||
        !parse_integer_type(tokens->items[index].text, &instruction.bits)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "select requires an integer true value");
        return 0;
    }
    ++index;
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected select true operand");
        return 0;
    }
    strcpy(instruction.second, tokens->items[index++].text);
    if (token_is(tokens, index, ",")) ++index;
    unsigned false_bits = 0;
    if (index >= tokens->count ||
        !parse_integer_type(tokens->items[index].text, &false_bits) ||
        false_bits != instruction.bits) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "select operand types must match");
        return 0;
    }
    ++index;
    if (index >= tokens->count || !is_value_operand(tokens->items[index].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected select false operand");
        return 0;
    }
    strcpy(instruction.third, tokens->items[index].text);
    instruction.result = add_value(parser, function, result_name,
                                   instruction.bits, line,
                                   tokens->items[0].column);
    return instruction.result >= 0 &&
           add_instruction(parser, function, instruction);
}

static int parse_return_instruction(IrParser *parser, IrFunction *function,
                                    const IrTokens *tokens, size_t line,
                                    size_t op_index)
{
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = IR_RET;
    instruction.line = line;
    if (op_index + 2 >= tokens->count ||
        !parse_integer_type(tokens->items[op_index + 1].text,
                            &instruction.bits) ||
        !is_value_operand(tokens->items[op_index + 2].text)) {
        set_error(parser, line, tokens->items[op_index].column,
                  "cvmir currently requires 'ret iN VALUE'");
        return 0;
    }
    strcpy(instruction.first, tokens->items[op_index + 2].text);
    return add_instruction(parser, function, instruction);
}

static int parse_branch_instruction(IrParser *parser, IrFunction *function,
                                    const IrTokens *tokens, size_t line,
                                    size_t op_index)
{
    IrInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.line = line;
    size_t index = op_index + 1;
    if (token_is(tokens, index, "label")) {
        instruction.opcode = IR_BR;
        ++index;
        if (index >= tokens->count || tokens->items[index].text[0] != '%') {
            set_error(parser, line, index < tokens->count
                                         ? tokens->items[index].column : 1,
                      "expected branch target label");
            return 0;
        }
        return copy_name(parser, instruction.true_block,
                         tokens->items[index].text, line,
                         tokens->items[index].column, '%') &&
               add_instruction(parser, function, instruction);
    }
    instruction.opcode = IR_BR_COND;
    if (!token_is(tokens, index, "i1") || index + 1 >= tokens->count ||
        !is_value_operand(tokens->items[index + 1].text)) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected i1 branch condition");
        return 0;
    }
    strcpy(instruction.first, tokens->items[index + 1].text);
    index += 2;
    if (token_is(tokens, index, ",")) ++index;
    if (!token_is(tokens, index, "label")) {
        set_error(parser, line, tokens->items[index].column,
                  "expected true label");
        return 0;
    }
    ++index;
    if (index >= tokens->count ||
        !copy_name(parser, instruction.true_block,
                   tokens->items[index].text, line,
                   tokens->items[index].column, '%')) {
        return 0;
    }
    ++index;
    if (token_is(tokens, index, ",")) ++index;
    if (!token_is(tokens, index, "label")) {
        set_error(parser, line, index < tokens->count
                                     ? tokens->items[index].column : 1,
                  "expected false label");
        return 0;
    }
    ++index;
    return index < tokens->count &&
           copy_name(parser, instruction.false_block,
                     tokens->items[index].text, line,
                     tokens->items[index].column, '%') &&
           add_instruction(parser, function, instruction);
}

static int parse_instruction(IrParser *parser, IrFunction *function,
                             const IrTokens *tokens, size_t line)
{
    if (tokens->count == 0) {
        return 1;
    }
    const char *result_name = NULL;
    size_t op_index = 0;
    if (tokens->items[0].text[0] == '%' && token_is(tokens, 1, "=")) {
        result_name = tokens->items[0].text;
        op_index = 2;
    }
    if (op_index >= tokens->count) {
        set_error(parser, line, 1, "missing LLVM instruction");
        return 0;
    }
    const char *opcode = tokens->items[op_index].text;
    if ((strcmp(opcode, "tail") == 0 || strcmp(opcode, "musttail") == 0 ||
         strcmp(opcode, "notail") == 0) &&
        token_is(tokens, op_index + 1, "call")) {
        ++op_index;
        opcode = tokens->items[op_index].text;
    }
    if (is_binary_name(opcode)) {
        if (result_name == NULL) {
            set_error(parser, line, tokens->items[op_index].column,
                      "binary instruction requires an SSA result");
            return 0;
        }
        return parse_binary_instruction(parser, function, tokens, line,
                                        result_name, op_index);
    }
    if (strcmp(opcode, "icmp") == 0) {
        if (result_name == NULL) {
            set_error(parser, line, tokens->items[op_index].column,
                      "icmp requires an SSA result");
            return 0;
        }
        return parse_icmp_instruction(parser, function, tokens, line,
                                      result_name, op_index);
    }
    if (strcmp(opcode, "select") == 0) {
        if (result_name == NULL) {
            set_error(parser, line, tokens->items[op_index].column,
                      "select requires an SSA result");
            return 0;
        }
        return parse_select_instruction(parser, function, tokens, line,
                                        result_name, op_index);
    }
    if (strcmp(opcode, "call") == 0) {
        return parse_call_instruction(parser, function, tokens, line,
                                      result_name, op_index);
    }
    if (strcmp(opcode, "ret") == 0) {
        if (result_name != NULL) {
            set_error(parser, line, tokens->items[0].column,
                      "ret cannot define an SSA value");
            return 0;
        }
        return parse_return_instruction(parser, function, tokens, line,
                                        op_index);
    }
    if (strcmp(opcode, "br") == 0) {
        return parse_branch_instruction(parser, function, tokens, line,
                                        op_index);
    }
    set_error(parser, line, tokens->items[op_index].column,
              "unsupported LLVM instruction '%s'", opcode);
    return 0;
}

static int append_function(IrParser *parser, IrModule *module,
                           IrFunction function, size_t line)
{
    for (size_t i = 0; i < module->function_count; ++i) {
        if (strcmp(module->functions[i].name, function.name) == 0) {
            set_error(parser, line, 1, "duplicate function '@%s'",
                      function.name);
            return 0;
        }
    }
    size_t old_count = module->function_count;
    if (!reserve_array(parser, (void **)&module->functions,
                       sizeof(*module->functions), old_count, old_count + 1,
                       line)) {
        return 0;
    }
    module->functions[old_count] = function;
    module->function_count = old_count + 1;
    return 1;
}

static int parse_function_header(IrParser *parser, const IrTokens *tokens,
                                 size_t line, IrFunction *function)
{
    memset(function, 0, sizeof(*function));
    size_t at_index = tokens->count;
    for (size_t i = 1; i < tokens->count; ++i) {
        unsigned bits = 0;
        if (parse_integer_type(tokens->items[i].text, &bits)) {
            function->return_bits = bits;
        }
        if (strcmp(tokens->items[i].text, "signext") == 0) {
            function->return_signext = 1;
        } else if (strcmp(tokens->items[i].text, "zeroext") == 0) {
            function->return_zeroext = 1;
        }
        if (tokens->items[i].text[0] == '@') {
            at_index = i;
            break;
        }
    }
    if (at_index == tokens->count || function->return_bits == 0 ||
        !copy_name(parser, function->name, tokens->items[at_index].text,
                   line, tokens->items[at_index].column, '@')) {
        set_error(parser, line, 1,
                  "expected integer function definition and @name");
        return 0;
    }
    size_t index = at_index + 1;
    if (!token_is(tokens, index, "(")) {
        set_error(parser, line, tokens->items[at_index].column,
                  "expected function parameter list");
        return 0;
    }
    ++index;
    while (index < tokens->count && !token_is(tokens, index, ")")) {
        unsigned bits = 0;
        if (!parse_integer_type(tokens->items[index].text, &bits)) {
            set_error(parser, line, tokens->items[index].column,
                      "only integer parameters are supported initially");
            return 0;
        }
        ++index;
        int signext = 0;
        int zeroext = 0;
        while (index < tokens->count && tokens->items[index].text[0] != '%') {
            signext |= token_is(tokens, index, "signext");
            zeroext |= token_is(tokens, index, "zeroext");
            ++index;
        }
        if (index >= tokens->count || function->parameter_count == CVMIR_MAX_ARGS) {
            set_error(parser, line, index < tokens->count
                                         ? tokens->items[index].column : 1,
                      "function requires unsupported parameters");
            return 0;
        }
        int value_index = add_value(parser, function,
                                    tokens->items[index].text, bits, line,
                                    tokens->items[index].column);
        if (value_index < 0) {
            return 0;
        }
        function->values[value_index].signext = signext;
        function->values[value_index].zeroext = zeroext;
        ++function->parameter_count;
        ++index;
        if (token_is(tokens, index, ",")) {
            ++index;
        }
    }
    function->frame_size = (function->value_count * 8 + 15) & ~(size_t)15;
    return 1;
}

static int parse_target_triple(IrParser *parser, IrModule *module,
                               const char *line_text, size_t line)
{
    const char *first = strchr(line_text, '"');
    const char *last = first != NULL ? strchr(first + 1, '"') : NULL;
    if (first == NULL || last == NULL || last == first + 1 ||
        (size_t)(last - first - 1) >= sizeof(module->target_triple)) {
        set_error(parser, line, 1, "malformed target triple");
        return 0;
    }
    memcpy(module->target_triple, first + 1, (size_t)(last - first - 1));
    module->target_triple[last - first - 1] = '\0';
    module->saw_target_triple = 1;
    if (!parser->options->allow_foreign_triple &&
        strcmp(module->target_triple, CVM_LLVM_TARGET_TRIPLE) != 0) {
        set_error(parser, line, 1,
                  "IR target triple '%s' is not '%s'",
                  module->target_triple, CVM_LLVM_TARGET_TRIPLE);
        return 0;
    }
    return 1;
}

static int parse_data_layout(IrParser *parser, IrModule *module,
                             const char *line_text, size_t line)
{
    const char *first = strchr(line_text, '"');
    const char *last = first != NULL ? strchr(first + 1, '"') : NULL;
    if (first == NULL || last == NULL || last == first + 1 ||
        (size_t)(last - first - 1) >= sizeof(module->data_layout)) {
        set_error(parser, line, 1, "malformed target datalayout");
        return 0;
    }
    memcpy(module->data_layout, first + 1, (size_t)(last - first - 1));
    module->data_layout[last - first - 1] = '\0';
    module->saw_data_layout = 1;
    if (!parser->options->allow_foreign_triple &&
        strcmp(module->data_layout, CVM_LLVM_DATA_LAYOUT) != 0) {
        set_error(parser, line, 1,
                  "IR DataLayout does not match CVM LLVM DataLayout v1");
        return 0;
    }
    return 1;
}

static int parse_module(IrParser *parser, const char *source, IrModule *module)
{
    memset(module, 0, sizeof(*module));
    size_t source_length = strlen(source);
    char *copy = malloc(source_length + 1);
    if (copy == NULL) {
        set_error(parser, 1, 1, "out of memory");
        return 0;
    }
    memcpy(copy, source, source_length + 1);
    IrFunction current;
    memset(&current, 0, sizeof(current));
    int in_function = 0;
    size_t line_number = 1;
    char *cursor = copy;
    while (!parser->failed) {
        char *line_start = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline != NULL) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor += strlen(cursor);
        }
        char *line_text = trim(line_start);
        if (*line_text != '\0' && *line_text != ';') {
            if (!in_function &&
                strncmp(line_text, "target datalayout", 17) == 0) {
                parse_data_layout(parser, module, line_text, line_number);
            } else if (!in_function &&
                       strncmp(line_text, "target triple", 13) == 0) {
                parse_target_triple(parser, module, line_text, line_number);
            } else if (!in_function &&
                       strncmp(line_text, "define ", 7) != 0) {
                /* Attributes, declarations and metadata are not executable. */
            } else {
                IrTokens tokens;
                if (!tokenize_line(parser, line_text, line_number, &tokens)) {
                    break;
                }
                if (!in_function && token_is(&tokens, 0, "define")) {
                    if (!parse_function_header(parser, &tokens, line_number,
                                               &current)) {
                        break;
                    }
                    in_function = 1;
                } else if (in_function && token_is(&tokens, 0, "}")) {
                    current.frame_size =
                        (current.value_count * 8 + 15) & ~(size_t)15;
                    if (!append_function(parser, module, current, line_number)) {
                        break;
                    }
                    memset(&current, 0, sizeof(current));
                    in_function = 0;
                } else if (in_function && tokens.count >= 2 &&
                           token_is(&tokens, 1, ":")) {
                    if (!add_block(parser, &current, tokens.items[0].text,
                                   line_number, tokens.items[0].column)) {
                        break;
                    }
                } else if (in_function) {
                    if (!parse_instruction(parser, &current, &tokens,
                                           line_number)) {
                        break;
                    }
                }
            }
        }
        if (newline == NULL) {
            break;
        }
        ++line_number;
    }
    if (!parser->failed && in_function) {
        set_error(parser, line_number, 1, "unterminated function definition");
    }
    free(copy);
    return !parser->failed;
}

static int buffer_reserve(TextBuffer *buffer, size_t additional)
{
    if (additional > SIZE_MAX - buffer->length - 1) {
        buffer->failed = 1;
        return 0;
    }
    size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) {
        return 1;
    }
    size_t capacity = buffer->capacity != 0 ? buffer->capacity : 1024;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static void emit(IrGenerator *generator, const char *format, ...)
{
    if (generator->output.failed) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 ||
        !buffer_reserve(&generator->output, (size_t)needed)) {
        generator->output.failed = 1;
        va_end(arguments);
        return;
    }
    vsnprintf(generator->output.data + generator->output.length,
              generator->output.capacity - generator->output.length,
              format, arguments);
    generator->output.length += (size_t)needed;
    va_end(arguments);
}

static int parse_constant(const char *text, uint64_t *value)
{
    errno = 0;
    char *end = NULL;
    if (text[0] == '-') {
        long long signed_value = strtoll(text, &end, 10);
        if (errno == ERANGE || end == text || *end != '\0') {
            return 0;
        }
        *value = (uint64_t)signed_value;
    } else {
        unsigned long long unsigned_value = strtoull(text, &end, 10);
        if (errno == ERANGE || end == text || *end != '\0') {
            return 0;
        }
        *value = (uint64_t)unsigned_value;
    }
    return 1;
}

static void emit_load_operand(IrGenerator *generator,
                              const IrFunction *function,
                              const char *operand, const char *reg)
{
    int value_index = find_value(function, operand);
    if (value_index >= 0) {
        emit(generator, "    LOAD64O %s, R14, %d\n", reg,
             function->values[value_index].offset);
        return;
    }
    uint64_t constant = 0;
    if (parse_constant(operand, &constant)) {
        emit(generator, "    MOVI64 %s, 0x%llx\n", reg,
             (unsigned long long)constant);
    }
}

static void emit_zero_normalize(IrGenerator *generator, const char *reg,
                                unsigned bits)
{
    if (bits == 1 || bits == 8) emit(generator, "    ZEXT8 %s, %s\n", reg, reg);
    else if (bits == 16) emit(generator, "    ZEXT16 %s, %s\n", reg, reg);
    else if (bits == 32) emit(generator, "    ZEXT32 %s, %s\n", reg, reg);
}

static void emit_sign_normalize(IrGenerator *generator, const char *reg,
                                unsigned bits)
{
    if (bits == 1 || bits == 8) emit(generator, "    SEXT8 %s, %s\n", reg, reg);
    else if (bits == 16) emit(generator, "    SEXT16 %s, %s\n", reg, reg);
    else if (bits == 32) emit(generator, "    SEXT32 %s, %s\n", reg, reg);
}

static const IrBlock *find_block(const IrFunction *function, const char *name)
{
    for (size_t i = 0; i < function->block_count; ++i) {
        if (strcmp(function->blocks[i].name, name) == 0) {
            return &function->blocks[i];
        }
    }
    return NULL;
}

static void emit_block_target(IrGenerator *generator,
                              const IrFunction *function,
                              const char *instruction, const char *name)
{
    const IrBlock *block = find_block(function, name);
    if (block != NULL) {
        emit(generator, "    %s .L%u\n", instruction, block->label);
    }
}

static void generate_instruction(IrGenerator *generator,
                                 const IrFunction *function,
                                 const IrInstruction *instruction,
                                 unsigned return_label)
{
    if (instruction->opcode == IR_RET) {
        emit_load_operand(generator, function, instruction->first, "R0");
        if (function->return_signext) {
            emit_sign_normalize(generator, "R0", function->return_bits);
        } else if (function->return_zeroext) {
            emit_zero_normalize(generator, "R0", function->return_bits);
        }
        emit(generator, "    JUMP .L%u\n", return_label);
        return;
    }
    if (instruction->opcode == IR_BR) {
        emit_block_target(generator, function, "JUMP",
                          instruction->true_block);
        return;
    }
    if (instruction->opcode == IR_BR_COND) {
        emit_load_operand(generator, function, instruction->first, "R0");
        emit(generator, "    CMPI32 R0, 0\n");
        emit_block_target(generator, function, "JNZ",
                          instruction->true_block);
        emit_block_target(generator, function, "JUMP",
                          instruction->false_block);
        return;
    }
    if (instruction->opcode == IR_CALL) {
        for (size_t i = 0; i < instruction->argument_count; ++i) {
            char reg[32];
            snprintf(reg, sizeof(reg), "R%zu", i);
            emit_load_operand(generator, function,
                              instruction->arguments[i], reg);
        }
        emit(generator, "    CALLREL %s\n", instruction->callee);
        emit_zero_normalize(generator, "R0", instruction->bits);
        emit(generator, "    STORE64O R14, R0, %d\n",
             function->values[instruction->result].offset);
        return;
    }
    if (instruction->opcode == IR_SELECT) {
        unsigned false_label = ++generator->next_label;
        unsigned end_label = ++generator->next_label;
        emit_load_operand(generator, function, instruction->first, "R0");
        emit(generator, "    CMPI32 R0, 0\n    JZ .L%u\n", false_label);
        emit_load_operand(generator, function, instruction->second, "R0");
        emit(generator, "    JUMP .L%u\n.L%u:\n", end_label, false_label);
        emit_load_operand(generator, function, instruction->third, "R0");
        emit(generator, ".L%u:\n", end_label);
        emit_zero_normalize(generator, "R0", instruction->bits);
        emit(generator, "    STORE64O R14, R0, %d\n",
             function->values[instruction->result].offset);
        return;
    }
    emit_load_operand(generator, function, instruction->first, "R0");
    emit_load_operand(generator, function, instruction->second, "R1");
    if (instruction->opcode == IR_SDIV || instruction->opcode == IR_SREM ||
        instruction->opcode == IR_ASHR ||
        (instruction->opcode == IR_ICMP && instruction->predicate[0] == 's')) {
        emit_sign_normalize(generator, "R0", instruction->bits);
        emit_sign_normalize(generator, "R1", instruction->bits);
    }
    if (instruction->opcode == IR_ICMP) {
        const char *jump = "JZ";
        if (strcmp(instruction->predicate, "ne") == 0) jump = "JNZ";
        else if (strcmp(instruction->predicate, "slt") == 0) jump = "JLT";
        else if (strcmp(instruction->predicate, "sle") == 0) jump = "JLE";
        else if (strcmp(instruction->predicate, "sgt") == 0) jump = "JGT";
        else if (strcmp(instruction->predicate, "sge") == 0) jump = "JGE";
        else if (strcmp(instruction->predicate, "ult") == 0) jump = "JLTU";
        else if (strcmp(instruction->predicate, "ule") == 0) jump = "JLEU";
        else if (strcmp(instruction->predicate, "ugt") == 0) jump = "JGTU";
        else if (strcmp(instruction->predicate, "uge") == 0) jump = "JGEU";
        unsigned true_label = ++generator->next_label;
        unsigned end_label = ++generator->next_label;
        emit(generator, "    CMP R0, R1\n    MOVI64 R0, 0\n");
        emit(generator, "    %s .L%u\n    JUMP .L%u\n", jump, true_label,
             end_label);
        emit(generator, ".L%u:\n    MOVI64 R0, 1\n.L%u:\n", true_label,
             end_label);
    } else {
        const char *mnemonic = "ADD";
        switch (instruction->opcode) {
            case IR_ADD: mnemonic = "ADD"; break;
            case IR_SUB: mnemonic = "SUB"; break;
            case IR_MUL: mnemonic = "MUL"; break;
            case IR_SDIV: mnemonic = "DIVS"; break;
            case IR_UDIV: mnemonic = "DIVU"; break;
            case IR_SREM: mnemonic = "MODS"; break;
            case IR_UREM: mnemonic = "MODU"; break;
            case IR_AND: mnemonic = "AND"; break;
            case IR_OR: mnemonic = "OR"; break;
            case IR_XOR: mnemonic = "XOR"; break;
            case IR_SHL: mnemonic = "SHLV"; break;
            case IR_LSHR: mnemonic = "SHRV"; break;
            case IR_ASHR: mnemonic = "SARV"; break;
            default: break;
        }
        emit(generator, "    %s R0, R1\n", mnemonic);
        emit_zero_normalize(generator, "R0", instruction->bits);
    }
    emit(generator, "    STORE64O R14, R0, %d\n",
         function->values[instruction->result].offset);
}

static int function_is_defined(const IrModule *module, const char *name)
{
    for (size_t i = 0; i < module->function_count; ++i) {
        if (strcmp(module->functions[i].name, name) == 0) return 1;
    }
    return 0;
}

static void generate_module(IrGenerator *generator, IrModule *module)
{
    emit(generator, "; generated by cvmir for %s\n", CVM_LLVM_TARGET_TRIPLE);
    for (size_t f = 0; f < module->function_count; ++f) {
        IrFunction *function = &module->functions[f];
        for (size_t i = 0; i < function->instruction_count; ++i) {
            IrInstruction *instruction = &function->instructions[i];
            if (instruction->opcode == IR_CALL &&
                !function_is_defined(module, instruction->callee)) {
                int already = 0;
                for (size_t j = 0; j < module->declaration_count; ++j) {
                    already |= strcmp(module->declarations[j],
                                      instruction->callee) == 0;
                }
                if (!already) {
                    size_t old_count = module->declaration_count;
                    char (*grown)[CVMIR_NAME_MAX] = realloc(
                        module->declarations,
                        (old_count + 1) * sizeof(*module->declarations));
                    if (grown == NULL) {
                        generator->output.failed = 1;
                        return;
                    }
                    module->declarations = grown;
                    strcpy(module->declarations[old_count], instruction->callee);
                    module->declaration_count = old_count + 1;
                }
            }
        }
    }
    for (size_t i = 0; i < module->declaration_count; ++i) {
        emit(generator, ".extern %s\n.type %s, function\n",
             module->declarations[i], module->declarations[i]);
    }
    for (size_t f = 0; f < module->function_count; ++f) {
        IrFunction *function = &module->functions[f];
        for (size_t b = 0; b < function->block_count; ++b) {
            function->blocks[b].label = ++generator->next_label;
        }
        unsigned return_label = ++generator->next_label;
        emit(generator,
             "\n.section .text\n.global %s\n.type %s, function\n%s:\n",
             function->name, function->name, function->name);
        emit(generator, "    PUSH R14\n    MOV R14, SP\n");
        if (function->frame_size != 0) {
            emit(generator, "    ADDI32 SP, -%zu\n", function->frame_size);
        }
        for (size_t i = 0; i < function->parameter_count; ++i) {
            emit(generator, "    STORE64O R14, R%zu, %d\n", i,
                 function->values[i].offset);
        }
        size_t next_block = 0;
        for (size_t i = 0; i < function->instruction_count; ++i) {
            while (next_block < function->block_count &&
                   function->blocks[next_block].first_instruction == i) {
                emit(generator, ".L%u:\n",
                     function->blocks[next_block].label);
                ++next_block;
            }
            generate_instruction(generator, function,
                                 &function->instructions[i], return_label);
        }
        while (next_block < function->block_count) {
            emit(generator, ".L%u:\n", function->blocks[next_block].label);
            ++next_block;
        }
        emit(generator, ".L%u:\n    MOV SP, R14\n    POP R14\n    RET\n",
             return_label);
        emit(generator, ".size %s, $ - %s\n", function->name,
             function->name);
    }
}

static void destroy_module(IrModule *module)
{
    for (size_t i = 0; i < module->function_count; ++i) {
        free(module->functions[i].values);
        free(module->functions[i].instructions);
        free(module->functions[i].blocks);
    }
    free(module->functions);
    free(module->declarations);
    memset(module, 0, sizeof(*module));
}

static int validate_references(IrParser *parser, const IrModule *module)
{
    for (size_t f = 0; f < module->function_count; ++f) {
        const IrFunction *function = &module->functions[f];
        for (size_t i = 0; i < function->instruction_count; ++i) {
            const IrInstruction *instruction = &function->instructions[i];
            const char *operands[2] = {instruction->first,
                                       instruction->second};
            size_t operand_count = 0;
            if (instruction->opcode == IR_RET ||
                instruction->opcode == IR_BR_COND) operand_count = 1;
            else if (instruction->opcode == IR_SELECT) operand_count = 1;
            else if (instruction->opcode != IR_BR &&
                     instruction->opcode != IR_CALL) operand_count = 2;
            for (size_t j = 0; j < operand_count; ++j) {
                if (operands[j][0] == '%' &&
                    find_value(function, operands[j]) < 0) {
                    set_error(parser, instruction->line, 1,
                              "unknown SSA value '%s'", operands[j]);
                    return 0;
                }
            }
            if (instruction->opcode == IR_CALL) {
                for (size_t j = 0; j < instruction->argument_count; ++j) {
                    if (instruction->arguments[j][0] == '%' &&
                        find_value(function, instruction->arguments[j]) < 0) {
                        set_error(parser, instruction->line, 1,
                                  "unknown call argument '%s'",
                                  instruction->arguments[j]);
                        return 0;
                    }
                }
            }
            if (instruction->opcode == IR_SELECT) {
                const char *selected[2] = {instruction->second,
                                           instruction->third};
                for (size_t j = 0; j < 2; ++j) {
                    if (selected[j][0] == '%' &&
                        find_value(function, selected[j]) < 0) {
                        set_error(parser, instruction->line, 1,
                                  "unknown select operand '%s'", selected[j]);
                        return 0;
                    }
                }
            }
            if (instruction->opcode == IR_BR ||
                instruction->opcode == IR_BR_COND) {
                if (find_block(function, instruction->true_block) == NULL ||
                    (instruction->opcode == IR_BR_COND &&
                     find_block(function, instruction->false_block) == NULL)) {
                    set_error(parser, instruction->line, 1,
                              "branch references an unknown basic block");
                    return 0;
                }
            }
        }
    }
    return 1;
}

int cvmir_translate(const char *source, const CvmIrOptions *options,
                    char **assembly, CvmIrError *error)
{
    if (source == NULL || options == NULL || assembly == NULL || error == NULL) {
        return 0;
    }
    *assembly = NULL;
    memset(error, 0, sizeof(*error));
    IrParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.options = options;
    parser.error = error;
    IrModule module;
    if (!parse_module(&parser, source, &module) ||
        !validate_references(&parser, &module)) {
        destroy_module(&module);
        return 0;
    }
    if (module.function_count == 0) {
        set_error(&parser, 1, 1, "IR module contains no function definitions");
        destroy_module(&module);
        return 0;
    }
    IrGenerator generator;
    memset(&generator, 0, sizeof(generator));
    generate_module(&generator, &module);
    destroy_module(&module);
    if (generator.output.failed ||
        !buffer_reserve(&generator.output, 0)) {
        free(generator.output.data);
        error->line = 1;
        error->column = 1;
        snprintf(error->message, sizeof(error->message), "out of memory");
        return 0;
    }
    generator.output.data[generator.output.length] = '\0';
    *assembly = generator.output.data;
    return 1;
}
