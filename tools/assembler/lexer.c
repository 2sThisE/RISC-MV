#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int lexer_error(AssemblyError *error,
                       size_t line,
                       size_t column,
                       const char *message)
{
    if (error != NULL) {
        error->line = line;
        error->column = column;
        (void)snprintf(error->message,
                       sizeof(error->message),
                       "%s",
                       message);
    }
    return 0;
}

static int is_identifier_start(unsigned char character)
{
    return isalpha(character) || character == '_' ||
           character == '.' || character == '$';
}

static int is_identifier_continue(unsigned char character)
{
    return isalnum(character) || character == '_' ||
           character == '.' || character == '$';
}

static int hex_digit_value(unsigned char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = (unsigned char)tolower(character);
    return character >= 'a' && character <= 'f'
               ? character - 'a' + 10
               : -1;
}

static int parse_escape(const char *line,
                        size_t *cursor,
                        uint8_t *value,
                        size_t line_number,
                        AssemblyError *error)
{
    unsigned char escaped = (unsigned char)line[(*cursor)++];
    switch (escaped) {
        case '0': *value = 0; return 1;
        case 'n': *value = (uint8_t)'\n'; return 1;
        case 'r': *value = (uint8_t)'\r'; return 1;
        case 't': *value = (uint8_t)'\t'; return 1;
        case '\\': *value = (uint8_t)'\\'; return 1;
        case '\'': *value = (uint8_t)'\''; return 1;
        case '"': *value = (uint8_t)'"'; return 1;
        case 'x': {
            if (line[*cursor] == '\0' || line[*cursor + 1] == '\0') {
                return lexer_error(error,
                                   line_number,
                                   *cursor + 1,
                                   "expected two hexadecimal escape digits");
            }
            int high = hex_digit_value((unsigned char)line[*cursor]);
            int low = hex_digit_value((unsigned char)line[*cursor + 1]);
            if (high < 0 || low < 0) {
                return lexer_error(error,
                                   line_number,
                                   *cursor + 1,
                                   "expected two hexadecimal escape digits");
            }
            *cursor += 2;
            *value = (uint8_t)((high << 4) | low);
            return 1;
        }
        case '\0':
            return lexer_error(error,
                               line_number,
                               *cursor,
                               "unfinished escape sequence");
        default:
            return lexer_error(error,
                               line_number,
                               *cursor,
                               "unknown escape sequence");
    }
}

static int parse_number(const char *line,
                        size_t *cursor,
                        uint64_t *number,
                        size_t line_number,
                        AssemblyError *error)
{
    size_t start = *cursor;
    unsigned int base = 10;
    if (line[*cursor] == '0' &&
        (line[*cursor + 1] == 'x' || line[*cursor + 1] == 'X')) {
        base = 16;
        *cursor += 2;
    } else if (line[*cursor] == '0' &&
               (line[*cursor + 1] == 'b' || line[*cursor + 1] == 'B')) {
        base = 2;
        *cursor += 2;
    }

    size_t digit_start = *cursor;
    uint64_t value = 0;
    while (line[*cursor] != '\0') {
        unsigned char character = (unsigned char)line[*cursor];
        if (character == '_') {
            ++*cursor;
            continue;
        }
        int digit = hex_digit_value(character);
        if (digit < 0 || (unsigned int)digit >= base) {
            break;
        }
        if (value > (UINT64_MAX - (uint64_t)digit) / base) {
            return lexer_error(error,
                               line_number,
                               start + 1,
                               "integer literal overflow");
        }
        value = value * base + (uint64_t)digit;
        ++*cursor;
    }
    if (*cursor == digit_start) {
        return lexer_error(error,
                           line_number,
                           start + 1,
                           "expected digits after numeric prefix");
    }
    *number = value;
    return 1;
}

static int add_token(AsmToken tokens[ASM_MAX_TOKENS_PER_LINE],
                     size_t *count,
                     AsmToken token,
                     size_t line_number,
                     AssemblyError *error)
{
    if (*count >= ASM_MAX_TOKENS_PER_LINE - 1) {
        return lexer_error(error,
                           line_number,
                           token.column,
                           "too many tokens on one line");
    }
    tokens[(*count)++] = token;
    return 1;
}

