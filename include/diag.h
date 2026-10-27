// Copyright (c) 2026 foggy (abit-foggy)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef ZOV_DIAG_H
#define ZOV_DIAG_H

#include "common.h"

typedef enum {
    DIAG_ERR,
    DIAG_WARN,
    DIAG_NOTE,
} DiagSeverity;

typedef struct {
    const char *file;
    int line;          /* 1-based */
    int col;           /* 1-based */
    int len;           /* underline span in source characters */
    const char *label; /* inline label rendered after the carets */
    const char *src_ptr; /* pointer into the in-memory source line */
} DiagLoc;

typedef struct {
    DiagSeverity severity;
    const char *code;    /* "E0104" etc, or NULL for legacy errors */
    const char *message;
    DiagLoc primary;
    DiagLoc secondary;   /* secondary.file != NULL means present */
    const char *help;
} Diagnostic;

void diag_emit(Diagnostic *d);
void diag_set_color(int enabled);
int diag_print_explanation(const char *code);

// closest candidate within Levenshtein distance 2, or NULL
const char *diag_suggest_symbol(const char *input, const char **candidates,
                                size_t n);

// rich fatal: renders the snippet and exits; code/label/help/sec are optional
void ZOV_NORETURN diag_fatal_at(const char *code, Span span,
                                const char *label, Span *sec,
                                const char *seclabel, const char *help,
                                const char *fmt, ...);

#endif /* ZOV_DIAG_H */
