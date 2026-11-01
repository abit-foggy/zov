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

/* Central registry entry: every code emitted by the compiler is listed in
 * src/diag.c.  Codes are stable once released: never reuse or repurpose
 * one, add a new code instead. */
typedef struct {
    const char *code;  /* "E0402" */
    const char *title; /* short human title, e.g. "undefined symbol" */
    const char *help;  /* one-line actionable guidance */
} DiagCodeInfo;

/* Diagnostic code identifiers.  Using the macro at a call site makes a
 * misspelled code a compile error; diag.c checks it against the registry.
 * Codes are stable: add new ones, never repurpose existing ones. */
#define E0101 "E0101"
#define E0102 "E0102"
#define E0103 "E0103"
#define E0104 "E0104"
#define E0105 "E0105"
#define E0106 "E0106"
#define E0107 "E0107"
#define E0108 "E0108"
#define E0201 "E0201"
#define E0202 "E0202"
#define E0203 "E0203"
#define E0204 "E0204"
#define E0205 "E0205"
#define E0206 "E0206"
#define E0207 "E0207"
#define E0208 "E0208"
#define E0209 "E0209"
#define E0210 "E0210"
#define E0211 "E0211"
#define E0212 "E0212"
#define E0213 "E0213"
#define E0214 "E0214"
#define E0215 "E0215"
#define E0301 "E0301"
#define E0302 "E0302"
#define E0303 "E0303"
#define E0304 "E0304"
#define E0305 "E0305"
#define E0306 "E0306"
#define E0401 "E0401"
#define E0402 "E0402"
#define E0403 "E0403"
#define E0404 "E0404"
#define E0405 "E0405"
#define E0406 "E0406"
#define E0407 "E0407"
#define E0408 "E0408"
#define E0409 "E0409"
#define E0410 "E0410"
#define E0411 "E0411"
#define E0412 "E0412"
#define E0413 "E0413"
#define E0414 "E0414"
#define E0415 "E0415"
#define E0416 "E0416"
#define E0502 "E0502"
#define E0503 "E0503"
#define E0504 "E0504"
#define E0505 "E0505"
#define E0506 "E0506"
#define E0507 "E0507"
#define E0508 "E0508"
#define E0509 "E0509"
#define E0510 "E0510"
#define E0511 "E0511"
#define E0512 "E0512"
#define E0513 "E0513"
#define E0514 "E0514"
#define E0515 "E0515"
#define E0516 "E0516"
#define E0517 "E0517"
#define E0611 "E0611"
#define E0612 "E0612"
#define E0601 "E0601"
#define E0602 "E0602"
#define E0603 "E0603"
#define E0604 "E0604"
#define E0605 "E0605"
#define E0606 "E0606"
#define E0607 "E0607"
#define E0608 "E0608"
#define E0609 "E0609"
#define E0610 "E0610"
#define E0701 "E0701"
#define E0702 "E0702"
#define E0703 "E0703"
#define E0704 "E0704"
#define E0705 "E0705"
#define E0706 "E0706"
#define E0707 "E0707"
#define E0708 "E0708"
#define E0709 "E0709"
#define E0801 "E0801"
#define E0802 "E0802"
#define E0803 "E0803"
#define E0804 "E0804"
#define E0805 "E0805"
#define E0806 "E0806"
#define E0901 "E0901"
#define E0902 "E0902"
#define E1001 "E1001"
#define E1002 "E1002"
#define E1003 "E1003"
#define E1004 "E1004"
#define E1005 "E1005"
#define E1006 "E1006"
#define E1007 "E1007"
#define E1008 "E1008"
#define W0001 "W0001"

const DiagCodeInfo *diag_code_lookup(const char *code);
const char *diag_code_title(const char *code);
void diag_list_codes(FILE *out);

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

// coded fatal without a source span (whole-program or tool-level errors)
void ZOV_NORETURN diag_fatal_global(const char *code, const char *fmt, ...);

#endif /* ZOV_DIAG_H */
