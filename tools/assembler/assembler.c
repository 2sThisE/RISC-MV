#include "assembler.h"

#include "encoder.h"
#include "lexer.h"
#include "symbol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASM_MAX_OUTPUT_SIZE ((size_t)256 * 1024 * 1024)

typedef struct {
    int pass;
    uint64_t base_address;
    size_t offset;
    size_t final_size;
    uint8_t *output;
    AsmSymbolTable *symbols;
    AssemblyResult *result;
    AssemblyError *error;
} AssemblyContext;

typedef struct {
    AssemblyContext *context;
    const AsmToken *tokens;
    size_t token_count;
    size_t index;
    size_t line;
} LineParser;

static int assembly_error_at(AssemblyContext *context,
                             size_t line,
                             size_t column,
                             const char *message)
{
    if (context->error != NULL) {
        context->error->line = line;
        context->error->column = column;
        (void)snprintf(context->error->message,
                       sizeof(context->error->message),
                       "%s",
                       message);
    }
    return 0;
}

static int parser_error(LineParser *parser,
                        const AsmToken *token,
                        const char *message)
{
    size_t column = token != NULL ? token->column : 1;
    return assembly_error_at(parser->context,
                             parser->line,
                             column,
                             message);
}

static const AsmToken *current_token(LineParser *parser)
{
    return parser->index < parser->token_count
               ? &parser->tokens[parser->index]
               : &parser->tokens[parser->token_count - 1];
}

static int consume_token(LineParser *parser, AsmTokenType type)
{
    if (current_token(parser)->type != type) {
        return 0;
    }
    ++parser->index;
    return 1;
}

static int expect_token(LineParser *parser,
                        AsmTokenType type,
                        const char *message)
{
    if (!consume_token(parser, type)) {
        return parser_error(parser, current_token(parser), message);
    }
    return 1;
}

static int parse_expression(LineParser *parser,
                            uint64_t *value,
                            int *resolved);

static int parse_primary(LineParser *parser,
                         uint64_t *value,
                         int *resolved)
{
    const AsmToken *token = current_token(parser);
    if (token->type == ASM_TOKEN_NUMBER) {
        *value = token->number;
        *resolved = 1;
        ++parser->index;
        return 1;
    }
    if (token->type == ASM_TOKEN_IDENTIFIER) {
        if (strcmp(token->text, "$") == 0) {
            if (parser->context->base_address >
                UINT64_MAX - parser->context->offset) {
                return parser_error(parser,
                                    token,
                                    "current address overflow");
            }
            *value = parser->context->base_address +
                     parser->context->offset;
            *resolved = 1;
        } else if (asm_symbol_lookup(parser->context->symbols,
                                     token->text,
                                     value)) {
            *resolved = 1;
        } else if (parser->context->pass == 1) {
            *value = 0;
            *resolved = 0;
        } else {
            char message[256];
            (void)snprintf(message,
                           sizeof(message),
                           "undefined symbol '%s'",
                           token->text);
            return parser_error(parser, token, message);
        }
        ++parser->index;
        return 1;
    }
    if (consume_token(parser, ASM_TOKEN_LPAREN)) {
        if (!parse_expression(parser, value, resolved) ||
            !expect_token(parser,
                          ASM_TOKEN_RPAREN,
                          "expected ')' after expression")) {
            return 0;
        }
        return 1;
    }
    return parser_error(parser, token, "expected expression");
}

static int parse_unary(LineParser *parser,
                       uint64_t *value,
                       int *resolved)
{
    if (consume_token(parser, ASM_TOKEN_PLUS)) {
        return parse_unary(parser, value, resolved);
    }
    if (consume_token(parser, ASM_TOKEN_MINUS)) {
        if (!parse_unary(parser, value, resolved)) {
            return 0;
        }
        *value = UINT64_C(0) - *value;
        return 1;
    }
    if (consume_token(parser, ASM_TOKEN_TILDE)) {
        if (!parse_unary(parser, value, resolved)) return 0;
        *value = ~*value;
        return 1;
    }
    return parse_primary(parser, value, resolved);
}

