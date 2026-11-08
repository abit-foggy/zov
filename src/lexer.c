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

#include "token.h"
#include "diag.h"
#include "ast.h"

typedef struct Keyword {
    const char *name;
    TokenKind kind;
} Keyword;

static const Keyword keywords[] = {
    {"fn", TK_FN}, {"let", TK_LET}, {"mut", TK_MUT}, {"type", TK_TYPE},
    {"struct", TK_STRUCT}, {"enum", TK_ENUM}, {"match", TK_MATCH},
    {"case", TK_CASE}, {"if", TK_IF}, {"else", TK_ELSE}, {"for", TK_FOR},
    {"while", TK_WHILE}, {"return", TK_RETURN}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"defer", TK_DEFER}, {"use", TK_USE},
    {"as", TK_AS}, {"export", TK_EXPORT}, {"nullable", TK_NULLABLE},
    {"null", TK_NULL}, {"true", TK_TRUE}, {"false", TK_FALSE},
    {"resource", TK_RESOURCE}, {"in", TK_IN}, {"test", TK_TEST},
    {"orelse", TK_ORELSE},
    {"void", TK_VOID}, {"i8", TK_I8}, {"i16", TK_I16}, {"i32", TK_I32},
    {"i64", TK_I64}, {"isize", TK_ISIZE}, {"u8", TK_U8}, {"u16", TK_U16},
    {"u32", TK_U32}, {"u64", TK_U64}, {"usize", TK_USIZE},
    {"f32", TK_F32}, {"f64", TK_F64}, {"bool", TK_BOOL},
    {"char", TK_CHAR_KW}, {"str", TK_STR_KW},
};

static TokenKind keyword_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if (zstr_eq(keywords[i].name, name))
            return keywords[i].kind;
    return TK_IDENT;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_ident0(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident(char c) { return is_ident0(c) || is_digit(c); }

static Span here(Lexer *lx, const char *at, const char *to)
{
    Span s;
    s.file = lx->file;
    s.line = lx->line;
    s.col = (size_t)(at - lx->bol) + 1;
    s.start = at;
    s.end = to; /* widened by composite exprs in the parser */
    return s;
}

static char peek(Lexer *lx) { return lx->pos < lx->end ? *lx->pos : '\0'; }
static char peek2(Lexer *lx)
{
    return (lx->pos + 1) < lx->end ? lx->pos[1] : '\0';
}

void lex_init(Lexer *lx, const char *src, const char *file)
{
    lx->pos = src;
    lx->end = src + strlen(src);
    lx->file = str_intern_c(file);
    lx->line = 1;
    lx->bol = src;
    memset(&lx->tok, 0, sizeof(lx->tok));
    lx->tok.suffix = str_intern_c("");
}

static void skip_ws(Lexer *lx)
{
    for (;;) {
        char c = peek(lx);
        if (c == '\n') {
            lx->line++;
            lx->pos++;
            lx->bol = lx->pos;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f'
                   || c == '\v') {
            lx->pos++;
        } else if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) != '\0' && peek(lx) != '\n')
                lx->pos++;
        } else if (c == '/' && peek2(lx) == '*') {
            const char *open = lx->pos;
            lx->pos += 2;
            for (;;) {
                if (peek(lx) == '\0') {
                    fatal_at(E0101, here(lx, open, open), "unterminated block comment");
                    return;
                }
                if (peek(lx) == '\n') {
                    lx->line++;
                    lx->pos++;
                    lx->bol = lx->pos;
                } else if (peek(lx) == '*' && peek2(lx) == '/') {
                    lx->pos += 2;
                    break;
                } else {
                    lx->pos++;
                }
            }
        } else {
            break;
        }
    }
}

/* Decode one escape sequence at lx->pos; returns byte value, advances pos. */
static uint32_t lex_escape(Lexer *lx)
{
    const char *at = lx->pos;
    lx->pos++; /* consume backslash */
    char c = peek(lx);
    switch (c) {
    case 'n': lx->pos++; return '\n';
    case 't': lx->pos++; return '\t';
    case 'r': lx->pos++; return '\r';
    case '0': lx->pos++; return '\0';
    case '\\': lx->pos++; return '\\';
    case '"': lx->pos++; return '"';
    case '\'': lx->pos++; return '\'';
    case '{':
        // '\{' is a literal brace: private marker, rewritten on emission
        lx->pos++;
        return '\x01';
    case 'x': {
        lx->pos++;
        uint32_t v = 0;
        for (int i = 0; i < 2; i++) {
            char h = peek(lx);
            if (!is_hex(h))
                fatal_at(E0102, here(lx, at, at), "invalid \\x escape");
            uint32_t d = (h >= '0' && h <= '9') ? (uint32_t)(h - '0') :
                         (h >= 'a' && h <= 'f') ? (uint32_t)(h - 'a' + 10) :
                         (uint32_t)(h - 'A' + 10);
            v = v * 16 + d;
            lx->pos++;
        }
        return v;
    }
    default:
        fatal_at(E0103, here(lx, at, at), "unknown escape sequence '\\%c'", c);
        return 0;
    }
}

