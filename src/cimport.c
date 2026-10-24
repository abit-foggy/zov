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

#define _POSIX_C_SOURCE 200809L

#include "cimport.h"
#include "common.h"

#include <sys/wait.h>

typedef struct CTok {
    const char *s;
    size_t n;
    int punct; /* punctuator token, s[0] is the character */
} CTok;

typedef DA(CTok) CTokList;

static int is_punct_ch(char c)
{
    return strchr(";*(),{}[].", c) != NULL;
}

static CTok tok_at(CTokList * t, size_t i)
{
    static const CTok eof_tok = { "", 0, 0 };
    return i < t->len ? t->items[i] : eof_tok;
}

static int tok_is(CTok *tok, const char *word)
{
    return !tok->punct && strlen(word) == tok->n
           && memcmp(tok->s, word, tok->n) == 0;
}

static int tok_is_punct(CTok *tok, char c)
{
    return tok->punct && tok->s[0] == c;
}

static char *tok_text(CTok *tok)
{
    char *s = zalloc(tok->n + 1);
    memcpy(s, tok->s, tok->n);
    s[tok->n] = '\0';
    return s;
}

/* ---- preprocessor invocation ---- */

static char *preprocess(const char *path, int angle)
{
    char cmd[512];
    if (angle)
        snprintf(cmd, sizeof(cmd),
                 "printf '#include <%s>\\n' | cc -E -P -dD -x c -", path);
    else
        snprintf(cmd, sizeof(cmd), "cc -E -P -dD -x c '%s'", path);
    if ((int)strlen(cmd) >= (int)sizeof(cmd))
        return NULL;
    FILE *out = popen(cmd, "r");
    if (!out)
        return NULL;
    DA(char) buf = {0};
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), out)) > 0) {
        for (size_t i = 0; i < n; i++)
            da_push(&buf, tmp[i]);
    }
    int st = pclose(out);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return NULL;
    if (!buf.items)
        return NULL;
    da_push(&buf, '\0');
    return buf.items;
}

/* ---- C scalar mapping ---- */

static Type *map_base(int u, int l, int s, int c, int i, int f, int d, int b,
                      int v)
{
    if (v)
        return type_prim(TY_VOID);
    if (b)
        return type_prim(TY_BOOL);
    if (f)
        return type_prim(TY_F32);
    if (d)
        return type_prim(TY_F64);
    if (c)
        return type_prim(u ? TY_U8 : TY_I8);
    if (s)
        return type_prim(u ? TY_U16 : TY_I16);
    if (l)
        return type_prim(u ? TY_U64 : TY_I64);
    if (i || u)
        return type_prim(u ? TY_U32 : TY_I32);
    return NULL;
}

static int base_word(CTok *t, int *u, int *l, int *s, int *c, int *i, int *f,
                     int *d, int *b, int *v)
{
    const char *w[] = {"unsigned", "long", "short", "char", "int", "float",
                       "double", "_Bool", "void"};
    int *flags[] = {u, l, s, c, i, f, d, b, v};
    for (int k = 0; k < 9; k++)
        if (tok_is(t, w[k])) {
            (*flags[k])++;
            return 1;
        }
    if (tok_is(t, "signed") || tok_is(t, "__signed")
        || tok_is(t, "__signed__"))
        return 1;
    return 0;
}

/* Aliases whose types we trust for pointer arithmetic-free v0.2 use. */
static Type *map_known_alias(const char *name)
{
    static const struct { const char *n; TypeKind k; } tab[] = {
        {"int8_t", TY_I8}, {"int16_t", TY_I16}, {"int32_t", TY_I32},
        {"int64_t", TY_I64}, {"ssize_t", TY_I64}, {"ptrdiff_t", TY_I64},
        {"intptr_t", TY_ISIZE},
        {"uint8_t", TY_U8}, {"uint16_t", TY_U16}, {"uint32_t", TY_U32},
        {"uint64_t", TY_U64}, {"size_t", TY_USIZE}, {"uintptr_t", TY_USIZE},
    };
    for (size_t k = 0; k < sizeof(tab) / sizeof(tab[0]); k++)
        if (strcmp(name, tab[k].n) == 0)
            return type_prim(tab[k].k);
    return NULL;
}