static int parse_multiply(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_unary(parser, value, resolved)) return 0;
    while (current_token(parser)->type == ASM_TOKEN_STAR ||
           current_token(parser)->type == ASM_TOKEN_SLASH ||
           current_token(parser)->type == ASM_TOKEN_PERCENT) {
        AsmTokenType operation = current_token(parser)->type;
        const AsmToken *operator_token = current_token(parser);
        ++parser->index;
        uint64_t right;
        int right_resolved;
        if (!parse_unary(parser, &right, &right_resolved)) return 0;
        if (right_resolved && right == 0 && operation != ASM_TOKEN_STAR)
            return parser_error(parser, operator_token,
                                "division or remainder by zero");
        int both = *resolved && right_resolved;
        if (operation == ASM_TOKEN_STAR) *value *= right;
        else if (both && operation == ASM_TOKEN_SLASH) *value /= right;
        else if (both) *value %= right;
        else *value = 0;
        *resolved = both;
    }
    return 1;
}

static int parse_add(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_multiply(parser, value, resolved)) return 0;
    while (current_token(parser)->type == ASM_TOKEN_PLUS ||
           current_token(parser)->type == ASM_TOKEN_MINUS) {
        AsmTokenType operation = current_token(parser)->type;
        ++parser->index;
        uint64_t right;
        int right_resolved;
        if (!parse_multiply(parser, &right, &right_resolved)) return 0;
        *resolved = *resolved && right_resolved;
        *value = operation == ASM_TOKEN_PLUS
                     ? *value + right
                     : *value - right;
    }
    return 1;
}

static int parse_shift(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_add(parser, value, resolved)) return 0;
    while (current_token(parser)->type == ASM_TOKEN_SHIFT_LEFT ||
           current_token(parser)->type == ASM_TOKEN_SHIFT_RIGHT) {
        AsmTokenType operation = current_token(parser)->type;
        const AsmToken *operator_token = current_token(parser);
        ++parser->index;
        uint64_t right;
        int right_resolved;
        if (!parse_add(parser, &right, &right_resolved)) return 0;
        if (right_resolved && right >= 64)
            return parser_error(parser, operator_token,
                                "expression shift must be between 0 and 63");
        int both = *resolved && right_resolved;
        if (both)
            *value = operation == ASM_TOKEN_SHIFT_LEFT
                         ? *value << right : *value >> right;
        else *value = 0;
        *resolved = both;
    }
    return 1;
}

static int parse_and(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_shift(parser, value, resolved)) return 0;
    while (consume_token(parser, ASM_TOKEN_AMPERSAND)) {
        uint64_t right;
        int right_resolved;
        if (!parse_shift(parser, &right, &right_resolved)) return 0;
        *value &= right;
        *resolved = *resolved && right_resolved;
    }
    return 1;
}

static int parse_xor(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_and(parser, value, resolved)) return 0;
    while (consume_token(parser, ASM_TOKEN_CARET)) {
        uint64_t right;
        int right_resolved;
        if (!parse_and(parser, &right, &right_resolved)) return 0;
        *value ^= right;
        *resolved = *resolved && right_resolved;
    }
    return 1;
}

static int parse_expression(LineParser *parser, uint64_t *value, int *resolved)
{
    if (!parse_xor(parser, value, resolved)) return 0;
    while (consume_token(parser, ASM_TOKEN_PIPE)) {
        uint64_t right;
        int right_resolved;
        if (!parse_xor(parser, &right, &right_resolved)) return 0;
        *value |= right;
        *resolved = *resolved && right_resolved;
    }
    return 1;
}

static int parse_register(LineParser *parser, uint8_t *reg)
{
    const AsmToken *token = current_token(parser);
    if (token->type != ASM_TOKEN_IDENTIFIER ||
        !asm_parse_register(token->text, reg)) {
        return parser_error(parser, token, "expected register R0-R15 or SP");
    }
    ++parser->index;
    return 1;
}

static int parse_comma_register(LineParser *parser, uint8_t *reg)
{
    return expect_token(parser,
                        ASM_TOKEN_COMMA,
                        "expected ',' between operands") &&
           parse_register(parser, reg);
}

