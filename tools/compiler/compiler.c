#include "compiler.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_PUNCT,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_INT,
    TOK_LONG,
    TOK_CHAR,
    TOK_SHORT,
    TOK_VOID,
    TOK_SIGNED,
    TOK_UNSIGNED,
    TOK_EXTERN,
    TOK_STATIC
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;
    size_t length;
    uint64_t number;
    size_t line;
    size_t column;
} Token;

typedef enum {
    TY_VOID,
    TY_I8,
    TY_U8,
    TY_I16,
    TY_U16,
    TY_I32,
    TY_U32,
    TY_I64,
    TY_U64
} TypeKind;

typedef struct {
    TypeKind kind;
} Type;

typedef enum {
    EX_NUMBER,
    EX_VARIABLE,
    EX_ASSIGN,
    EX_BINARY,
    EX_UNARY,
    EX_CALL
} ExprKind;

typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    Token token;
    Type type;
    uint64_t number;
    char op[3];
    char *name;
    Expr *left;
    Expr *right;
    Expr **args;
    size_t arg_count;
};

typedef enum {
    ST_BLOCK,
    ST_RETURN,
    ST_IF,
    ST_WHILE,
    ST_EXPR,
    ST_DECL,
    ST_EMPTY
} StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    Token token;
    Expr *expr;
    Expr *init;
    Stmt *then_branch;
    Stmt *else_branch;
    Stmt **items;
    size_t item_count;
    char *name;
    Type type;
    int offset;
};

typedef struct {
    char *name;
    Type type;
    int offset;
    int is_parameter;
} Local;

typedef struct {
    char *name;
    Type return_type;
    Local *locals;
    size_t local_count;
    size_t param_count;
    Stmt *body;
    size_t frame_size;
    int is_static;
} Function;

typedef struct {
    char *name;
    Type type;
    int is_extern;
    int has_initializer;
    uint64_t initializer;
} Global;

typedef struct Allocation Allocation;
struct Allocation {
    void *pointer;
    Allocation *next;
};

typedef struct {
    const char *source;
    const char *cursor;
    size_t line;
    size_t column;
    Token current;
    CvmCompilerError *error;
    int failed;
    Allocation *allocations;
    Function *current_function;
    Function *functions;
    size_t function_count;
    Global *globals;
    size_t global_count;
} Parser;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} Buffer;

typedef struct {
    Buffer output;
    Parser *parser;
    Function *function;
    unsigned label_id;
    size_t temporary_depth;
    unsigned return_label;
} Generator;

static Type make_type(TypeKind kind)
{
    Type type;
    type.kind = kind;
    return type;
}

static size_t type_size(Type type)
{
    switch (type.kind) {
        case TY_I8:
        case TY_U8: return 1;
        case TY_I16:
        case TY_U16: return 2;
        case TY_I32:
        case TY_U32: return 4;
        case TY_I64:
        case TY_U64: return 8;
        case TY_VOID: return 0;
    }
    return 0;
}

static int type_is_signed(Type type)
{
    return type.kind == TY_I8 || type.kind == TY_I16 ||
           type.kind == TY_I32 || type.kind == TY_I64;
}

static void parser_error_at(Parser *parser, const Token *token,
                            const char *format, ...)
{
    if (parser->failed) {
        return;
    }
    parser->failed = 1;
    parser->error->line = token != NULL ? token->line : parser->line;
    parser->error->column = token != NULL ? token->column : parser->column;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(parser->error->message, sizeof(parser->error->message),
              format, arguments);
    va_end(arguments);
}

static void *parser_alloc(Parser *parser, size_t size)
{
    void *pointer = calloc(1, size);
    Allocation *allocation = malloc(sizeof(*allocation));
    if (pointer == NULL || allocation == NULL) {
        free(pointer);
        free(allocation);
        parser_error_at(parser, NULL, "out of memory");
        return NULL;
    }
    allocation->pointer = pointer;
    allocation->next = parser->allocations;
    parser->allocations = allocation;
    return pointer;
}

