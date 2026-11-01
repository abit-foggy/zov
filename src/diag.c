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

/* ---- code registry ---- */

/* Codes are grouped by compiler stage:
 *   E01xx lexer            E05xx borrow / ownership
 *   E02xx parser           E06xx types
 *   E03xx modules          E07xx literals, statements, control flow
 *   E04xx name resolution  E08xx generics
 *   E09xx code generation / internal
 * Codes listed here are stable: extend the table, never repurpose an
 * existing code. */
static const DiagCodeInfo g_codes[] = {
    {"E0101", "unterminated block comment",
     "Close the comment with '*/'."},
    {"E0102", "invalid \\x escape",
     "\\x must be followed by exactly two hexadecimal digits."},
    {"E0103", "unknown escape sequence",
     "Use a supported escape: \\n \\t \\r \\\\ \\0 \\' \\\" \\xNN."},
    {"E0104", "unterminated string literal",
     "Close the string before the end of the line."},
    {"E0105", "unterminated character literal",
     "Close the character literal with a single quote."},
    {"E0106", "invalid numeric suffix",
     "Use a width suffix such as i8, u32, usize, f64."},
    {"E0107", "float literal with integer suffix",
     "Float literals take f32 or f64, or no suffix."},
    {"E0108", "unexpected character",
     "Remove the character or write it inside a string literal."},

    {"E0201", "unexpected token",
     "Remove the token or complete the construct it belongs to."},
    {"E0202", "expected expression",
     "Provide a value here."},
    {"E0203", "expected type",
     "Write a type name such as i32, str, or Vec[T]."},
    {"E0204", "malformed string interpolation",
     "Close the '{' interpolation with '}'."},
    {"E0205", "malformed match pattern",
     "Write a variant name, 'case let name: Type', or '_'."},
    {"E0206", "malformed struct literal",
     "Write Type{ field = value, ... }."},
    {"E0207", "expected identifier",
     "Provide the name this declaration requires."},
    {"E0208", "malformed path",
     "Complete the '::' or '.' path with an identifier."},
    {"E0209", "invalid defer target",
     "defer may wrap a call or a block, not a declaration."},
    {"E0210", "malformed declaration",
     "Check the declaration's parameter and field syntax."},
    {"E0211", "unsupported syntax",
     "See the language reference for supported constructs."},
    {"E0212", "malformed use declaration",
     "Write 'use std::module;' or 'use <header.h> as alias;'."},
    {"E0213", "malformed type declaration",
     "Check the type's variants and fields."},
    {"E0214", "malformed function declaration",
     "Check the parameter list and return type."},
    {"E0215", "expected top-level item",
     "Only 'use', 'type', 'fn', and 'test' may appear at top level."},

    {"E0301", "module not found",
     "Check the module path and the standard-library search path."},
    {"E0302", "import cycle",
     "Break the cycle by moving shared declarations to a third module."},
    {"E0303", "C header not available",
     "Check the header name and the include path."},
    {"E0304", "malformed import",
     "Write 'use <header.h> as alias;' for C headers."},
    {"E0305", "duplicate import",
     "Remove the repeated use declaration."},
    {"E0306", "library unavailable in this build",
     "This standard-library facility needs a hosted runtime."},

    {"E0401", "undefined function",
     "Check the spelling or import the defining module."},
    {"E0402", "undefined symbol",
     "Declare the name before use, or check the spelling."},
    {"E0403", "module member not found",
     "Check the module source for the exported spelling."},
    {"E0404", "module not imported",
     "Add a use declaration for the module."},
    {"E0405", "unknown namespace",
     "Check the namespace name or import it first."},
    {"E0406", "name is not a value",
     "This name is a type, module, or function; use it in that position."},
    {"E0407", "no such member",
     "Check the field, method, or variant name."},
    {"E0408", "unknown type",
     "Check the type name or declare it in this module."},
    {"E0409", "duplicate definition",
     "Rename or remove one of the definitions."},
    {"E0410", "recursive type has infinite size",
     "Break the cycle with a pointer, slice, or Vec."},
    {"E0411", "invalid entry point",
     "A Zov entry point takes no parameters and returns i32 or void."},
    {"E0412", "unresolved module symbol",
     "Qualify the symbol with a module that exports it."},
    {"E0413", "value is not callable",
     "Call a function or method instead of a plain value."},
    {"E0414", "invalid method receiver",
     "A method receiver must be a struct type, written as (self: &T)."},
    {"E0415", "field is private",
     "Use the type's accessor methods instead of its raw fields."},
    {"E0416", "type alias cycle",
     "An alias must resolve to a concrete type; remove the cycle."},

    {"E0502", "move while borrowed",
     "Consume the borrow before moving or freeing the value."},
    {"E0503", "use of moved value",
     "Resource values are linear; each value has exactly one owner."},
    {"E0504", "use of freed value",
     "Do not read a value after free(); reorder the cleanup."},
    {"E0505", "write through shared reference",
     "Use '&mut T' to mutate through a reference."},
    {"E0506", "conflicting borrow",
     "End the first borrow before starting a second overlapping one."},
    {"E0507", "read while mutably borrowed",
     "Stop using the mutable borrow before reading the value."},
    {"E0508", "cannot assign through borrow",
     "The destination is borrowed; end that borrow first."},
    {"E0509", "value already freed",
     "Remove the second free(); each resource is freed once."},
    {"E0510", "cannot move value",
     "The value is borrowed, deferred, or partially moved."},
    {"E0511", "cannot move field out of container",
     "Move the whole value, or make the container a resource struct."},
    {"E0512", "returning reference to local",
     "Return an owned value, or a view of a parameter instead."},
    {"E0513", "value moved in loop",
     "Reassign the value at the end of each iteration."},
    {"E0514", "conflicting borrows in call arguments",
     "Split the call so only one argument borrows mutably."},
    {"E0515", "deferred cleanup conflict",
     "A deferred block still uses this value; move the defer earlier."},
    {"E0516", "mutation of immutable binding",
     "Declare the binding with 'let mut'."},

    {"E0601", "mismatched types",
     "Use an explicit 'as' cast when a conversion is intended."},
    {"E0602", "wrong number of arguments",
     "Match the function's declared parameters."},
    {"E0603", "invalid cast",
     "Only numeric, pointer, and error conversions may be cast."},
    {"E0604", "invalid operand type",
     "Check the operand types accepted by this operator."},
    {"E0605", "type cannot be used here",
     "This position requires a different kind of value."},
    {"E0606", "missing field in literal",
     "Resource literals must initialize every field."},
    {"E0607", "nullable value used directly",
     "Narrow with match, '?', or '!' before use."},
    {"E0608", "non-exhaustive match",
     "Cover every variant, or add a '_' arm."},
    {"E0609", "duplicate match arm",
     "Remove the repeated arm."},
    {"E0610", "invalid pattern",
     "Check the pattern against the matched type."},

    {"E0701", "integer literal out of range",
     "Use a wider type or a smaller literal."},
    {"E0702", "missing return value",
     "Return a value of the declared type."},
    {"E0703", "function may fall through",
     "Return a value on every path, or return void."},
    {"E0704", "unhandled error value",
     "Handle the error with '?', match, or 'let _ = ...'."},
    {"E0705", "break outside loop",
     "break and continue require an enclosing loop."},
    {"E0706", "void function cannot return a value",
     "Remove the returned expression."},
    {"E0707", "invalid assignment target",
     "Assign to a variable, field, index, or dereference."},
    {"E0708", "invalid initializer",
     "Provide an initializer or an explicit complete type."},
    {"E0709", "compound assignment on non-integer",
     "Compound operators require an integer or numeric lvalue."},

    {"E0801", "not a generic function",
     "Remove the type arguments, or use a generic function."},
    {"E0802", "wrong number of type arguments",
     "Match the declared type parameters."},
    {"E0803", "explicit type arguments required",
     "Write f[T](...) to instantiate a generic function."},
    {"E0804", "invalid generic instantiation",
     "Instantiate through a call or struct literal."},
    {"E0805", "type nesting too deep",
     "Simplify deeply nested generic or pointer types."},
    {"E0806", "unsupported generic form",
     "Check the builtin's accepted generic signature."},

    {"E0901", "unsupported construct in code generation",
     "This is a compiler limitation; please report it."},

    /* E1xxx: runtime failures reported by the generated panic path. */
    {"E1001", "index out of bounds",
     "Check the index against the collection's length."},
    {"E1002", "slice out of bounds",
     "Slice ranges must satisfy start <= end <= len."},
    {"E1003", "integer division by zero",
     "Guard the divisor before dividing or taking a remainder."},
    {"E1004", "unwrap of null pointer",
     "Check for null with match before forcing the pointer."},
    {"E1005", "unwrap of error value",
     "Handle the error variant before forcing a success value."},
    {"E1006", "assertion failed",
     "The asserted condition evaluated to false."},
    {"E1007", "allocation failure",
     "The allocator returned no memory; reduce or bound the allocation."},
    {"E1008", "invalid owned String construction",
     "String::from_owned requires len <= cap and a non-null buffer."},
    {"E0902", "internal compiler error",
     "This is a compiler bug; please report it."},

    {"W0001", "unused variable",
     "Prefix the name with '_' if the binding is intentional."},
};