static int parse_vector_register(LineParser *parser, uint8_t *reg)
{
    const AsmToken *token = current_token(parser);
    if (token->type != ASM_TOKEN_IDENTIFIER ||
        !asm_parse_vector_register(token->text, reg)) {
        return parser_error(parser, token, "expected vector register V0-V15");
    }
    ++parser->index;
    return 1;
}

static int parse_comma_vector_register(LineParser *parser, uint8_t *reg)
{
    return expect_token(parser,
                        ASM_TOKEN_COMMA,
                        "expected ',' between operands") &&
           parse_vector_register(parser, reg);
}

static int parse_comma_expression(LineParser *parser,
                                  uint64_t *value,
                                  int *resolved)
{
    return expect_token(parser,
                        ASM_TOKEN_COMMA,
                        "expected ',' between operands") &&
           parse_expression(parser, value, resolved);
}

static int value_fits_encoded_width(uint64_t value, unsigned int bits)
{
    uint64_t unsigned_max = bits == 64
                                ? UINT64_MAX
                                : (UINT64_C(1) << bits) - 1;
    uint64_t signed_negative_min =
        UINT64_MAX - ((UINT64_C(1) << (bits - 1)) - 1);
    return value <= unsigned_max || value >= signed_negative_min;
}

static int add_output_size(LineParser *parser, size_t amount)
{
    AssemblyContext *context = parser->context;
    if (amount > ASM_MAX_OUTPUT_SIZE ||
        context->offset > ASM_MAX_OUTPUT_SIZE - amount ||
        context->base_address > UINT64_MAX -
                                    (uint64_t)(context->offset + amount)) {
        return parser_error(parser,
                            current_token(parser),
                            "assembled output is too large");
    }
    context->offset += amount;
    return 1;
}

static void write_little_endian(uint8_t *destination,
                                uint64_t value,
                                size_t width)
{
    for (size_t i = 0; i < width; ++i) {
        destination[i] = (uint8_t)(value >> (i * 8));
    }
}

static int relative_displacement(uint64_t target,
                                 uint64_t next_address,
                                 uint32_t *encoded)
{
    if (target >= next_address) {
        uint64_t distance = target - next_address;
        if (distance > INT32_MAX) {
            return 0;
        }
        *encoded = (uint32_t)distance;
        return 1;
    }
    uint64_t distance = next_address - target;
    if (distance > UINT64_C(0x80000000)) {
        return 0;
    }
    *encoded = (uint32_t)(UINT32_C(0) - (uint32_t)distance);
    return 1;
}

static int ensure_statement_end(LineParser *parser)
{
    return current_token(parser)->type == ASM_TOKEN_END
               ? 1
               : parser_error(parser,
                              current_token(parser),
                              "unexpected token after statement");
}

