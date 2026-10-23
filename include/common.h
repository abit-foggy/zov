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

#ifndef ZOV_COMMON_H
#define ZOV_COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Span {
    const char *file;
    size_t line;
    size_t col;
} Span;

typedef struct Arena {
    char *base;
    size_t cap;
    size_t off;
} Arena;

void arena_init(Arena *a, size_t cap);
void arena_destroy(Arena *a);
void *arena_alloc(Arena *a, size_t size, size_t align);
char *arena_strndup(Arena *a, const char *s, size_t n);

const char *str_intern(const char *start, const char *end);
const char *str_intern_c(const char *s);
int zstr_eq(const char *a, const char *b);

void fatal(const char *fmt, ...);
void fatal_at(Span span, const char *fmt, ...);

/* Typed dynamic array; POD storage, geometric growth. */
#define DA(T) struct { T *items; size_t len; size_t cap; }

#define da_push(da, item)                                                       \
    do {                                                                        \
        if ((da)->len >= (da)->cap) {                                           \
            size_t da_cap_ = (da)->cap ? (da)->cap * 2 : 8;                     \
            void *da_p_ = realloc((da)->items,                                  \
                                   da_cap_ * sizeof(*(da)->items));             \
            if (!da_p_)                                                         \
                fatal("out of memory");                                         \
            (da)->items = da_p_;                                                \
            (da)->cap = da_cap_;                                                \
        }                                                                       \
        (da)->items[(da)->len++] = (item);                                      \
    } while (0)

#endif /* ZOV_COMMON_H */