/* Opaque C type: emitted verbatim; the real header defines it. */
static Type *c_opaque(const char *name)
{
    Type *t = zalloc(sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = str_intern_c(name);
    t->c_name = t->name;
    return t;
}

static Type *c_struct_tag(const char *kw, const char *tag)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s", kw, tag);
    Type *t = zalloc(sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = str_intern_c(buf);
    t->c_name = t->name;
    return t;
}

/* ---- token stream helpers ---- */

static int is_qualifier(CTok *t)
{
    static const char *q[] = {"const", "volatile", "restrict", "__restrict",
                               "__restrict__", "__extension__", "register"};
    for (size_t k = 0; k < sizeof(q) / sizeof(q[0]); k++)
        if (tok_is(t, q[k]))
            return 1;
    return 0;
}

/* Consume `__attribute__((...))` / `__asm__(...)` noise starting at i. */
static size_t skip_gcc_noise(CTokList * t, size_t i)
{
    for (;;) {
        CTok tok = tok_at(t, i);
        if (tok_is(&tok, "__attribute__") || tok_is(&tok, "__asm__")
            || tok_is(&tok, "__asm")) {
            i++;
            int depth = 0;
            do {
                CTok x = tok_at(t, i);
                if (x.punct && x.s[0] == '(')
                    depth++;
                else if (x.punct && x.s[0] == ')')
                    depth--;
                else if (x.n == 0)
                    return i;
                i++;
            } while (depth > 0);
            continue;
        }
        if (is_qualifier(&tok)) {
            i++;
            continue;
        }
        return i;
    }
}

/* Parse one C type (base + stars) starting at i; returns Zov type and
 * advances i past the type. Unknown scalars become verbatim opaques. */
static Type *parse_c_type(CHeader *h, CTokList *t, size_t *ip)
{
    size_t i = *ip;
    i = skip_gcc_noise(t, i);
    CTok tok = tok_at(t, i);
    Type *base = NULL;
    if (tok_is(&tok, "struct") || tok_is(&tok, "union")
        || tok_is(&tok, "enum")) {
        char kw[8];
        snprintf(kw, sizeof(kw), "%.*s", (int)tok.n, tok.s);
        i++;
        CTok tag = tok_at(t, i);
        if (!tag.punct && tag.n > 0) {
            base = c_struct_tag(kw, tok_text(&tag));
            i++;
        } else {
            base = zalloc(sizeof(Type));
            base->kind = TY_STRUCT;
            base->c_name = str_intern_c(kw);
            base->name = base->c_name;
        }
    } else {
        int u = 0, l = 0, s = 0, c = 0, in = 0, f = 0, d = 0, b = 0, v = 0;
        int any = 0;
        for (;;) {
            tok = tok_at(t, i);
            if (base_word(&tok, &u, &l, &s, &c, &in, &f, &d, &b, &v)) {
                any = 1;
                i++;
                i = skip_gcc_noise(t, i);
                continue;
            }
            break;
        }
        if (any)
            base = map_base(u, l, s, c, in, f, d, b, v);
        if (!base) {
            /* typedef name: builtin alias, header typedef, or opaque */
            tok = tok_at(t, i);
            if (tok.punct || tok.n == 0)
                return NULL;
            char *name = tok_text(&tok);
            base = map_known_alias(name);
            if (!base) {
                CMember *al = cimport_find(h, name);
                if (al && al->kind == CM_TYPE)
                    base = al->ret;
            }
            if (!base)
                base = c_opaque(name);
            i++;
        }
    }
    /* pointer suffix */
    for (;;) {
        i = skip_gcc_noise(t, i);
        tok = tok_at(t, i);
        if (tok_is_punct(&tok, '*')) {
            base = type_ptr(base, 0);
            i++;
            continue;
        }
        break;
    }
    *ip = i;
    return base;
}

/* Parse a parameter list; returns 0 on unparsable signature. */
static int parse_params(CHeader *h, CTokList *t, size_t *ip,
                        TypeList *params, int *variadic)
{
    size_t i = *ip;
    CTok tok = tok_at(t, i);
    if (!tok_is_punct(&tok, '('))
        return 0;
    i++;
    /* void / empty */
    tok = tok_at(t, i);
    CTok rparen = tok_at(t, i + 1);
    if (tok_is(&tok, "void") && tok_is_punct(&rparen, ')')) {
        i += 2;
        *ip = i;
        return 1;
    }
    if (tok_is_punct(&tok, ')')) {
        i++;
        *ip = i;
        return 1;
    }
    for (;;) {
        tok = tok_at(t, i);
        CTok d1 = tok_at(t, i + 1);
        CTok d2 = tok_at(t, i + 2);
        if (tok_is_punct(&tok, '.') && tok_is_punct(&d1, '.')
            && tok_is_punct(&d2, '.')) {
            *variadic = 1;
            i += 3;
            break;
        }
        size_t j = i;
        Type *pt = parse_c_type(h, t, &j);
        if (!pt)
            return 0;
        /* optional declarator: name, arrays, function-pointer junk */
        for (;;) {
            CTok dt = tok_at(t, j);
            if (tok_is_punct(&dt, '(') || tok_is_punct(&dt, '[')) {
                int depth = 0;
                do {
                    if (dt.n == 0)
                        return 0;
                    if (dt.punct && (dt.s[0] == '(' || dt.s[0] == '['))
                        depth++;
                    else if (dt.punct && (dt.s[0] == ')' || dt.s[0] == ']'))
                        depth--;
                    j++;
                    dt = tok_at(t, j);
                } while (depth > 0);
                continue;
            }
            if (!dt.punct && dt.n > 0) {
                j++;
                continue;
            }
            break;
        }
        CTok sep = tok_at(t, j);
        if (tok_is_punct(&sep, ',')) {
            da_push(params, pt);
            i = j + 1;
            continue;
        }
        if (tok_is_punct(&sep, ')')) {
            da_push(params, pt);
            i = j; /* ')' is consumed by the close check below */
            break;
        }
        return 0;
    }
    CTok close_paren = tok_at(t, i);
    if (!tok_is_punct(&close_paren, ')'))
        return 0;
    i++;
    *ip = i;
    return 1;
}

static void header_add(CHeader *h, CMember m)
{
    if (cimport_find(h, m.name))
        return;
    da_push(&h->members, m);
}

/* Try to interpret head[0..len) as a function prototype. */
static void parse_prototype(CHeader *h, CTokList * t, size_t i, size_t end)
{
    CTok tok = tok_at(t, i);
    if (tok_is(&tok, "typedef"))
        return; /* typedefs handled separately */
    for (;;) {
        tok = tok_at(t, i);
        if (tok_is(&tok, "extern") || tok_is(&tok, "static")
            || is_qualifier(&tok) || tok_is(&tok, "inline")
            || tok_is(&tok, "__inline") || tok_is(&tok, "__inline__")) {
            i++;
            continue;
        }
        break;
    }
    i = skip_gcc_noise(t, i);
    Type *ret = parse_c_type(h, t, &i);
    if (!ret)
        return;
    tok = tok_at(t, i);
    if (tok.punct || tok.n == 0)
        return; /* abstract declarator: not a named prototype */
    char *name = tok_text(&tok);
    i++;
    TypeList params = {0};
    int variadic = 0;
    size_t j = i;
    if (!parse_params(h, t, &j, &params, &variadic))
        return;
    if (j > end)
        return; /* junk trailing tokens: be conservative */
    CMember m;
    memset(&m, 0, sizeof(m));
    m.name = str_intern_c(name);
    m.kind = CM_FN;
    /* Golden safety rule: returned C pointers are nullable in Zov */
    m.ret = (ret->kind == TY_PTR) ? type_ptr(ret->base, 1) : ret;
    m.params = params;
    m.variadic = variadic;
    header_add(h, m);
}

/* Register a typedef alias: scalars map to primitives, the rest stay
 * opaque with verbatim C spelling. */
static void parse_typedef(CHeader *h, CTokList * t, size_t i, size_t end)
{
    i++; /* 'typedef' */
    size_t j = i;
    Type *base = parse_c_type(h, t, &j);
    if (!base)
        return;
    for (;;) {
        j = skip_gcc_noise(t, j);
        CTok tok = tok_at(t, j);
        if (tok.punct || tok.n == 0)
            return;
        char *alias = tok_text(&tok);
        j++;
        /* skip array suffixes on the alias */
        for (;;) {
            CTok lb = tok_at(t, j);
            if (!tok_is_punct(&lb, '['))
                break;
            int depth = 0;
            do {
                CTok x = tok_at(t, j);
                if (x.n == 0)
                    return;
                if (x.punct && x.s[0] == '[')
                    depth++;
                else if (x.punct && x.s[0] == ']')
                    depth--;
                j++;
            } while (depth > 0);
        }
        CMember m;
        memset(&m, 0, sizeof(m));
        m.name = str_intern_c(alias);
        m.kind = CM_TYPE;
        if (base->kind == TY_STRUCT && base->c_name)
            m.ret = c_opaque(alias); /* emit the alias itself */
        else
            m.ret = base;
        header_add(h, m);
        CTok nx = tok_at(t, j);
        if (tok_is_punct(&nx, ',') && j < end) {
            j++;
            continue;
        }
        return;
    }
}

static void parse_define(CHeader *h, char *line)
{
    char *name = line + 8; /* skip "#define" */
    while (*name == ' ' || *name == '\t')
        name++;
    char *p = name;
    while (*p && *p != ' ' && *p != '\t' && *p != '(')
        p++;
    /* C99: a macro is function-like only when '(' abuts the name */
    if (*p == '(')
        return;
    if (p == name)
        return;
    char *body = p;
    (void)0;
    /* integer-only: strip parens/whitespace then strtoll */
    char cleaned[128];
    size_t n = 0;
    for (char *q = body; *q && n + 1 < sizeof(cleaned); q++) {
        if (*q == ' ' || *q == '\t' || *q == '(' || *q == ')')
            continue;
        cleaned[n++] = *q;
    }
    cleaned[n] = '\0';
    if (n == 0)
        return;
    char *endp = NULL;
    long long v = strtoll(cleaned, &endp, 0);
    if (!endp || *endp != '\0')
        return;
    (void)v;
    CMember m;
    memset(&m, 0, sizeof(m));
    m.name = str_intern_c(name);
    m.kind = CM_CONST;
    m.ret = type_prim(v < INT32_MIN || v > INT32_MAX ? TY_I64 : TY_I32);
    header_add(h, m);
}

/* ---- tokenization + top-level scan ---- */

static void tokenize(const char *text, CTokList * out)
{
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (is_punct_ch(*p)) {
            CTok tok = { p, 1, 1 };
            da_push(out, tok);
            p++;
            continue;
        }
        const char *start = p;
        while (*p && !is_punct_ch(*p) && *p != ' ' && *p != '\t'
               && *p != '\n' && *p != '\r')
            p++;
        CTok tok = { start, (size_t)(p - start), 0 };
        da_push(out, tok);
    }
}