static int process_instruction(LineParser *parser,
                               const AsmInstructionSpec *instruction)
{
    AssemblyContext *context = parser->context;
    size_t instruction_size = asm_instruction_size(instruction->format);
    size_t start_offset = context->offset;
    uint8_t registers[4] = {0};
    uint8_t condition = 0;
    uint64_t immediate = 0;
    int immediate_resolved = 1;

    switch (instruction->format) {
        case ASM_FORMAT_NONE:
            break;
        case ASM_FORMAT_R:
            if (!parse_register(parser, &registers[0])) return 0;
            break;
        case ASM_FORMAT_RR:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_register(parser, &registers[1])) return 0;
            break;
        case ASM_FORMAT_RRR:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_register(parser, &registers[1]) ||
                !parse_comma_register(parser, &registers[2])) return 0;
            break;
        case ASM_FORMAT_RRRR:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_register(parser, &registers[1]) ||
                !parse_comma_register(parser, &registers[2]) ||
                !parse_comma_register(parser, &registers[3])) return 0;
            break;
        case ASM_FORMAT_R_IMM64:
        case ASM_FORMAT_R_IMM32U:
        case ASM_FORMAT_R_IMM32S:
        case ASM_FORMAT_R_U8:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_expression(parser,
                                        &immediate,
                                        &immediate_resolved)) return 0;
            break;
        case ASM_FORMAT_TARGET64:
        case ASM_FORMAT_REL32:
            if (!parse_expression(parser,
                                  &immediate,
                                  &immediate_resolved)) return 0;
            break;
        case ASM_FORMAT_RR_DISP32:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_register(parser, &registers[1]) ||
                !parse_comma_expression(parser,
                                        &immediate,
                                        &immediate_resolved)) return 0;
            break;
        case ASM_FORMAT_CC_REL32: {
            const AsmToken *condition_token = current_token(parser);
            if (condition_token->type == ASM_TOKEN_IDENTIFIER) {
                if (!asm_parse_condition(condition_token->text,
                                         &condition)) {
                    return parser_error(parser,
                                        condition_token,
                                        "unknown condition code");
                }
                ++parser->index;
            } else if (condition_token->type == ASM_TOKEN_NUMBER &&
                       condition_token->number < CPU_CONDITION_COUNT) {
                condition = (uint8_t)condition_token->number;
                ++parser->index;
            } else {
                return parser_error(parser,
                                    condition_token,
                                    "expected condition code");
            }
            if (!parse_comma_expression(parser,
                                        &immediate,
                                        &immediate_resolved)) return 0;
            break;
        }
        case ASM_FORMAT_V_R:
            if (!parse_vector_register(parser, &registers[0]) ||
                !parse_comma_register(parser, &registers[1])) return 0;
            break;
        case ASM_FORMAT_R_V:
            if (!parse_register(parser, &registers[0]) ||
                !parse_comma_vector_register(parser, &registers[1])) return 0;
            break;
        case ASM_FORMAT_VV:
            if (!parse_vector_register(parser, &registers[0]) ||
                !parse_comma_vector_register(parser, &registers[1])) return 0;
            break;
        case ASM_FORMAT_VVV:
            if (!parse_vector_register(parser, &registers[0]) ||
                !parse_comma_vector_register(parser, &registers[1]) ||
                !parse_comma_vector_register(parser, &registers[2])) return 0;
            break;
    }

    if (!ensure_statement_end(parser)) {
        return 0;
    }
    if (immediate_resolved) {
        unsigned int width = 64;
        if (instruction->format == ASM_FORMAT_R_IMM32U ||
            instruction->format == ASM_FORMAT_R_IMM32S ||
            instruction->format == ASM_FORMAT_RR_DISP32) {
            width = 32;
        } else if (instruction->format == ASM_FORMAT_R_U8) {
            width = 8;
        }
        if (!value_fits_encoded_width(immediate, width)) {
            return parser_error(parser,
                                current_token(parser),
                                "immediate value does not fit operand width");
        }
        if (instruction->format == ASM_FORMAT_R_U8 && immediate >= 64) {
            return parser_error(parser,
                                current_token(parser),
                                "shift amount must be between 0 and 63");
        }
    }

    uint32_t relative = 0;
    if ((instruction->format == ASM_FORMAT_REL32 ||
         instruction->format == ASM_FORMAT_CC_REL32) &&
        immediate_resolved) {
        uint64_t next_address = context->base_address +
                                start_offset + instruction_size;
        if (!relative_displacement(immediate,
                                   next_address,
                                   &relative)) {
            return parser_error(parser,
                                current_token(parser),
                                "relative branch target is out of range");
        }
    }

    if (!add_output_size(parser, instruction_size)) {
        return 0;
    }
    if (context->pass == 1) {
        return 1;
    }

    uint8_t *destination = context->output + start_offset;
    *destination++ = instruction->opcode;
    switch (instruction->format) {
        case ASM_FORMAT_NONE:
            break;
        case ASM_FORMAT_R:
            *destination = registers[0];
            break;
        case ASM_FORMAT_RR:
            destination[0] = registers[0];
            destination[1] = registers[1];
            break;
        case ASM_FORMAT_RRR:
            destination[0] = registers[0];
            destination[1] = registers[1];
            destination[2] = registers[2];
            break;
        case ASM_FORMAT_RRRR:
            destination[0] = registers[0];
            destination[1] = registers[1];
            destination[2] = registers[2];
            destination[3] = registers[3];
            break;
        case ASM_FORMAT_R_IMM64:
            destination[0] = registers[0];
            write_little_endian(destination + 1, immediate, 8);
            break;
        case ASM_FORMAT_R_IMM32U:
        case ASM_FORMAT_R_IMM32S:
            destination[0] = registers[0];
            write_little_endian(destination + 1, immediate, 4);
            break;
        case ASM_FORMAT_R_U8:
            destination[0] = registers[0];
            destination[1] = (uint8_t)immediate;
            break;
        case ASM_FORMAT_TARGET64:
            write_little_endian(destination, immediate, 8);
            break;
        case ASM_FORMAT_RR_DISP32:
            destination[0] = registers[0];
            destination[1] = registers[1];
            write_little_endian(destination + 2, immediate, 4);
            break;
        case ASM_FORMAT_REL32:
            write_little_endian(destination, relative, 4);
            break;
        case ASM_FORMAT_CC_REL32:
            destination[0] = condition;
            write_little_endian(destination + 1, relative, 4);
            break;
        case ASM_FORMAT_V_R:
        case ASM_FORMAT_R_V:
        case ASM_FORMAT_VV:
            destination[0] = registers[0];
            destination[1] = registers[1];
            break;
        case ASM_FORMAT_VVV:
            destination[0] = registers[0];
            destination[1] = registers[1];
            destination[2] = registers[2];
            break;
    }
    return 1;
}

