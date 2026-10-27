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

#include "diag.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DIAG_MAX_LINE 256
#define DIAG_MAX_NAME 64
#define DIAG_SUGGEST_MAX_DIST 2

static int g_diag_color = -1;

static int diag_use_color(void)
{
    if (g_diag_color >= 0)
        return g_diag_color;
    g_diag_color = isatty(STDERR_FILENO) && !getenv("NO_COLOR");
    return g_diag_color;
}

void diag_set_color(int enabled)
{
    g_diag_color = enabled ? 1 : 0;
}

static const char *C_RESET(void)  { return diag_use_color() ? "\033[0m" : ""; }
static const char *C_GUTTER(void)  { return diag_use_color() ? "\033[1;34m" : ""; }
static const char *C_PRIMARY(void) { return diag_use_color() ? "\033[1;31m" : ""; }
static const char *C_SECOND(void)  { return diag_use_color() ? "\033[1;36m" : ""; }
static const char *C_HELP(void)    { return diag_use_color() ? "\033[36m" : ""; }

static int num_width(int n)
{
    int w = 1;
    while (n >= 10) {
        n /= 10;
        w++;
    }
    return w;
}

// extracts the full source line text from a span's pointers
static const char *span_line_text(Span *sp, size_t *out_len)
{
    const char *ls = sp->start - (sp->col - 1);
    const char *le = ls;
    while (*le && *le != '\n' && (size_t)(le - ls) < DIAG_MAX_LINE - 1)
        le++;
    if (out_len)
        *out_len = (size_t)(le - ls);
    return ls;
}

// renders one source line with its gutter, carets, and label
static void emit_snippet(FILE *out, DiagLoc *loc, int gutter_w,
                         const char *caret_char, const char *color)
{
    char line[DIAG_MAX_LINE];
    Span sp;
    sp.file = loc->file;
    sp.line = (size_t)loc->line;
    sp.col = (size_t)loc->col;
    sp.start = NULL;
    sp.end = NULL;

    // reconstruct a minimal span for text extraction
    // the caller must have populated the source pointer via diag_from_span
    if (!loc->src_ptr)
        return;

    sp.start = loc->src_ptr;
    size_t len = 0;
    const char *text = span_line_text(&sp, &len);
    size_t copy = len < DIAG_MAX_LINE - 1 ? len : DIAG_MAX_LINE - 1;
    memcpy(line, text, copy);
    line[copy] = '\0';

    // gutter line number
    fprintf(out, "%s%*d%s | ", C_GUTTER(), gutter_w, loc->line, C_RESET());
    fputs(line, out);
    fputc('\n', out);

    // caret line: (col-1) spaces, then len carets, then label
    fprintf(out, "%s%*s%s | ", C_GUTTER(), gutter_w, "", C_RESET());
    int i;
    for (i = 0; i < loc->col - 1 && i < (int)sizeof(line); i++)
        fputc(' ', out);
    fprintf(out, "%s", color);
    for (i = 0; i < loc->len; i++)
        fputc(caret_char[0], out);
    fprintf(out, "%s", C_RESET());
    if (loc->label && loc->label[0])
        fprintf(out, " %s", loc->label);
    fputc('\n', out);
}

static const char *sev_color(DiagSeverity s)
{
    if (!diag_use_color())
        return "";
    return s == DIAG_ERR ? "\033[1;31m"
         : s == DIAG_WARN ? "\033[1;33m" : "\033[1;34m";
}

void diag_emit(Diagnostic *d)
{
    FILE *out = stderr;
    const char *sev = d->severity == DIAG_ERR ? "error"
                    : d->severity == DIAG_WARN ? "warning" : "note";

    // machine-parseable first line: the LSP extracts file:line:col + message
    fprintf(out, "%s%s:%d:%d:%s %s: ", sev_color(d->severity),
            d->primary.file, d->primary.line,
            d->primary.col, C_RESET(), sev);
    if (d->code)
        fprintf(out, "[%s] ", d->code);
    fprintf(out, "%s\n", d->message);

    if (!d->primary.src_ptr)
        return;

    // gutter width covers the largest line number shown
    int gw = num_width(d->primary.line);
    if (d->secondary.file)
        gw = gw > num_width(d->secondary.line) ? gw
                                               : num_width(d->secondary.line);

    fprintf(out, "%s%*s |%s\n", C_GUTTER(), gw, "", C_RESET());

    // secondary span first when it precedes the primary line
    if (d->secondary.file && d->secondary.src_ptr
        && d->secondary.line < d->primary.line) {
        emit_snippet(out, &d->secondary, gw, "-", C_SECOND());
    }

    emit_snippet(out, &d->primary, gw, "^", C_PRIMARY());

    // secondary span after when it follows the primary line
    if (d->secondary.file && d->secondary.src_ptr
        && d->secondary.line > d->primary.line) {
        fprintf(out, "%s...\n%s", C_GUTTER(), C_RESET());
        emit_snippet(out, &d->secondary, gw, "-", C_SECOND());
    }

    fprintf(out, "%s%*s |%s\n", C_GUTTER(), gw, "", C_RESET());

    if (d->help && d->help[0])
        fprintf(out, "%s%*s = help:%s %s\n", C_HELP(), gw, "", C_RESET(),
                d->help);
}

/* ---- Levenshtein ---- */

static int levenshtein(const char *a, const char *b, int max)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la >= DIAG_MAX_NAME || lb >= DIAG_MAX_NAME)
        return max + 1;
    if (la == 0)
        return lb;
    if (lb == 0)
        return la;
    int diff = la - lb;
    if (diff > max || -diff > max)
        return max + 1;

    int prev[DIAG_MAX_NAME], cur[DIAG_MAX_NAME];
    for (int j = 0; j <= lb; j++)
        prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j - 1] + 1 < m)
                m = cur[j - 1] + 1;
            if (prev[j - 1] + cost < m)
                m = prev[j - 1] + cost;
            cur[j] = m;
        }
        for (int j = 0; j <= lb; j++)
            prev[j] = cur[j];
    }
    return prev[lb];
}

const char *diag_suggest_symbol(const char *input, const char **candidates,
                                size_t n)
{
    const char *best = NULL;
    int best_dist = DIAG_SUGGEST_MAX_DIST + 1;
    for (size_t i = 0; i < n; i++) {
        int d = levenshtein(input, candidates[i], DIAG_SUGGEST_MAX_DIST);
        if (d <= DIAG_SUGGEST_MAX_DIST && d < best_dist) {
            best_dist = d;
            best = candidates[i];
        }
    }
    return best;
}

/* ---- fatal wrapper ---- */

void ZOV_NORETURN diag_fatal_at(const char *code, Span span,
                                const char *label, Span *sec,
                                const char *seclabel, const char *help,
                                const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    Diagnostic d;
    memset(&d, 0, sizeof(d));
    d.severity = DIAG_ERR;
    d.code = code;
    d.message = msg;
    d.primary.file = span.file;
    d.primary.line = (int)span.line;
    d.primary.col = (int)span.col;
    d.primary.len = (int)(span.end > span.start
                              ? span.end - span.start : 1);
    d.primary.label = label;
    d.primary.src_ptr = span.start;
    if (sec) {
        d.secondary.file = sec->file;
        d.secondary.line = (int)sec->line;
        d.secondary.col = (int)sec->col;
        d.secondary.len = (int)(sec->end - sec->start);
        d.secondary.label = seclabel;
        d.secondary.src_ptr = sec->start;
    }
    d.help = help;
    diag_emit(&d);
    exit(1);
}
