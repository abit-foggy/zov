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

#include "ast.h"

static Arena g_arena;
static Arena g_intern_arena;

void arena_init(Arena *a, size_t cap)
{
    a->base = malloc(cap);
    if (!a->base)
        fatal("out of memory");
    a->cap = cap;
    a->off = 0;
}

void arena_destroy(Arena *a)
{
    free(a->base);
    a->base = NULL;
    a->cap = 0;
    a->off = 0;
}

/* Align `off` up to `align` (power of two); grow geometrically on overflow. */
void *arena_alloc(Arena *a, size_t size, size_t align)
{
    size_t off = (a->off + align - 1) & ~(align - 1);
    if (off + size > a->cap) {
        while (off + size > a->cap)
            a->cap = a->cap ? a->cap * 2 : (1 << 16);
        a->base = realloc(a->base, a->cap);
        if (!a->base)
            fatal("out of memory");
    }
    a->off = off + size;
    return a->base + off;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *dst = arena_alloc(a, n + 1, 1);
    memcpy(dst, s, n);
    dst[n] = '\0';
    return dst;
}

void *zalloc(size_t size)
{
    return arena_alloc(&g_arena, size, 8);
}

/* Intern table: open addressing, FNV-1a, power-of-two capacity. */
#define INTERN_INIT_CAP 1024

typedef struct InternTab {
    const char **slots;
    size_t cap;
    size_t count;
} InternTab;

static InternTab g_interns;

static size_t fnv1a(const char *s, size_t n)
{
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void intern_grow(InternTab *t)
{
    size_t ncap = t->cap * 2;
    const char **ns = calloc(ncap, sizeof(*ns));
    if (!ns)
        fatal("out of memory");
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slots[i])
            continue;
        size_t h = fnv1a(t->slots[i], strlen(t->slots[i])) & (ncap - 1);
        while (ns[h])
            h = (h + 1) & (ncap - 1);
        ns[h] = t->slots[i];
    }
    free(t->slots);
    t->slots = ns;
    t->cap = ncap;
}

const char *str_intern(const char *start, const char *end)
{
    size_t n = (size_t)(end - start);
    if (g_interns.cap == 0) {
        g_interns.cap = INTERN_INIT_CAP;
        g_interns.slots = calloc(g_interns.cap, sizeof(*g_interns.slots));
        if (!g_interns.slots)
            fatal("out of memory");
    }
    size_t h = fnv1a(start, n) & (g_interns.cap - 1);
    while (g_interns.slots[h]) {
        const char *s = g_interns.slots[h];
        if (strlen(s) == n && memcmp(s, start, n) == 0)
            return s;
        h = (h + 1) & (g_interns.cap - 1);
    }
    char *dup = arena_strndup(&g_intern_arena, start, n);
    g_interns.slots[h] = dup;
    g_interns.count++;
    if (g_interns.count * 4 >= g_interns.cap * 3)
        intern_grow(&g_interns);
    return dup;
}

const char *str_intern_c(const char *s)
{
    return str_intern(s, s + strlen(s));
}

int zstr_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "zovc: error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void fatal_at(Span span, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%lu:%lu: error: ", span.file,
            (unsigned long)span.line, (unsigned long)span.col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---- types ---- */

/* Designated init keeps this extensible under -Wmissing-field-initializers. */
static Type g_prims[] = {
    [TY_NONE] = { .kind = TY_NONE }, [TY_UNKNOWN] = { .kind = TY_UNKNOWN },
    [TY_VOID] = { .kind = TY_VOID }, [TY_I8] = { .kind = TY_I8 },
    [TY_I16] = { .kind = TY_I16 }, [TY_I32] = { .kind = TY_I32 },
    [TY_I64] = { .kind = TY_I64 }, [TY_ISIZE] = { .kind = TY_ISIZE },
    [TY_U8] = { .kind = TY_U8 }, [TY_U16] = { .kind = TY_U16 },
    [TY_U32] = { .kind = TY_U32 }, [TY_U64] = { .kind = TY_U64 },
    [TY_USIZE] = { .kind = TY_USIZE }, [TY_F32] = { .kind = TY_F32 },
    [TY_F64] = { .kind = TY_F64 }, [TY_BOOL] = { .kind = TY_BOOL },
    [TY_CHAR] = { .kind = TY_CHAR }, [TY_STR] = { .kind = TY_STR },
};