static int process_data_directive(LineParser *parser,
                                  size_t width)
{
    int have_value = 0;
    while (current_token(parser)->type != ASM_TOKEN_END) {
        uint64_t value;
        int resolved;
        if (!parse_expression(parser, &value, &resolved)) {
            return 0;
        }
        if (resolved && !value_fits_encoded_width(value,
                                                  (unsigned int)(width * 8))) {
            return parser_error(parser,
                                current_token(parser),
                                "data value does not fit directive width");
        }
        size_t start = parser->context->offset;
        if (!add_output_size(parser, width)) {
            return 0;
        }
        if (parser->context->pass == 2) {
            write_little_endian(parser->context->output + start,
                                value,
                                width);
        }
        have_value = 1;
        if (!consume_token(parser, ASM_TOKEN_COMMA)) {
            break;
        }
    }
    if (!have_value) {
        return parser_error(parser,
                            current_token(parser),
                            "data directive requires a value");
    }
    return ensure_statement_end(parser);
}

static int process_string_directive(LineParser *parser, int zero_terminated)
{
    int have_string = 0;
    while (current_token(parser)->type != ASM_TOKEN_END) {
        const AsmToken *token = current_token(parser);
        if (token->type != ASM_TOKEN_STRING) {
            return parser_error(parser,
                                token,
                                "string directive requires a string literal");
        }
        size_t start = parser->context->offset;
        if (!add_output_size(parser, token->byte_count)) {
            return 0;
        }
        if (parser->context->pass == 2 && token->byte_count != 0) {
            memcpy(parser->context->output + start,
                   token->bytes,
                   token->byte_count);
        }
        ++parser->index;
        have_string = 1;
        if (!consume_token(parser, ASM_TOKEN_COMMA)) {
            break;
        }
    }
    if (!have_string) {
        return parser_error(parser,
                            current_token(parser),
                            "string directive requires a string literal");
    }
    if (zero_terminated) {
        size_t zero_offset = parser->context->offset;
        if (!add_output_size(parser, 1)) {
            return 0;
        }
        if (parser->context->pass == 2) {
            parser->context->output[zero_offset] = 0;
        }
    }
    return ensure_statement_end(parser);
}

static int process_org(LineParser *parser)
{
    uint64_t target;
    int resolved;
    if (!parse_expression(parser, &target, &resolved) ||
        !ensure_statement_end(parser)) {
        return 0;
    }
    if (!resolved) {
        return parser_error(parser,
                            current_token(parser),
                            ".org expression must be known in the first pass");
    }
    uint64_t current = parser->context->base_address +
                       parser->context->offset;
    if (target < current || target < parser->context->base_address) {
        return parser_error(parser,
                            current_token(parser),
                            ".org cannot move the location counter backwards");
    }
    uint64_t new_offset = target - parser->context->base_address;
    if (new_offset > ASM_MAX_OUTPUT_SIZE) {
        return parser_error(parser,
                            current_token(parser),
                            ".org target is too large");
    }
    parser->context->offset = (size_t)new_offset;
    return 1;
}

