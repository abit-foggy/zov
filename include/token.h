/*
 * Copyright (c) 2026 foggy (abit-foggy)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ZOV_TOKEN_H
#define ZOV_TOKEN_H

#include "common.h"

typedef enum TokenKind {
    TK_EOF = 0,
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_STR,
    TK_CHAR,

    TK_FN, TK_LET, TK_MUT, TK_TYPE, TK_STRUCT, TK_ENUM, TK_MATCH, TK_CASE,
    TK_IF, TK_ELSE, TK_FOR, TK_WHILE, TK_RETURN, TK_BREAK, TK_CONTINUE,
    TK_DEFER, TK_USE, TK_AS, TK_EXPORT, TK_NULLABLE, TK_NULL, TK_TRUE,
    TK_FALSE,

    TK_VOID, TK_I8, TK_I16, TK_I32, TK_I64, TK_ISIZE,
    TK_U8, TK_U16, TK_U32, TK_U64, TK_USIZE,
    TK_F32, TK_F64, TK_BOOL, TK_CHAR_KW, TK_STR_KW,

    TK_COLONCOLON, TK_FATARROW, TK_DOTDOT, TK_EQ, TK_NEQ, TK_LE, TK_GE,
    TK_ANDAND, TK_OROR, TK_QUESTION, TK_BANG,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_LT, TK_GT, TK_ASSIGN,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE,
    TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_SEMI, TK_COLON, TK_AMP, TK_DOT,
} TokenKind;

typedef struct Token {
    TokenKind kind;
    const char *start;
    const char *end;
    Span span;
    const char *name;    /* interned identifier text */
    const char *suffix;  /* interned numeric suffix; "" when absent */
    uint64_t int_val;
    double float_val;
    uint32_t char_val;
    const char *str_val; /* decoded bytes, AST-arena owned */
    size_t str_len;
} Token;

typedef struct Lexer {
    const char *pos;
    const char *end;
    const char *file;
    size_t line;
    const char *bol; /* pointer to first column of current line */
    Token tok;
} Lexer;

void lex_init(Lexer *lx, const char *src, const char *file);
void lex_next(Lexer *lx);

#endif /* ZOV_TOKEN_H */