Type *type_prim(TypeKind k)
{
    return &g_prims[k];
}

Type *type_default_int(void) { return type_prim(TY_I32); }
Type *type_default_float(void) { return type_prim(TY_F64); }

Type *type_null(void)
{
    return type_ptr(type_prim(TY_VOID), 1);
}

/* Composite cache keyed structurally; Type* stays comparable by identity
 * for the common pointer/ref/slice shapes. */
static DA(Type *) g_comp_types;

static Type *type_comp(TypeKind k, Type *base, size_t count, int nullable,
                        const char *name)
{
    for (size_t i = 0; i < g_comp_types.len; i++) {
        Type *t = g_comp_types.items[i];
        if (t->kind == k && t->base == base && t->count == count
            && t->nullable == nullable
            && ((t->name == NULL && name == NULL)
                || (t->name && name && t->name == name)))
            return t;
    }
    Type *t = zalloc(sizeof(Type));
    t->kind = k;
    t->base = base;
    t->count = count;
    t->nullable = nullable;
    t->name = name;
    da_push(&g_comp_types, t);
    return t;
}

Type *type_ptr(Type *base, int nullable)
{
    return type_comp(TY_PTR, base, 0, nullable, NULL);
}

Type *type_ref(Type *base, int is_mut)
{
    return type_comp(is_mut ? TY_MUTREF : TY_REF, base, 0, 0, NULL);
}

Type *type_slice(Type *base)
{
    return type_comp(TY_SLICE, base, 0, 0, NULL);
}

Type *type_array(Type *base, size_t n)
{
    return type_comp(TY_ARRAY, base, n, 0, NULL);
}

Type *type_struct(const char *name)
{
    return type_comp(TY_STRUCT, NULL, 0, 0, name);
}

/* Union member identity: error variants key by name, payload types by
 * structure; identical members are dropped on flatten. */
static int member_eq(Type *a, Type *b)
{
    if (a->kind == TY_ERR && b->kind == TY_ERR)
        return a->name == b->name;
    return type_eq(a, b);
}

static void union_flatten(Type *m, TypeList *out)
{
    if (m->kind == TY_UNION) {
        for (size_t i = 0; i < m->members.len; i++)
            union_flatten(m->members.items[i], out);
        return;
    }
    for (size_t i = 0; i < out->len; i++)
        if (member_eq(out->items[i], m))
            return;
    da_push(out, m);
}

Type *type_union(TypeList *members)
{
    Type *t = zalloc(sizeof(Type));
    t->kind = TY_UNION;
    for (size_t i = 0; i < members->len; i++)
        union_flatten(members->items[i], &t->members);
    return t;
}

/* Named error types are never cached: distinct names must stay distinct. */
Type *type_err(const char *name, Type *payload)
{
    Type *t = zalloc(sizeof(Type));
    t->kind = TY_ERR;
    t->name = name;
    t->base = payload;
    t->is_error = 1;
    return t;
}

int type_eq(const Type *a, const Type *b)
{
    if (a == b)
        return 1;
    if (!a || !b || a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case TY_PTR:
        return a->nullable == b->nullable && type_eq(a->base, b->base);
    case TY_REF:
    case TY_MUTREF:
    case TY_SLICE:
        return type_eq(a->base, b->base);
    case TY_ARRAY:
        return a->count == b->count && type_eq(a->base, b->base);
    case TY_STRUCT:
        return a->name == b->name;
    case TY_INST:
        if (a->name != b->name || a->members.len != b->members.len)
            return 0;
        for (size_t i = 0; i < a->members.len; i++)
            if (!type_eq(a->members.items[i], b->members.items[i]))
                return 0;
        return 1;
    case TY_ERR:
        return a->name == b->name && type_eq(a->base, b->base);
    case TY_UNION: {
        if (a->members.len != b->members.len)
            return 0;
        for (size_t i = 0; i < a->members.len; i++)
            if (!type_eq(a->members.items[i], b->members.items[i]))
                return 0;
        return 1;
    }
    default:
        return 1;
    }
}