static int process_align(LineParser *parser)
{
    uint64_t alignment;
    int resolved;
    if (!parse_expression(parser, &alignment, &resolved)) {
        return 0;
    }
    uint64_t fill = 0;
    int fill_resolved = 1;
    if (consume_token(parser, ASM_TOKEN_COMMA) &&
        !parse_expression(parser, &fill, &fill_resolved)) {
        return 0;
    }
    if (!ensure_statement_end(parser)) {
        return 0;
    }
    if (!resolved || !fill_resolved) {
        return parser_error(parser,
                            current_token(parser),
                            ".align values must be known in the first pass");
    }
    if (alignment == 0 || alignment > ASM_MAX_OUTPUT_SIZE || fill > 0xFF) {
        return parser_error(parser,
                            current_token(parser),
                            "invalid .align value");
    }
    uint64_t current = parser->context->base_address +
                       parser->context->offset;
    size_t padding = (size_t)((alignment - current % alignment) % alignment);
    size_t start = parser->context->offset;
    if (!add_output_size(parser, padding)) {
        return 0;
    }
    if (parser->context->pass == 2 && padding != 0) {
        memset(parser->context->output + start, (int)fill, padding);
    }
    return 1;
}

static int process_space(LineParser *parser)
{
    uint64_t count;
    int count_resolved;
    if (!parse_expression(parser, &count, &count_resolved)) {
        return 0;
    }
    uint64_t fill = 0;
    int fill_resolved = 1;
    if (consume_token(parser, ASM_TOKEN_COMMA) &&
        !parse_expression(parser, &fill, &fill_resolved)) {
        return 0;
    }
    if (!ensure_statement_end(parser)) {
        return 0;
    }
    if (!count_resolved || !fill_resolved) {
        return parser_error(parser,
                            current_token(parser),
                            ".space values must be known in the first pass");
    }
    if (count > ASM_MAX_OUTPUT_SIZE || fill > 0xFF) {
        return parser_error(parser,
                            current_token(parser),
                            "invalid .space value");
    }
    size_t start = parser->context->offset;
    if (!add_output_size(parser, (size_t)count)) {
        return 0;
    }
    if (parser->context->pass == 2 && count != 0) {
        memset(parser->context->output + start, (int)fill, (size_t)count);
    }
    return 1;
}

static int process_zero(LineParser *parser)
{
    uint64_t count;
    int resolved;
    if (!parse_expression(parser, &count, &resolved) ||
        !ensure_statement_end(parser)) return 0;
    if (!resolved)
        return parser_error(parser, current_token(parser),
                            ".zero size must be known in the first pass");
    if (count > ASM_MAX_OUTPUT_SIZE)
        return parser_error(parser, current_token(parser),
                            "invalid .zero size");
    size_t start = parser->context->offset;
    if (!add_output_size(parser, (size_t)count)) return 0;
    if (parser->context->pass == 2 && count != 0)
        memset(parser->context->output + start, 0, (size_t)count);
    return 1;
}

static int process_entry(LineParser *parser)
{
    uint64_t entry;
    int resolved;
    if (!parse_expression(parser, &entry, &resolved) ||
        !ensure_statement_end(parser)) {
        return 0;
    }
    if (parser->context->pass == 2) {
        if (!resolved) {
            return parser_error(parser,
                                current_token(parser),
                                "unresolved entry point");
        }
        if (parser->context->result->entry_set) {
            return parser_error(parser,
                                current_token(parser),
                                "duplicate .entry directive");
        }
        parser->context->result->entry_address = entry;
        parser->context->result->entry_set = 1;
    }
    return 1;
}