static size_t skip_braces(CTokList * t, size_t i)
{
    int depth = 0;
    do {
        CTok x = tok_at(t, i);
        if (x.n == 0)
            return i;
        if (x.punct && x.s[0] == '{')
            depth++;
        else if (x.punct && x.s[0] == '}')
            depth--;
        i++;
    } while (depth > 0);
    return i;
}

CHeader *cimport_header(const char *path, int angle)
{
    char *text = preprocess(path, angle);
    if (!text)
        return NULL;
    /* -dD defines must be handled line-wise before tokenization */
    CTokList toks = {0};
    CHeader *h = zalloc(sizeof(CHeader));
    char *line = text;
    char *nl;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';
        if (strncmp(line, "#define", 7) == 0)
            parse_define(h, line);
        else if (*line != '#')
            tokenize(line, &toks);
        line = nl + 1;
    }
    if (*line && *line != '#')
        tokenize(line, &toks);

    size_t i = 0;
    while (i < toks.len) {
        CTok tok = tok_at(&toks, i);
        if (tok_is_punct(&tok, ';')) {
            i++;
            continue;
        }
        if (tok.n == 0)
            break;
        /* collect the head of a declaration up to ';' or '{' */
        size_t head_start = i;
        while (i < toks.len) {
            CTok x = tok_at(&toks, i);
            if (x.n == 0)
                break;
            if (x.punct && x.s[0] == '{')
                break;
            if (x.punct && x.s[0] == ';')
                break;
            i++;
        }
        CTok x = tok_at(&toks, i);
        if (x.n == 0)
            break;
        if (x.punct && x.s[0] == '{') {
            CTok kw2 = tok_at(&toks, i >= 2 ? i - 2 : 0);
            int is_tag = tok_is(&kw2, "struct") || tok_is(&kw2, "union")
                         || tok_is(&kw2, "enum");
            int is_fn = !is_tag;
            i = skip_braces(&toks, i);
            if (is_fn) {
                parse_prototype(h, &toks, head_start, i);
                /* skip a function body's braces already consumed; the decl
                 * continues only for struct/enum paths */
                continue;
            }
            /* struct/union/enum definition: skip declarators to ';' */
            while (i < toks.len) {
                CTok y = tok_at(&toks, i);
                if (y.n == 0 || tok_is_punct(&y, ';'))
                    break;
                if (y.punct && y.s[0] == '{')
                    i = skip_braces(&toks, i);
                else
                    i++;
            }
            continue;
        }
        /* ';' terminates: ordinary declaration */
        CTok head0 = tok_at(&toks, head_start);
        if (tok_is(&head0, "typedef"))
            parse_typedef(h, &toks, head_start, i);
        else
            parse_prototype(h, &toks, head_start, i);
        i++; /* consume ';' */
    }
    return h;
}

CMember *cimport_find(CHeader *h, const char *name)
{
    if (!h)
        return NULL;
    for (size_t k = 0; k < h->members.len; k++)
        if (zstr_eq(h->members.items[k].name, name))
            return &h->members.items[k];
    return NULL;
}
