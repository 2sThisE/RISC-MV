#ifndef VM_ASSEMBLER_LEXER_H
#define VM_ASSEMBLER_LEXER_H

#include "assembler.h"

#define ASM_MAX_TOKENS_PER_LINE 256U
#define ASM_MAX_TOKEN_TEXT 128U
#define ASM_MAX_STRING_BYTES 2048U

typedef enum {
    ASM_TOKEN_END = 0,
    ASM_TOKEN_IDENTIFIER,
    ASM_TOKEN_NUMBER,
    ASM_TOKEN_STRING,
    ASM_TOKEN_COLON,
    ASM_TOKEN_COMMA,
    ASM_TOKEN_PLUS,
    ASM_TOKEN_MINUS,
    ASM_TOKEN_LPAREN,
    ASM_TOKEN_RPAREN
} AsmTokenType;

typedef struct {
    AsmTokenType type;
    size_t column;
    char text[ASM_MAX_TOKEN_TEXT];
    uint64_t number;
    uint8_t bytes[ASM_MAX_STRING_BYTES];
    size_t byte_count;
} AsmToken;

int asm_lex_line(const char *line,
                 size_t line_number,
                 AsmToken tokens[ASM_MAX_TOKENS_PER_LINE],
                 size_t *token_count,
                 AssemblyError *error);

#endif
