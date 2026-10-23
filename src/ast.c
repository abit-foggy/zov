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