int asm_lex_line(const char *line,
                 size_t line_number,
                 AsmToken tokens[ASM_MAX_TOKENS_PER_LINE],
                 size_t *token_count,
                 AssemblyError *error)
{
    if (line == NULL || tokens == NULL || token_count == NULL) {
        return lexer_error(error, line_number, 1, "invalid lexer input");
    }

    size_t count = 0;
    size_t cursor = 0;
    while (line[cursor] != '\0') {
        unsigned char character = (unsigned char)line[cursor];
        if (character == ';') {
            break;
        }
        if (isspace(character)) {
            ++cursor;
            continue;
        }

        AsmToken token = {
            .column = cursor + 1
        };
        if (is_identifier_start(character)) {
            token.type = ASM_TOKEN_IDENTIFIER;
            size_t length = 0;
            while (is_identifier_continue((unsigned char)line[cursor])) {
                if (length + 1 >= sizeof(token.text)) {
                    return lexer_error(error,
                                       line_number,
                                       token.column,
                                       "identifier is too long");
                }
                token.text[length++] = line[cursor++];
            }
            token.text[length] = '\0';
        } else if (isdigit(character)) {
            token.type = ASM_TOKEN_NUMBER;
            if (!parse_number(line,
                              &cursor,
                              &token.number,
                              line_number,
                              error)) {
                return 0;
            }
        } else if (character == '"') {
            token.type = ASM_TOKEN_STRING;
            ++cursor;
            while (line[cursor] != '"') {
                if (line[cursor] == '\0') {
                    return lexer_error(error,
                                       line_number,
                                       token.column,
                                       "unterminated string literal");
                }
                if (token.byte_count >= sizeof(token.bytes)) {
                    return lexer_error(error,
                                       line_number,
                                       token.column,
                                       "string literal is too long");
                }
                uint8_t value;
                if (line[cursor] == '\\') {
                    ++cursor;
                    if (!parse_escape(line,
                                      &cursor,
                                      &value,
                                      line_number,
                                      error)) {
                        return 0;
                    }
                } else {
                    value = (uint8_t)line[cursor++];
                }
                token.bytes[token.byte_count++] = value;
            }
            ++cursor;
        } else if (character == '\'') {
            token.type = ASM_TOKEN_NUMBER;
            ++cursor;
            if (line[cursor] == '\0' || line[cursor] == '\'') {
                return lexer_error(error,
                                   line_number,
                                   token.column,
                                   "empty or unterminated character literal");
            }
            uint8_t value;
            if (line[cursor] == '\\') {
                ++cursor;
                if (!parse_escape(line,
                                  &cursor,
                                  &value,
                                  line_number,
                                  error)) {
                    return 0;
                }
            } else {
                value = (uint8_t)line[cursor++];
            }
            if (line[cursor] != '\'') {
                return lexer_error(error,
                                   line_number,
                                   token.column,
                                   "character literal must contain one byte");
            }
            ++cursor;
            token.number = value;
        } else {
            ++cursor;
            switch (character) {
                case ':': token.type = ASM_TOKEN_COLON; break;
                case ',': token.type = ASM_TOKEN_COMMA; break;
                case '+': token.type = ASM_TOKEN_PLUS; break;
                case '-': token.type = ASM_TOKEN_MINUS; break;
                case '(': token.type = ASM_TOKEN_LPAREN; break;
                case ')': token.type = ASM_TOKEN_RPAREN; break;
                default:
                    return lexer_error(error,
                                       line_number,
                                       token.column,
                                       "unexpected character");
            }
        }

        if (!add_token(tokens,
                       &count,
                       token,
                       line_number,
                       error)) {
            return 0;
        }
    }

    tokens[count++] = (AsmToken){
        .type = ASM_TOKEN_END,
        .column = cursor + 1
    };
    *token_count = count;
    return 1;
}