const DiagCodeInfo *diag_code_lookup(const char *code)
{
    if (!code)
        return NULL;
    for (size_t i = 0; i < sizeof(g_codes) / sizeof(g_codes[0]); i++)
        if (strcmp(g_codes[i].code, code) == 0)
            return &g_codes[i];
    return NULL;
}

const char *diag_code_title(const char *code)
{
    const DiagCodeInfo *info = diag_code_lookup(code);
    return info ? info->title : NULL;
}

void diag_list_codes(FILE *out)
{
    for (size_t i = 0; i < sizeof(g_codes) / sizeof(g_codes[0]); i++)
        fprintf(out, "%s  %s\n", g_codes[i].code, g_codes[i].title);
}

/* ---- explain ---- */

struct diag_explain {
    const char *code;
    const char *title;
    const char *body;
    const char *good;
    const char *bad;
};

static const struct diag_explain g_explains[] = {
    {"E0416", "type alias cycle",
     "Two or more type aliases refer to each other, so none of them "
     "names a concrete type. Point at least one alias at a real "
     "struct, union, or primitive instead of another alias.",
     "type Handle = *Conn;",
     "type A = B; type B = A;"},
    {"E0401", "undefined function",
     "The function name was not found in any visible scope or imported "
     "module. Check the spelling, or add the corresponding `use` "
     "declaration at the top of the file.",
     "use std::io; io::println(\"hi\");",
     "io::println(\"hi\");"},
    {"E0402", "undefined symbol",
     "The variable was not found in any visible scope. Declare it "
     "with `let` before its first use, or check the spelling; names "
     "are case-sensitive.",
     "let count = 5; return count;",
     "let conut = 5; return count;"},
    {"E0403", "module member not found",
     "The imported module does not export a function with this name. "
     "Check the module's source for the exact exported spelling.",
     "vec::push(v, 1);",
     "vec::pushs(v, 1);"},
    {"E0404", "module not imported",
     "The module namespace is not in scope. Add a `use` declaration "
     "at the top of the file to bring it into scope.",
     "use std::io; io::println(\"hi\");",
     "io::println(\"hi\");"},
    {"E0502", "move while borrowed",
     "The value cannot be moved or freed while a borrow (a reference "
     "or slice derived from it) is still live. Drop or consume the "
     "borrow first, or move the cleanup after the borrow's last use.",
     "let s = v[0..2]; let x = s[0]; v.free();",
     "let s = v[0..2]; v.free(); let x = s[0];"},
    {"E0503", "use after move",
     "The value was moved (freed, passed by value, or returned) and "
     "cannot be used again. Resource types are linear: each value "
     "has exactly one owner at a time.",
     "let b = a; use(b);",
     "let b = a; use(a); use(b);"},
    {"E0601", "type mismatch",
     "The two sides of the expression have different types. Zov does "
     "not implicitly coerce between integer widths or signedness; "
     "use an explicit `as` cast.",
     "let x: i64 = 5i32 as i64;",
     "let x: i64 = 5i32;"},
    {"E0602", "argument count mismatch",
     "The function was called with a different number of arguments "
     "than it declares. Check the function signature.",
     "println(\"one\");",
     "println();"},
    {"W0001", "unused variable",
     "The variable was declared but never read. Prefix it with an "
     "underscore to silence this warning if the binding is "
     "intentional.",
     "let _unused = 5;",
     "let unused = 5;"},
};