static int process_equ(LineParser *parser)
{
    const AsmToken *name = current_token(parser);
    if (name->type != ASM_TOKEN_IDENTIFIER) {
        return parser_error(parser, name, ".equ requires a symbol name");
    }
    ++parser->index;
    if (!consume_token(parser, ASM_TOKEN_COMMA)) {
        return parser_error(parser, current_token(parser),
                            "expected ',' after .equ symbol");
    }
    uint64_t value;
    int resolved;
    if (!parse_expression(parser, &value, &resolved) ||
        !ensure_statement_end(parser)) return 0;
    if (!resolved) {
        return parser_error(parser, name,
                            ".equ expression must be known in the first pass");
    }
    if (parser->context->pass == 1) {
        if (!asm_symbol_define(parser->context->symbols, name->text, value)) {
            return parser_error(parser, name,
                                "duplicate .equ symbol or allocation failure");
        }
    } else {
        uint64_t previous;
        if (!asm_symbol_lookup(parser->context->symbols, name->text,
                               &previous) || previous != value) {
            return parser_error(parser, name,
                                ".equ value changed between assembly passes");
        }
    }
    return 1;
}

static int process_directive(LineParser *parser, const char *directive)
{
    if (asm_text_equal_ignore_case(directive, ".byte")) {
        return process_data_directive(parser, 1);
    }
    if (asm_text_equal_ignore_case(directive, ".word")) {
        return process_data_directive(parser, 2);
    }
    if (asm_text_equal_ignore_case(directive, ".dword")) {
        return process_data_directive(parser, 4);
    }
    if (asm_text_equal_ignore_case(directive, ".qword")) {
        return process_data_directive(parser, 8);
    }
    if (asm_text_equal_ignore_case(directive, ".ascii")) {
        return process_string_directive(parser, 0);
    }
    if (asm_text_equal_ignore_case(directive, ".asciz")) {
        return process_string_directive(parser, 1);
    }
    if (asm_text_equal_ignore_case(directive, ".org")) {
        return process_org(parser);
    }
    if (asm_text_equal_ignore_case(directive, ".align")) {
        return process_align(parser);
    }
    if (asm_text_equal_ignore_case(directive, ".space")) {
        return process_space(parser);
    }
    if (asm_text_equal_ignore_case(directive, ".zero")) {
        return process_zero(parser);
    }
    if (asm_text_equal_ignore_case(directive, ".entry")) {
        return process_entry(parser);
    }
    if (asm_text_equal_ignore_case(directive, ".equ") ||
        asm_text_equal_ignore_case(directive, ".set")) {
        return process_equ(parser);
    }
    return parser_error(parser,
                        &parser->tokens[parser->index - 1],
                        "unknown assembler directive");
}

static int process_line(AssemblyContext *context,
                        const AsmToken *tokens,
                        size_t token_count,
                        size_t line_number)
{
    LineParser parser = {
        .context = context,
        .tokens = tokens,
        .token_count = token_count,
        .line = line_number
    };
    if (current_token(&parser)->type == ASM_TOKEN_END) {
        return 1;
    }

    if (current_token(&parser)->type == ASM_TOKEN_IDENTIFIER &&
        parser.index + 1 < token_count &&
        tokens[parser.index + 1].type == ASM_TOKEN_COLON) {
        const AsmToken *label = current_token(&parser);
        parser.index += 2;
        if (context->pass == 1) {
            if (context->base_address > UINT64_MAX - context->offset) {
                return parser_error(&parser, label, "label address overflow");
            }
            if (!asm_symbol_define(context->symbols,
                                   label->text,
                                   context->base_address + context->offset)) {
                char message[256];
                (void)snprintf(message,
                               sizeof(message),
                               "duplicate label or allocation failure: '%s'",
                               label->text);
                return parser_error(&parser, label, message);
            }
        }
        if (current_token(&parser)->type == ASM_TOKEN_END) {
            return 1;
        }
    }

    const AsmToken *statement = current_token(&parser);
    if (statement->type != ASM_TOKEN_IDENTIFIER) {
        return parser_error(&parser,
                            statement,
                            "expected instruction, directive, or label");
    }
    ++parser.index;
    if (statement->text[0] == '.') {
        return process_directive(&parser, statement->text);
    }
    const AsmInstructionSpec *instruction =
        asm_instruction_find(statement->text);
    if (instruction == NULL) {
        char message[256];
        (void)snprintf(message,
                       sizeof(message),
                       "unknown instruction '%s'",
                       statement->text);
        return parser_error(&parser, statement, message);
    }
    return process_instruction(&parser, instruction);
}