static void lex_string(Lexer *lx)
{
    const char *at = lx->pos;
    lx->pos++; /* opening quote */
    DA(char) buf = {0};
    for (;;) {
        char c = peek(lx);
        if (c == '\0' || c == '\n')
            fatal_at(E0104, here(lx, at, at), "unterminated string literal");
        if (c == '"') {
            lx->pos++;
            break;
        }
        if (c == '\\') {
            uint32_t v = lex_escape(lx);
            char b = (char)v;
            da_push(&buf, b);
        } else {
            da_push(&buf, c);
            lx->pos++;
        }
    }
    lx->tok.kind = TK_STR;
    (void)at;
    char *dec = zalloc(buf.len + 1);
    if (buf.len)
        memcpy(dec, buf.items, buf.len);
    dec[buf.len] = '\0';
    free(buf.items);
    lx->tok.str_val = dec;
    lx->tok.str_len = buf.len;
}

static void lex_char(Lexer *lx)
{
    const char *at = lx->pos;
    lx->pos++;
    uint32_t v;
    if (peek(lx) == '\\') {
        v = lex_escape(lx);
    } else if (peek(lx) == '\0' || peek(lx) == '\n') {
        fatal_at(E0105, here(lx, at, at), "unterminated character literal");
        return;
    } else {
        v = (unsigned char)peek(lx);
        lx->pos++;
    }
    if (peek(lx) != '\'')
        fatal_at(E0105, here(lx, at, at), "unterminated character literal");
    lx->pos++;
    lx->tok.kind = TK_CHAR;
    lx->tok.char_val = v;
}

static int is_type_suffix(const char *s)
{
    static const char *const suffixes[] = {
        "i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64",
        "usize", "f32", "f64",
    };
    if (!*s)
        return 1;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (zstr_eq(suffixes[i], s))
            return 1;
    return 0;
}

static void lex_number(Lexer *lx)
{
    const char *at = lx->pos;
    int is_float = 0;
    int base = 10;
    const char *digits_start = at;
    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        base = 16;
        lx->pos += 2;
        digits_start = lx->pos;
        while (is_hex(peek(lx)) || peek(lx) == '_')
            lx->pos++;
    } else if (peek(lx) == '0' && (peek2(lx) == 'o' || peek2(lx) == 'O')) {
        base = 8;
        lx->pos += 2;
        digits_start = lx->pos;
        while ((peek(lx) >= '0' && peek(lx) <= '7') || peek(lx) == '_')
            lx->pos++;
    } else if (peek(lx) == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
        base = 2;
        lx->pos += 2;
        digits_start = lx->pos;
        while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_')
            lx->pos++;
    } else {
        while (is_digit(peek(lx)) || peek(lx) == '_')
            lx->pos++;
    }
    /* '..' is the range operator, never a float dot */
    if (peek(lx) == '.' && peek2(lx) != '.' && is_digit(peek2(lx))) {
        is_float = 1;
        lx->pos++;
        while (is_digit(peek(lx)) || peek(lx) == '_')
            lx->pos++;
    }
    if ((peek(lx) == 'e' || peek(lx) == 'E')
        && (is_digit(peek2(lx))
            || ((peek2(lx) == '+' || peek2(lx) == '-')
                && is_digit(lx->pos[2])))) {
        is_float = 1;
        lx->pos++;
        if (peek(lx) == '+' || peek(lx) == '-')
            lx->pos++;
        while (is_digit(peek(lx)) || peek(lx) == '_')
            lx->pos++;
    }
    const char *num_end = lx->pos;
    while (is_ident(peek(lx)))
        lx->pos++;
    const char *suf = str_intern(num_end, lx->pos);
    if (!is_type_suffix(suf))
        fatal_at(E0106, here(lx, at, at), "invalid numeric suffix '%s'", suf);
    if (is_float && *suf && !(*suf == 'f'))
        fatal_at(E0107, here(lx, at, at), "float literal with integer suffix '%s'", suf);

    // digit separators are stripped before the numeric conversion
    char buf[64];
    size_t n = 0;
    const char *start_p = is_float ? at : digits_start;
    for (const char *p = start_p; p < num_end && n < sizeof(buf) - 1; p++)
        if (*p != '_')
            buf[n++] = *p;
    buf[n] = '\0';
    if (is_float) {
        lx->tok.kind = TK_FLOAT;
        lx->tok.float_val = strtod(buf, NULL);
    } else {
        lx->tok.kind = TK_INT;
        lx->tok.int_val = strtoull(buf, NULL, base);
    }
    lx->tok.suffix = suf;
}

static void lex_ident(Lexer *lx)
{
    const char *start = lx->pos;
    while (is_ident(peek(lx)))
        lx->pos++;
    const char *name = str_intern(start, lx->pos);
    lx->tok.kind = keyword_lookup(name);
    lx->tok.name = name;
}