int diag_print_explanation(const char *code)
{
    const DiagCodeInfo *info = diag_code_lookup(code);
    for (size_t i = 0; i < sizeof(g_explains) / sizeof(g_explains[0]); i++) {
        if (strcmp(g_explains[i].code, code) != 0)
            continue;
        printf("%s[%s]: %s\n\n%s\n", code[0] == 'W' ? "warning" : "error",
               code, g_explains[i].title, g_explains[i].body);
        if (g_explains[i].good)
            printf("\n  good: %s\n", g_explains[i].good);
        if (g_explains[i].bad)
            printf("  bad:  %s\n", g_explains[i].bad);
        return 0;
    }
    if (info) {
        printf("%s[%s]: %s\n\n%s\n",
               code[0] == 'W' ? "warning" : "error", code, info->title,
               info->help);
        return 0;
    }
    fprintf(stderr, "zovc: no explanation for code '%s'\n", code);
    return 1;
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

void ZOV_NORETURN diag_fatal_global(const char *code, const char *fmt, ...)
{
    char msg[1024];
    const DiagCodeInfo *info = diag_code_lookup(code);
    if (!info)
        fatal("unknown diagnostic code '%s' emitted for: %s",
              code ? code : "(null)", fmt);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "error[%s]: %s\n", code, msg);
    if (info->help && info->help[0])
        fprintf(stderr, " = help: %s\n", info->help);
    exit(1);
}