static char *parser_copy_text(Parser *parser, const char *text, size_t length)
{
    char *copy = parser_alloc(parser, length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static int token_text_is(const Token *token, const char *text)
{
    size_t length = strlen(text);
    return token->length == length &&
           memcmp(token->start, text, length) == 0;
}

static TokenKind keyword_kind(const char *text, size_t length)
{
    struct Keyword { const char *name; TokenKind kind; };
    static const struct Keyword keywords[] = {
        {"return", TOK_RETURN}, {"if", TOK_IF}, {"else", TOK_ELSE},
        {"while", TOK_WHILE}, {"int", TOK_INT}, {"long", TOK_LONG},
        {"char", TOK_CHAR}, {"short", TOK_SHORT}, {"void", TOK_VOID},
        {"signed", TOK_SIGNED}, {"unsigned", TOK_UNSIGNED},
        {"extern", TOK_EXTERN}, {"static", TOK_STATIC}
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strlen(keywords[i].name) == length &&
            memcmp(keywords[i].name, text, length) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

static void advance_character(Parser *parser)
{
    if (*parser->cursor == '\n') {
        ++parser->line;
        parser->column = 1;
    } else {
        ++parser->column;
    }
    ++parser->cursor;
}

static void skip_space_and_comments(Parser *parser)
{
    for (;;) {
        while (isspace((unsigned char)*parser->cursor)) {
            advance_character(parser);
        }
        if (*parser->cursor == '#' && parser->column == 1) {
            while (*parser->cursor != '\0' && *parser->cursor != '\n') {
                advance_character(parser);
            }
            continue;
        }
        if (parser->cursor[0] == '/' && parser->cursor[1] == '/') {
            while (*parser->cursor != '\0' && *parser->cursor != '\n') {
                advance_character(parser);
            }
            continue;
        }
        if (parser->cursor[0] == '/' && parser->cursor[1] == '*') {
            advance_character(parser);
            advance_character(parser);
            while (*parser->cursor != '\0' &&
                   !(parser->cursor[0] == '*' && parser->cursor[1] == '/')) {
                advance_character(parser);
            }
            if (*parser->cursor == '\0') {
                parser_error_at(parser, NULL, "unterminated block comment");
                return;
            }
            advance_character(parser);
            advance_character(parser);
            continue;
        }
        break;
    }
}

static void next_token(Parser *parser)
{
    skip_space_and_comments(parser);
    Token token;
    memset(&token, 0, sizeof(token));
    token.start = parser->cursor;
    token.line = parser->line;
    token.column = parser->column;
    if (*parser->cursor == '\0') {
        token.kind = TOK_EOF;
        parser->current = token;
        return;
    }
    if (isalpha((unsigned char)*parser->cursor) || *parser->cursor == '_') {
        token.kind = TOK_IDENT;
        while (isalnum((unsigned char)*parser->cursor) ||
               *parser->cursor == '_') {
            advance_character(parser);
        }
        token.length = (size_t)(parser->cursor - token.start);
        token.kind = keyword_kind(token.start, token.length);
        parser->current = token;
        return;
    }
    if (isdigit((unsigned char)*parser->cursor)) {
        char *end = NULL;
        token.number = strtoull(parser->cursor, &end, 0);
        if (end == parser->cursor) {
            parser_error_at(parser, &token, "invalid integer literal");
            return;
        }
        while (parser->cursor < end) {
            advance_character(parser);
        }
        while (*parser->cursor == 'u' || *parser->cursor == 'U' ||
               *parser->cursor == 'l' || *parser->cursor == 'L') {
            advance_character(parser);
        }
        token.length = (size_t)(parser->cursor - token.start);
        token.kind = TOK_NUMBER;
        parser->current = token;
        return;
    }
    static const char *const double_punct[] = {
        "==", "!=", "<=", ">=", "<<", ">>", "&&", "||"
    };
    for (size_t i = 0; i < sizeof(double_punct) / sizeof(double_punct[0]); ++i) {
        if (parser->cursor[0] == double_punct[i][0] &&
            parser->cursor[1] == double_punct[i][1]) {
            advance_character(parser);
            advance_character(parser);
            token.kind = TOK_PUNCT;
            token.length = 2;
            parser->current = token;
            return;
        }
    }
    if (strchr("(){};,+-*/%<>=!~&|^", *parser->cursor) != NULL) {
        advance_character(parser);
        token.kind = TOK_PUNCT;
        token.length = 1;
        parser->current = token;
        return;
    }
    parser_error_at(parser, &token, "unsupported character '%c'",
                    *parser->cursor);
}

static int consume(Parser *parser, const char *text)
{
    if (parser->current.kind == TOK_PUNCT &&
        token_text_is(&parser->current, text)) {
        next_token(parser);
        return 1;
    }
    return 0;
}

static int consume_kind(Parser *parser, TokenKind kind)
{
    if (parser->current.kind == kind) {
        next_token(parser);
        return 1;
    }
    return 0;
}

static void expect(Parser *parser, const char *text)
{
    if (!consume(parser, text)) {
        parser_error_at(parser, &parser->current, "expected '%s'", text);
    }
}

static char *expect_identifier(Parser *parser, Token *token)
{
    if (parser->current.kind != TOK_IDENT) {
        parser_error_at(parser, &parser->current, "expected identifier");
        return NULL;
    }
    *token = parser->current;
    char *name = parser_copy_text(parser, token->start, token->length);
    next_token(parser);
    return name;
}

static int token_starts_type(TokenKind kind)
{
    return kind == TOK_INT || kind == TOK_LONG || kind == TOK_CHAR ||
           kind == TOK_SHORT || kind == TOK_VOID || kind == TOK_SIGNED ||
           kind == TOK_UNSIGNED;
}

static Type parse_type(Parser *parser)
{
    int is_unsigned = consume_kind(parser, TOK_UNSIGNED);
    if (!is_unsigned) {
        (void)consume_kind(parser, TOK_SIGNED);
    }
    if (consume_kind(parser, TOK_CHAR)) {
        return make_type(is_unsigned ? TY_U8 : TY_I8);
    }
    if (consume_kind(parser, TOK_SHORT)) {
        (void)consume_kind(parser, TOK_INT);
        return make_type(is_unsigned ? TY_U16 : TY_I16);
    }
    if (consume_kind(parser, TOK_LONG)) {
        (void)consume_kind(parser, TOK_LONG);
        (void)consume_kind(parser, TOK_INT);
        return make_type(is_unsigned ? TY_U64 : TY_I64);
    }
    if (consume_kind(parser, TOK_VOID)) {
        if (is_unsigned) {
            parser_error_at(parser, &parser->current,
                            "unsigned void is invalid");
        }
        return make_type(TY_VOID);
    }
    if (consume_kind(parser, TOK_INT) || is_unsigned) {
        return make_type(is_unsigned ? TY_U32 : TY_I32);
    }
    parser_error_at(parser, &parser->current, "expected type name");
    return make_type(TY_I32);
}

static Expr *new_expr(Parser *parser, ExprKind kind, Token token)
{
    Expr *expr = parser_alloc(parser, sizeof(*expr));
    if (expr != NULL) {
        expr->kind = kind;
        expr->token = token;
        expr->type = make_type(TY_I64);
    }
    return expr;
}

static Stmt *new_stmt(Parser *parser, StmtKind kind, Token token)
{
    Stmt *stmt = parser_alloc(parser, sizeof(*stmt));
    if (stmt != NULL) {
        stmt->kind = kind;
        stmt->token = token;
    }
    return stmt;
}

static Local *find_local(Function *function, const char *name)
{
    for (size_t i = function->local_count; i != 0; --i) {
        if (strcmp(function->locals[i - 1].name, name) == 0) {
            return &function->locals[i - 1];
        }
    }
    return NULL;
}

static Global *find_global(Parser *parser, const char *name)
{
    for (size_t i = 0; i < parser->global_count; ++i) {
        if (strcmp(parser->globals[i].name, name) == 0) {
            return &parser->globals[i];
        }
    }
    return NULL;
}

static int append_local(Parser *parser, Function *function, char *name,
                        Type type, int is_parameter, Token token)
{
    if (find_local(function, name) != NULL) {
        parser_error_at(parser, &token, "duplicate local '%s'", name);
        return 0;
    }
    size_t count = function->local_count + 1;
    Local *locals = parser_alloc(parser, count * sizeof(*locals));
    if (locals == NULL) {
        return 0;
    }
    if (function->local_count != 0) {
        memcpy(locals, function->locals,
               function->local_count * sizeof(*locals));
    }
    locals[count - 1].name = name;
    locals[count - 1].type = type;
    locals[count - 1].offset = -(int)(count * 8);
    locals[count - 1].is_parameter = is_parameter;
    function->locals = locals;
    function->local_count = count;
    return 1;
}

static Expr *parse_expression(Parser *parser);

static Expr *parse_primary(Parser *parser)
{
    Token token = parser->current;
    if (consume(parser, "(")) {
        Expr *expr = parse_expression(parser);
        expect(parser, ")");
        return expr;
    }
    if (token.kind == TOK_NUMBER) {
        next_token(parser);
        Expr *expr = new_expr(parser, EX_NUMBER, token);
        if (expr != NULL) {
            expr->number = token.number;
            expr->type = token.number <= INT32_MAX
                             ? make_type(TY_I32) : make_type(TY_I64);
        }
        return expr;
    }
    if (token.kind != TOK_IDENT) {
        parser_error_at(parser, &token, "expected expression");
        return NULL;
    }
    char *name = parser_copy_text(parser, token.start, token.length);
    next_token(parser);
    if (consume(parser, "(")) {
        Expr *call = new_expr(parser, EX_CALL, token);
        call->name = name;
        while (!parser->failed && !consume(parser, ")")) {
            if (call->arg_count == 8) {
                parser_error_at(parser, &token,
                                "more than 8 call arguments are not yet supported");
                break;
            }
            Expr **args = parser_alloc(parser,
                (call->arg_count + 1) * sizeof(*args));
            if (args == NULL) {
                break;
            }
            if (call->arg_count != 0) {
                memcpy(args, call->args, call->arg_count * sizeof(*args));
            }
            args[call->arg_count++] = parse_expression(parser);
            call->args = args;
            if (!consume(parser, ",")) {
                expect(parser, ")");
                break;
            }
        }
        return call;
    }
    Expr *variable = new_expr(parser, EX_VARIABLE, token);
    variable->name = name;
    Local *local = parser->current_function != NULL
                       ? find_local(parser->current_function, name) : NULL;
    Global *global = find_global(parser, name);
    if (local != NULL) {
        variable->type = local->type;
    } else if (global != NULL) {
        variable->type = global->type;
    } else {
        parser_error_at(parser, &token, "unknown variable '%s'", name);
    }
    return variable;
}

static Expr *parse_unary(Parser *parser)
{
    Token token = parser->current;
    if (consume(parser, "+")) {
        return parse_unary(parser);
    }
    if (consume(parser, "-") || consume(parser, "!") ||
        consume(parser, "~")) {
        Expr *expr = new_expr(parser, EX_UNARY, token);
        expr->op[0] = token.start[0];
        expr->op[1] = '\0';
        expr->left = parse_unary(parser);
        return expr;
    }
    return parse_primary(parser);
}

static Expr *parse_binary(Parser *parser, Expr *(*sub)(Parser *),
                          const char *const *operators, size_t count)
{
    Expr *left = sub(parser);
    for (;;) {
        size_t selected = count;
        for (size_t i = 0; i < count; ++i) {
            if (parser->current.kind == TOK_PUNCT &&
                token_text_is(&parser->current, operators[i])) {
                selected = i;
                break;
            }
        }
        if (selected == count) {
            return left;
        }
        Token token = parser->current;
        next_token(parser);
        Expr *expr = new_expr(parser, EX_BINARY, token);
        memcpy(expr->op, operators[selected], strlen(operators[selected]) + 1);
        expr->left = left;
        expr->right = sub(parser);
        expr->type = left != NULL ? left->type : make_type(TY_I64);
        left = expr;
    }
}

static Expr *parse_multiply(Parser *parser)
{
    static const char *const ops[] = {"*", "/", "%"};
    return parse_binary(parser, parse_unary, ops, 3);
}

static Expr *parse_add(Parser *parser)
{
    static const char *const ops[] = {"+", "-"};
    return parse_binary(parser, parse_multiply, ops, 2);
}

static Expr *parse_shift(Parser *parser)
{
    static const char *const ops[] = {"<<", ">>"};
    return parse_binary(parser, parse_add, ops, 2);
}

static Expr *parse_relational(Parser *parser)
{
    static const char *const ops[] = {"<", "<=", ">", ">="};
    return parse_binary(parser, parse_shift, ops, 4);
}

static Expr *parse_equality(Parser *parser)
{
    static const char *const ops[] = {"==", "!="};
    return parse_binary(parser, parse_relational, ops, 2);
}

static Expr *parse_bit_and(Parser *parser)
{
    static const char *const ops[] = {"&"};
    return parse_binary(parser, parse_equality, ops, 1);
}

static Expr *parse_bit_xor(Parser *parser)
{
    static const char *const ops[] = {"^"};
    return parse_binary(parser, parse_bit_and, ops, 1);
}

static Expr *parse_bit_or(Parser *parser)
{
    static const char *const ops[] = {"|"};
    return parse_binary(parser, parse_bit_xor, ops, 1);
}

static Expr *parse_logical_and(Parser *parser)
{
    static const char *const ops[] = {"&&"};
    Expr *expr = parse_binary(parser, parse_bit_or, ops, 1);
    if (expr != NULL && expr->kind == EX_BINARY &&
        strcmp(expr->op, "&&") == 0) {
        expr->type = make_type(TY_I32);
    }
    return expr;
}

static Expr *parse_logical_or(Parser *parser)
{
    static const char *const ops[] = {"||"};
    Expr *expr = parse_binary(parser, parse_logical_and, ops, 1);
    if (expr != NULL && expr->kind == EX_BINARY &&
        strcmp(expr->op, "||") == 0) {
        expr->type = make_type(TY_I32);
    }
    return expr;
}

static Expr *parse_assignment(Parser *parser)
{
    Expr *left = parse_logical_or(parser);
    if (!consume(parser, "=")) {
        return left;
    }
    if (left == NULL || left->kind != EX_VARIABLE) {
        parser_error_at(parser, left != NULL ? &left->token : &parser->current,
                        "assignment target must be a variable");
        return left;
    }
    Expr *expr = new_expr(parser, EX_ASSIGN, left->token);
    expr->left = left;
    expr->right = parse_assignment(parser);
    expr->type = left->type;
    return expr;
}

static Expr *parse_expression(Parser *parser)
{
    return parse_assignment(parser);
}

static Stmt *parse_statement(Parser *parser);

static Stmt *parse_block(Parser *parser, Token token)
{
    Stmt *block = new_stmt(parser, ST_BLOCK, token);
    while (!parser->failed && !consume(parser, "}")) {
        if (parser->current.kind == TOK_EOF) {
            parser_error_at(parser, &parser->current, "expected '}'");
            break;
        }
        Stmt **items = parser_alloc(parser,
            (block->item_count + 1) * sizeof(*items));
        if (items == NULL) {
            break;
        }
        if (block->item_count != 0) {
            memcpy(items, block->items, block->item_count * sizeof(*items));
        }
        items[block->item_count++] = parse_statement(parser);
        block->items = items;
    }
    return block;
}

static Stmt *parse_statement(Parser *parser)
{
    Token token = parser->current;
    if (consume(parser, "{")) {
        return parse_block(parser, token);
    }
    if (consume_kind(parser, TOK_RETURN)) {
        Stmt *stmt = new_stmt(parser, ST_RETURN, token);
        if (!consume(parser, ";")) {
            stmt->expr = parse_expression(parser);
            expect(parser, ";");
        }
        return stmt;
    }
    if (consume_kind(parser, TOK_IF)) {
        Stmt *stmt = new_stmt(parser, ST_IF, token);
        expect(parser, "(");
        stmt->expr = parse_expression(parser);
        expect(parser, ")");
        stmt->then_branch = parse_statement(parser);
        if (consume_kind(parser, TOK_ELSE)) {
            stmt->else_branch = parse_statement(parser);
        }
        return stmt;
    }
    if (consume_kind(parser, TOK_WHILE)) {
        Stmt *stmt = new_stmt(parser, ST_WHILE, token);
        expect(parser, "(");
        stmt->expr = parse_expression(parser);
        expect(parser, ")");
        stmt->then_branch = parse_statement(parser);
        return stmt;
    }
    if (token_starts_type(parser->current.kind)) {
        Type type = parse_type(parser);
        Token name_token;
        char *name = expect_identifier(parser, &name_token);
        if (type.kind == TY_VOID) {
            parser_error_at(parser, &name_token,
                            "local variable cannot have void type");
        }
        append_local(parser, parser->current_function, name, type, 0,
                     name_token);
        Local *local = find_local(parser->current_function, name);
        Stmt *stmt = new_stmt(parser, ST_DECL, token);
        stmt->name = name;
        stmt->type = type;
        stmt->offset = local != NULL ? local->offset : 0;
        if (consume(parser, "=")) {
            stmt->init = parse_expression(parser);
        }
        expect(parser, ";");
        return stmt;
    }
    if (consume(parser, ";")) {
        return new_stmt(parser, ST_EMPTY, token);
    }
    Stmt *stmt = new_stmt(parser, ST_EXPR, token);
    stmt->expr = parse_expression(parser);
    expect(parser, ";");
    return stmt;
}

static int append_function(Parser *parser, Function function)
{
    size_t count = parser->function_count + 1;
    Function *functions = parser_alloc(parser, count * sizeof(*functions));
    if (functions == NULL) {
        return 0;
    }
    if (parser->function_count != 0) {
        memcpy(functions, parser->functions,
               parser->function_count * sizeof(*functions));
    }
    functions[count - 1] = function;
    parser->functions = functions;
    parser->function_count = count;
    return 1;
}

static int append_global(Parser *parser, Global global, Token token)
{
    if (find_global(parser, global.name) != NULL) {
        parser_error_at(parser, &token, "duplicate global '%s'", global.name);
        return 0;
    }
    size_t count = parser->global_count + 1;
    Global *globals = parser_alloc(parser, count * sizeof(*globals));
    if (globals == NULL) {
        return 0;
    }
    if (parser->global_count != 0) {
        memcpy(globals, parser->globals,
               parser->global_count * sizeof(*globals));
    }
    globals[count - 1] = global;
    parser->globals = globals;
    parser->global_count = count;
    return 1;
}

static void parse_translation_unit(Parser *parser)
{
    next_token(parser);
    while (!parser->failed && parser->current.kind != TOK_EOF) {
        int is_extern = consume_kind(parser, TOK_EXTERN);
        int is_static = consume_kind(parser, TOK_STATIC);
        if (!token_starts_type(parser->current.kind)) {
            parser_error_at(parser, &parser->current,
                            "expected declaration or function definition");
            break;
        }
        Type type = parse_type(parser);
        Token name_token;
        char *name = expect_identifier(parser, &name_token);
        if (consume(parser, "(")) {
            Function function;
            memset(&function, 0, sizeof(function));
            function.name = name;
            function.return_type = type;
            function.is_static = is_static;
            parser->current_function = &function;
            if (!consume(parser, ")")) {
                if (parser->current.kind == TOK_VOID) {
                    next_token(parser);
                    expect(parser, ")");
                } else {
                    while (!parser->failed) {
                        Type param_type = parse_type(parser);
                        Token param_token;
                        char *param_name = expect_identifier(parser, &param_token);
                        if (param_type.kind == TY_VOID) {
                            parser_error_at(parser, &param_token,
                                            "parameter cannot have void type");
                        }
                        if (function.param_count == 8) {
                            parser_error_at(parser, &param_token,
                                            "more than 8 parameters are not yet supported");
                        }
                        append_local(parser, &function, param_name, param_type,
                                     1, param_token);
                        ++function.param_count;
                        if (!consume(parser, ",")) {
                            expect(parser, ")");
                            break;
                        }
                    }
                }
            }
            if (consume(parser, ";")) {
                parser->current_function = NULL;
                continue;
            }
            if (is_extern) {
                parser_error_at(parser, &name_token,
                                "extern function cannot have a body");
                break;
            }
            if (!consume(parser, "{")) {
                parser_error_at(parser, &parser->current,
                                "expected function body");
                break;
            }
            function.body = parse_block(parser, name_token);
            function.frame_size = (function.local_count * 8 + 15) & ~(size_t)15;
            append_function(parser, function);
            parser->current_function = NULL;
            continue;
        }
        if (is_static) {
            parser_error_at(parser, &name_token,
                            "static global variables are not yet supported");
            break;
        }
        Global global;
        memset(&global, 0, sizeof(global));
        global.name = name;
        global.type = type;
        global.is_extern = is_extern;
        if (consume(parser, "=")) {
            if (parser->current.kind != TOK_NUMBER) {
                parser_error_at(parser, &parser->current,
                                "global initializer must be an integer constant");
            } else {
                global.has_initializer = 1;
                global.initializer = parser->current.number;
                next_token(parser);
            }
        }
        expect(parser, ";");
        append_global(parser, global, name_token);
    }
}

static int buffer_reserve(Buffer *buffer, size_t additional)
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
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static void emit(Generator *generator, const char *format, ...)
{
    if (generator->output.failed) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0 || !buffer_reserve(&generator->output, (size_t)length)) {
        generator->output.failed = 1;
        va_end(arguments);
        return;
    }
    vsnprintf(generator->output.data + generator->output.length,
              generator->output.capacity - generator->output.length,
              format, arguments);
    generator->output.length += (size_t)length;
    va_end(arguments);
}

static void normalize_register(Generator *generator, const char *reg,
                               Type type)
{
    switch (type.kind) {
        case TY_I8: emit(generator, "    SEXT8 %s, %s\n", reg, reg); break;
        case TY_U8: emit(generator, "    ZEXT8 %s, %s\n", reg, reg); break;
        case TY_I16: emit(generator, "    SEXT16 %s, %s\n", reg, reg); break;
        case TY_U16: emit(generator, "    ZEXT16 %s, %s\n", reg, reg); break;
        case TY_I32: emit(generator, "    SEXT32 %s, %s\n", reg, reg); break;
        case TY_U32: emit(generator, "    ZEXT32 %s, %s\n", reg, reg); break;
        case TY_I64:
        case TY_U64:
        case TY_VOID: break;
    }
}

static Local *generator_local(Generator *generator, const char *name)
{
    return find_local(generator->function, name);
}

static void emit_load(Generator *generator, const char *destination,
                      const char *base, int offset, Type type)
{
    const char *instruction = "LOAD64O";
    switch (type.kind) {
        case TY_I8: instruction = "LOAD8SO"; break;
        case TY_U8: instruction = "LOAD8UO"; break;
        case TY_I16: instruction = "LOAD16SO"; break;
        case TY_U16: instruction = "LOAD16UO"; break;
        case TY_I32: instruction = "LOAD32SO"; break;
        case TY_U32: instruction = "LOAD32UO"; break;
        default: break;
    }
    emit(generator, "    %s %s, %s, %d\n", instruction, destination,
         base, offset);
}

static void emit_store(Generator *generator, const char *base,
                       const char *source, int offset, Type type)
{
    const char *instruction = "STORE64O";
    switch (type_size(type)) {
        case 1: instruction = "STORE8O"; break;
        case 2: instruction = "STORE16O"; break;
        case 4: instruction = "STORE32O"; break;
        default: break;
    }
    emit(generator, "    %s %s, %s, %d\n", instruction, base, source,
         offset);
}

static unsigned new_label(Generator *generator)
{
    return ++generator->label_id;
}

static void generate_expression(Generator *generator, Expr *expr);

static void generate_variable_load(Generator *generator, Expr *expr)
{
    Local *local = generator_local(generator, expr->name);
    if (local != NULL) {
        emit_load(generator, "R0", "R14", local->offset, local->type);
        return;
    }
    emit(generator, "    MOVI64 R13, %s\n", expr->name);
    emit_load(generator, "R0", "R13", 0, expr->type);
}

static void generate_variable_store(Generator *generator, Expr *variable)
{
    Local *local = generator_local(generator, variable->name);
    if (local != NULL) {
        emit_store(generator, "R14", "R0", local->offset, local->type);
        return;
    }
    emit(generator, "    MOVI64 R13, %s\n", variable->name);
    emit_store(generator, "R13", "R0", 0, variable->type);
}

static void generate_comparison(Generator *generator, const char *op,
                                Type operand_type)
{
    const char *jump = "JZ";
    if (strcmp(op, "!=") == 0) jump = "JNZ";
    else if (strcmp(op, "<") == 0) jump = type_is_signed(operand_type) ? "JLT" : "JLTU";
    else if (strcmp(op, "<=") == 0) jump = type_is_signed(operand_type) ? "JLE" : "JLEU";
    else if (strcmp(op, ">") == 0) jump = type_is_signed(operand_type) ? "JGT" : "JGTU";
    else if (strcmp(op, ">=") == 0) jump = type_is_signed(operand_type) ? "JGE" : "JGEU";
    unsigned true_label = new_label(generator);
    unsigned end_label = new_label(generator);
    emit(generator, "    CMP R0, R1\n    MOVI64 R0, 0\n");
    emit(generator, "    %s .L%u\n    JUMP .L%u\n", jump, true_label,
         end_label);
    emit(generator, ".L%u:\n    MOVI64 R0, 1\n.L%u:\n", true_label,
         end_label);
}

static void generate_call(Generator *generator, Expr *expr)
{
    for (size_t i = 0; i < expr->arg_count; ++i) {
        generate_expression(generator, expr->args[i]);
        emit(generator, "    PUSH R0\n");
        generator->temporary_depth += 8;
    }
    for (size_t i = expr->arg_count; i != 0; --i) {
        emit(generator, "    POP R%zu\n", i - 1);
        generator->temporary_depth -= 8;
    }
    int padded = (generator->temporary_depth & 15U) != 0;
    if (padded) {
        emit(generator, "    ADDI32 SP, -8\n");
        generator->temporary_depth += 8;
    }
    emit(generator, "    CALLREL %s\n", expr->name);
    if (padded) {
        emit(generator, "    ADDI32 SP, 8\n");
        generator->temporary_depth -= 8;
    }
}

static void generate_expression(Generator *generator, Expr *expr)
{
    if (expr == NULL) {
        return;
    }
    if (expr->kind == EX_NUMBER) {
        emit(generator, "    MOVI64 R0, 0x%llx\n",
             (unsigned long long)expr->number);
        return;
    }
    if (expr->kind == EX_VARIABLE) {
        generate_variable_load(generator, expr);
        return;
    }
    if (expr->kind == EX_ASSIGN) {
        generate_expression(generator, expr->right);
        normalize_register(generator, "R0", expr->type);
        generate_variable_store(generator, expr->left);
        return;
    }
    if (expr->kind == EX_CALL) {
        generate_call(generator, expr);
        return;
    }
    if (expr->kind == EX_UNARY) {
        generate_expression(generator, expr->left);
        if (strcmp(expr->op, "~") == 0) {
            emit(generator, "    NOT R0\n");
        } else if (strcmp(expr->op, "-") == 0) {
            emit(generator, "    MOVI64 R1, 0\n    SUB R1, R0\n    MOV R0, R1\n");
        } else {
            unsigned true_label = new_label(generator);
            unsigned end_label = new_label(generator);
            emit(generator, "    CMPI32 R0, 0\n    MOVI64 R0, 0\n");
            emit(generator, "    JZ .L%u\n    JUMP .L%u\n", true_label,
                 end_label);
            emit(generator, ".L%u:\n    MOVI64 R0, 1\n.L%u:\n", true_label,
                 end_label);
        }
        normalize_register(generator, "R0", expr->type);
        return;
    }
    if (strcmp(expr->op, "&&") == 0 || strcmp(expr->op, "||") == 0) {
        unsigned result_label = new_label(generator);
        unsigned end_label = new_label(generator);
        int is_and = strcmp(expr->op, "&&") == 0;
        generate_expression(generator, expr->left);
        emit(generator, "    CMPI32 R0, 0\n    %s .L%u\n",
             is_and ? "JZ" : "JNZ", result_label);
        generate_expression(generator, expr->right);
        emit(generator, "    CMPI32 R0, 0\n    %s .L%u\n",
             is_and ? "JZ" : "JNZ", result_label);
        emit(generator, "    MOVI64 R0, %d\n    JUMP .L%u\n.L%u:\n",
             is_and ? 1 : 0, end_label, result_label);
        emit(generator, "    MOVI64 R0, %d\n.L%u:\n",
             is_and ? 0 : 1, end_label);
        return;
    }
    generate_expression(generator, expr->left);
    emit(generator, "    PUSH R0\n");
    generator->temporary_depth += 8;
    generate_expression(generator, expr->right);
    emit(generator, "    MOV R1, R0\n    POP R0\n");
    generator->temporary_depth -= 8;
    if (strcmp(expr->op, "+") == 0) emit(generator, "    ADD R0, R1\n");
    else if (strcmp(expr->op, "-") == 0) emit(generator, "    SUB R0, R1\n");
    else if (strcmp(expr->op, "*") == 0) emit(generator, "    MUL R0, R1\n");
    else if (strcmp(expr->op, "/") == 0) emit(generator, "    %s R0, R1\n", type_is_signed(expr->type) ? "DIVS" : "DIVU");
    else if (strcmp(expr->op, "%") == 0) emit(generator, "    %s R0, R1\n", type_is_signed(expr->type) ? "MODS" : "MODU");
    else if (strcmp(expr->op, "&") == 0) emit(generator, "    AND R0, R1\n");
    else if (strcmp(expr->op, "|") == 0) emit(generator, "    OR R0, R1\n");
    else if (strcmp(expr->op, "^") == 0) emit(generator, "    XOR R0, R1\n");
    else if (strcmp(expr->op, "<<") == 0) emit(generator, "    SHLV R0, R1\n");
    else if (strcmp(expr->op, ">>") == 0) emit(generator, "    %s R0, R1\n", type_is_signed(expr->type) ? "SARV" : "SHRV");
    else generate_comparison(generator, expr->op, expr->left->type);
    if (strcmp(expr->op, "==") != 0 && strcmp(expr->op, "!=") != 0 &&
        strcmp(expr->op, "<") != 0 && strcmp(expr->op, "<=") != 0 &&
        strcmp(expr->op, ">") != 0 && strcmp(expr->op, ">=") != 0) {
        normalize_register(generator, "R0", expr->type);
    }
}

static void generate_statement(Generator *generator, Stmt *stmt)
{
    if (stmt == NULL || stmt->kind == ST_EMPTY) {
        return;
    }
    if (stmt->kind == ST_BLOCK) {
        for (size_t i = 0; i < stmt->item_count; ++i) {
            generate_statement(generator, stmt->items[i]);
        }
        return;
    }
    if (stmt->kind == ST_RETURN) {
        if (stmt->expr != NULL) {
            generate_expression(generator, stmt->expr);
            normalize_register(generator, "R0", generator->function->return_type);
        }
        emit(generator, "    JUMP .L%u\n", generator->return_label);
        return;
    }
    if (stmt->kind == ST_EXPR) {
        generate_expression(generator, stmt->expr);
        return;
    }
    if (stmt->kind == ST_DECL) {
        if (stmt->init != NULL) {
            generate_expression(generator, stmt->init);
            normalize_register(generator, "R0", stmt->type);
            emit_store(generator, "R14", "R0", stmt->offset, stmt->type);
        }
        return;
    }
    if (stmt->kind == ST_IF) {
        unsigned else_label = new_label(generator);
        unsigned end_label = new_label(generator);
        generate_expression(generator, stmt->expr);
        emit(generator, "    CMPI32 R0, 0\n    JZ .L%u\n", else_label);
        generate_statement(generator, stmt->then_branch);
        emit(generator, "    JUMP .L%u\n.L%u:\n", end_label, else_label);
        generate_statement(generator, stmt->else_branch);
        emit(generator, ".L%u:\n", end_label);
        return;
    }
    if (stmt->kind == ST_WHILE) {
        unsigned begin_label = new_label(generator);
        unsigned end_label = new_label(generator);
        emit(generator, ".L%u:\n", begin_label);
        generate_expression(generator, stmt->expr);
        emit(generator, "    CMPI32 R0, 0\n    JZ .L%u\n", end_label);
        generate_statement(generator, stmt->then_branch);
        emit(generator, "    JUMP .L%u\n.L%u:\n", begin_label, end_label);
    }
}

static void generate_global(Generator *generator, const Global *global)
{
    if (global->is_extern) {
        emit(generator, ".extern %s\n.type %s, object\n", global->name,
             global->name);
        return;
    }
    if (!global->has_initializer) {
        emit(generator, ".comm %s, %zu, %zu\n.type %s, object\n",
             global->name, type_size(global->type), type_size(global->type),
             global->name);
        return;
    }
    emit(generator, ".section .data\n.global %s\n.type %s, object\n%s:\n",
         global->name, global->name, global->name);
    switch (type_size(global->type)) {
        case 1: emit(generator, "    .byte 0x%llx\n", (unsigned long long)global->initializer); break;
        case 2: emit(generator, "    .word 0x%llx\n", (unsigned long long)global->initializer); break;
        case 4: emit(generator, "    .dword 0x%llx\n", (unsigned long long)global->initializer); break;
        default: emit(generator, "    .qword 0x%llx\n", (unsigned long long)global->initializer); break;
    }
    emit(generator, ".size %s, $ - %s\n", global->name, global->name);
}

static void generate_function(Generator *generator, Function *function)
{
    generator->function = function;
    generator->temporary_depth = 0;
    generator->return_label = new_label(generator);
    emit(generator, "\n.section .text\n");
    if (!function->is_static) {
        emit(generator, ".global %s\n", function->name);
    }
    emit(generator, ".type %s, function\n%s:\n",
         function->name, function->name);
    emit(generator, "    PUSH R14\n    MOV R14, SP\n");
    if (function->frame_size != 0) {
        emit(generator, "    ADDI32 SP, -%zu\n", function->frame_size);
    }
    for (size_t i = 0; i < function->param_count; ++i) {
        char source[32];
        snprintf(source, sizeof(source), "R%zu", i);
        emit_store(generator, "R14", source, function->locals[i].offset,
                   function->locals[i].type);
    }
    generate_statement(generator, function->body);
    if (function->return_type.kind != TY_VOID) {
        emit(generator, "    MOVI64 R0, 0\n");
    }
    emit(generator, ".L%u:\n    MOV SP, R14\n    POP R14\n    RET\n",
         generator->return_label);
    emit(generator, ".size %s, $ - %s\n", function->name, function->name);
}

static void free_parser_allocations(Parser *parser)
{
    Allocation *allocation = parser->allocations;
    while (allocation != NULL) {
        Allocation *next = allocation->next;
        free(allocation->pointer);
        free(allocation);
        allocation = next;
    }
}

int cvm_compile_c_source(const char *source, char **assembly,
                         CvmCompilerError *error)
{
    if (source == NULL || assembly == NULL || error == NULL) {
        return 0;
    }
    *assembly = NULL;
    memset(error, 0, sizeof(*error));
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    parser.source = source;
    parser.cursor = source;
    parser.line = 1;
    parser.column = 1;
    parser.error = error;
    parse_translation_unit(&parser);
    if (parser.failed) {
        free_parser_allocations(&parser);
        return 0;
    }
    Generator generator;
    memset(&generator, 0, sizeof(generator));
    generator.parser = &parser;
    emit(&generator, "; generated by cvmcc (CVM ABI v1)\n");
    for (size_t i = 0; i < parser.global_count; ++i) {
        generate_global(&generator, &parser.globals[i]);
    }
    for (size_t i = 0; i < parser.function_count; ++i) {
        generate_function(&generator, &parser.functions[i]);
    }
    if (generator.output.failed ||
        !buffer_reserve(&generator.output, 0)) {
        free(generator.output.data);
        error->line = 1;
        error->column = 1;
        snprintf(error->message, sizeof(error->message), "out of memory");
        free_parser_allocations(&parser);
        return 0;
    }
    generator.output.data[generator.output.length] = '\0';
    *assembly = generator.output.data;
    free_parser_allocations(&parser);
    return 1;
}