void lex_next(Lexer *lx)
{
    const char *tok_start = lx->pos;
    skip_ws(lx);
    Token *t = &lx->tok;
    memset(t, 0, sizeof(*t));
    t->suffix = str_intern_c("");
    t->start = lx->pos;
    t->span = here(lx, lx->pos, lx->pos);
    (void)tok_start;

    char c = peek(lx);
    if (c == '\0') {
        t->kind = TK_EOF;
        t->end = lx->pos;
        t->span.end = lx->pos;
        return;
    }
    if (is_ident0(c)) {
        lex_ident(lx);
        t->end = lx->pos;
        return;
    }
    if (is_digit(c)) {
        lex_number(lx);
        t->end = lx->pos;
        return;
    }
    switch (c) {
    case '"': lex_string(lx); break;
    case '\'': lex_char(lx); break;
    case ':':
        lx->pos++;
        if (peek(lx) == ':') { lx->pos++; t->kind = TK_COLONCOLON; }
        else t->kind = TK_COLON;
        break;
    case '=':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_EQ; }
        else if (peek(lx) == '>') { lx->pos++; t->kind = TK_FATARROW; }
        else if (peek(lx) == '+') { lx->pos++; t->kind = TK_PLUS_EQ; }
        else if (peek(lx) == '-') { lx->pos++; t->kind = TK_MINUS_EQ; }
        else if (peek(lx) == '*') { lx->pos++; t->kind = TK_STAR_EQ; }
        else if (peek(lx) == '/') { lx->pos++; t->kind = TK_SLASH_EQ; }
        else if (peek(lx) == '%') { lx->pos++; t->kind = TK_PERCENT_EQ; }
        else if (peek(lx) == '&') { lx->pos++; t->kind = TK_AMP_EQ; }
        else if (peek(lx) == '|') { lx->pos++; t->kind = TK_PIPE_EQ; }
        else if (peek(lx) == '^') { lx->pos++; t->kind = TK_CARET_EQ; }
        else t->kind = TK_ASSIGN;
        break;
    case '.':
        lx->pos++;
        if (peek(lx) == '.') { lx->pos++; t->kind = TK_DOTDOT; }
        else t->kind = TK_DOT;
        break;
    case '!':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_NEQ; }
        else t->kind = TK_BANG;
        break;
    case '<':
        lx->pos++;
        if (peek(lx) == '<') {
            lx->pos++;
            if (peek(lx) == '=') { lx->pos++; t->kind = TK_SHL_EQ; }
            else t->kind = TK_SHL;
        } else if (peek(lx) == '=') { lx->pos++; t->kind = TK_LE; }
        else t->kind = TK_LT;
        break;
    case '>':
        lx->pos++;
        if (peek(lx) == '>') {
            lx->pos++;
            if (peek(lx) == '=') { lx->pos++; t->kind = TK_SHR_EQ; }
            else t->kind = TK_SHR;
        } else if (peek(lx) == '=') { lx->pos++; t->kind = TK_GE; }
        else t->kind = TK_GT;
        break;
    case '&':
        lx->pos++;
        if (peek(lx) == '&') { lx->pos++; t->kind = TK_ANDAND; }
        else if (peek(lx) == '=') { lx->pos++; t->kind = TK_AMP_EQ; }
        else t->kind = TK_AMP;
        break;
    case '|':
        lx->pos++;
        if (peek(lx) == '|') { lx->pos++; t->kind = TK_OROR; }
        else if (peek(lx) == '=') { lx->pos++; t->kind = TK_PIPE_EQ; }
        else t->kind = TK_PIPE; /* union separator in type position */
        break;
    case '^':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_CARET_EQ; }
        else t->kind = TK_CARET;
        break;
    case '~':
        lx->pos++;
        t->kind = TK_TILDE;
        break;
    case '+':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_PLUS_EQ; }
        else t->kind = TK_PLUS;
        break;
    case '-':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_MINUS_EQ; }
        else t->kind = TK_MINUS;
        break;
    case '*':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_STAR_EQ; }
        else t->kind = TK_STAR;
        break;
    case '/':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_SLASH_EQ; }
        else t->kind = TK_SLASH;
        break;
    case '%':
        lx->pos++;
        if (peek(lx) == '=') { lx->pos++; t->kind = TK_PERCENT_EQ; }
        else t->kind = TK_PERCENT;
        break;
    case '?': t->kind = TK_QUESTION; lx->pos++; break;
    case '(': t->kind = TK_LPAREN; lx->pos++; break;
    case ')': t->kind = TK_RPAREN; lx->pos++; break;
    case '{': t->kind = TK_LBRACE; lx->pos++; break;
    case '}': t->kind = TK_RBRACE; lx->pos++; break;
    case '[': t->kind = TK_LBRACKET; lx->pos++; break;
    case ']': t->kind = TK_RBRACKET; lx->pos++; break;
    case ',': t->kind = TK_COMMA; lx->pos++; break;
    case ';': t->kind = TK_SEMI; lx->pos++; break;
    default:
        fatal_at(E0108, t->span, "unexpected character '%c'", c);
        return;
    }
    t->end = lx->pos;
    t->span.end = lx->pos;
}