const char *type_spelling(Type *t)
{
    if (!t)
        return "<nil>";
    switch (t->kind) {
    case TY_NONE: return "<none>";
    case TY_UNKNOWN: return "<extern>";
    case TY_VOID: return "void";
    case TY_I8: return "i8";
    case TY_I16: return "i16";
    case TY_I32: return "i32";
    case TY_I64: return "i64";
    case TY_ISIZE: return "isize";
    case TY_U8: return "u8";
    case TY_U16: return "u16";
    case TY_U32: return "u32";
    case TY_U64: return "u64";
    case TY_USIZE: return "usize";
    case TY_F32: return "f32";
    case TY_F64: return "f64";
    case TY_BOOL: return "bool";
    case TY_CHAR: return "char";
    case TY_STR: return "str";
    case TY_PTR: {
        char *s = zalloc(64);
        snprintf(s, 64, "%s*%s", t->nullable ? "nullable " : "",
                 type_spelling(t->base));
        return s;
    }
    case TY_REF:
    case TY_MUTREF: {
        char *s = zalloc(64);
        snprintf(s, 64, "&%s%s", t->kind == TY_MUTREF ? "mut " : "",
                 type_spelling(t->base));
        return s;
    }
    case TY_SLICE: {
        char *s = zalloc(64);
        snprintf(s, 64, "[]%s", type_spelling(t->base));
        return s;
    }
    case TY_ARRAY: {
        char *s = zalloc(64);
        snprintf(s, 64, "[%lu]%s", (unsigned long)t->count,
                 type_spelling(t->base));
        return s;
    }
    case TY_STRUCT:
        return t->name;
    case TY_ERR:
        return t->name ? t->name : type_spelling(t->base);
    case TY_UNION: {
        char *s = zalloc(256);
        size_t off = 0;
        off += (size_t)snprintf(s + off, 256 - off, "(");
        for (size_t i = 0; i < t->members.len; i++)
            off += (size_t)snprintf(s + off, 256 - off, "%s%s",
                                    i ? " | " : "",
                                    type_spelling(t->members.items[i]));
        snprintf(s + off, 256 - off, ")");
        return s;
    }
    case TY_INST: {
        char *s = zalloc(256);
        size_t off = 0;
        off += (size_t)snprintf(s + off, 256 - off, "%s[", t->name);
        for (size_t i = 0; i < t->members.len; i++)
            off += (size_t)snprintf(s + off, 256 - off, "%s%s",
                                    i ? ", " : "",
                                    type_spelling(t->members.items[i]));
        snprintf(s + off, 256 - off, "]");
        return s;
    }
    }
    return "<invalid>";
}

Type *type_from_suffix(const char *suffix)
{
    static const struct { const char *s; TypeKind k; } tab[] = {
        {"i8", TY_I8}, {"i16", TY_I16}, {"i32", TY_I32}, {"i64", TY_I64},
        {"isize", TY_ISIZE}, {"u8", TY_U8}, {"u16", TY_U16},
        {"u32", TY_U32}, {"u64", TY_U64}, {"usize", TY_USIZE},
        {"f32", TY_F32}, {"f64", TY_F64},
    };
    if (!*suffix)
        return NULL;
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (zstr_eq(tab[i].s, suffix))
            return type_prim(tab[i].k);
    return NULL;
}

int ty_is_int(TypeKind k)
{
    return k >= TY_I8 && k <= TY_USIZE;
}

int ty_is_float(TypeKind k)
{
    return k == TY_F32 || k == TY_F64;
}

int ty_is_num(TypeKind k)
{
    return ty_is_int(k) || ty_is_float(k);
}

int ty_is_bool_like(TypeKind k)
{
    return k == TY_BOOL;
}

/* ---- nodes ---- */

Expr *expr_new(ExprKind kind, Span span)
{
    Expr *e = zalloc(sizeof(Expr));
    e->kind = kind;
    e->span = span;
    return e;
}

Stmt *stmt_new(StmtKind kind, Span span)
{
    Stmt *s = zalloc(sizeof(Stmt));
    s->kind = kind;
    s->span = span;
    return s;
}

Block *block_new(void)
{
    return zalloc(sizeof(Block));
}

Decl *decl_new(DeclKind kind, Span span)
{
    Decl *d = zalloc(sizeof(Decl));
    d->kind = kind;
    d->span = span;
    return d;
}