static int run_pass(const char *source, AssemblyContext *context)
{
    const char *cursor = source;
    size_t line_number = 1;
    AsmToken *tokens = malloc(sizeof(*tokens) * ASM_MAX_TOKENS_PER_LINE);
    if (tokens == NULL) {
        return assembly_error_at(context,
                                 line_number,
                                 1,
                                 "failed to allocate lexer tokens");
    }
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t length = line_end != NULL
                            ? (size_t)(line_end - cursor)
                            : strlen(cursor);
        if (length != 0 && cursor[length - 1] == '\r') {
            --length;
        }
        char *line = malloc(length + 1);
        if (line == NULL) {
            free(tokens);
            return assembly_error_at(context,
                                     line_number,
                                     1,
                                     "failed to allocate source line");
        }
        memcpy(line, cursor, length);
        line[length] = '\0';

        size_t token_count;
        int lexed = asm_lex_line(line,
                                 line_number,
                                 tokens,
                                 &token_count,
                                 context->error);
        free(line);
        if (!lexed || !process_line(context,
                                    tokens,
                                    token_count,
                                    line_number)) {
            free(tokens);
            return 0;
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
        ++line_number;
    }
    free(tokens);
    return 1;
}

void assembly_result_destroy(AssemblyResult *result)
{
    if (result == NULL) {
        return;
    }
    free(result->data);
    for (size_t i = 0; i < result->symbol_count; ++i) {
        free(result->symbols[i].name);
    }
    free(result->symbols);
    *result = (AssemblyResult){0};
}

int assembler_assemble(const char *source,
                       uint64_t base_address,
                       AssemblyResult *result,
                       AssemblyError *error)
{
    if (source == NULL || result == NULL) {
        if (error != NULL) {
            *error = (AssemblyError){
                .line = 1,
                .column = 1
            };
            (void)snprintf(error->message,
                           sizeof(error->message),
                           "invalid assembler input");
        }
        return 0;
    }
    *result = (AssemblyResult){
        .base_address = base_address,
        .entry_address = base_address
    };
    if (error != NULL) {
        *error = (AssemblyError){0};
    }

    AsmSymbolTable symbols;
    asm_symbol_table_init(&symbols);
    AssemblyContext first = {
        .pass = 1,
        .base_address = base_address,
        .symbols = &symbols,
        .result = result,
        .error = error
    };
    if (!run_pass(source, &first) || first.offset == 0) {
        if (first.offset == 0 && error != NULL && error->message[0] == '\0') {
            (void)assembly_error_at(&first,
                                    1,
                                    1,
                                    "assembly produced no output");
        }
        asm_symbol_table_destroy(&symbols);
        return 0;
    }

    uint8_t *output = calloc(first.offset, 1);
    if (output == NULL) {
        (void)assembly_error_at(&first,
                                1,
                                1,
                                "failed to allocate assembled output");
        asm_symbol_table_destroy(&symbols);
        return 0;
    }
    result->data = output;
    result->size = first.offset;

    AssemblyContext second = {
        .pass = 2,
        .base_address = base_address,
        .final_size = first.offset,
        .output = output,
        .symbols = &symbols,
        .result = result,
        .error = error
    };
    if (!run_pass(source, &second) || second.offset != first.offset) {
        if (error != NULL && error->message[0] == '\0') {
            (void)assembly_error_at(&second,
                                    1,
                                    1,
                                    "assembler passes produced different sizes");
        }
        free(output);
        result->data = NULL;
        result->size = 0;
        asm_symbol_table_destroy(&symbols);
        return 0;
    }

    result->symbols = symbols.items;
    result->symbol_count = symbols.count;
    symbols.items = NULL;
    symbols.count = 0;
    symbols.capacity = 0;
    return 1;
}
