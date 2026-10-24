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

#include "codegen.h"

typedef struct Buf {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_puts(Buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap)
            b->cap = b->cap ? b->cap * 2 : 4096;
        b->data = realloc(b->data, b->cap);
        if (!b->data)
            fatal("out of memory");
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    char *out = tmp;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)need >= sizeof(tmp)) {
        out = malloc((size_t)need + 1);
        if (!out)
            fatal("out of memory");
        vsnprintf(out, (size_t)need + 1, fmt, ap2);
    }
    va_end(ap2);
    buf_puts(b, out);
    if (out != tmp)
        free(out);
}

/* Single defer stack slot: the statement plus nothing else, because the
 * owning block is implied by the stack-base recorded at block entry. */
typedef struct DeferEntry {
    Stmt *stmt;
} DeferEntry;

typedef struct CG {
    Buf head; /* includes, typedefs, prototypes */
    Buf body; /* function definitions */
    Buf *cur; /* active emission target */
    DA(DeferEntry) defers;
    DA(size_t) loop_marks;
    int terminated; /* current block ended in an unconditional jump */
    DA(Type *) slices;
    int needs_bounds;
} CG;

/* ---- mangling ---- */

/* Locals/params get zv_, globals zov_; mangled names colliding with the
 * runtime-provided zov_str/zov_slice_* typedefs get a suffix. */
static const char *mangle_local(const char *name)
{
    char *s = zalloc(strlen(name) + 8);
    sprintf(s, "zv_%s", name);
    return s;
}

static const char *mangle_fn(const char *name)
{
    if (zstr_eq(name, "main"))
        return name;
    size_t n = strlen(name);
    char *s = zalloc(n + 16);
    sprintf(s, "zov_%s", name);
    /* guard the runtime's own typedef names from user collisions */
    if (strcmp(s, "zov_str") == 0 || strncmp(s, "zov_slice_", 10) == 0)
        strcat(s, "_fn");
    return s;
}

static const char *mangle_type(const char *name)
{
    char *s = zalloc(strlen(name) + 16);
    sprintf(s, "zov_type_%s", name);
    return s;
}

/* ---- C type spelling ---- */

static const char *c_type(CG *cg, Type *t);

/* Slice lowering: { T *data; size_t len; } via a registered typedef. */
static const char *slice_name(CG *cg, Type *base)
{
    for (size_t i = 0; i < cg->slices.len; i++)
        if (cg->slices.items[i]->base == base) {
            char *s = zalloc(32);
            snprintf(s, 32, "zov_slice_%lu", (unsigned long)i);
            return s;
        }
    Type *t = type_slice(base);
    da_push(&cg->slices, t);
    char *s = zalloc(32);
    snprintf(s, 32, "zov_slice_%lu", (unsigned long)(cg->slices.len - 1));
    return s;
}

static const char *c_type(CG *cg, Type *t)
{
    switch (t->kind) {
    case TY_VOID: return "void";
    case TY_I8: return "int8_t";
    case TY_I16: return "int16_t";
    case TY_I32: return "int32_t";
    case TY_I64: return "int64_t";
    case TY_ISIZE: return "intptr_t";
    case TY_U8: return "uint8_t";
    case TY_U16: return "uint16_t";
    case TY_U32: return "uint32_t";
    case TY_U64: return "uint64_t";
    case TY_USIZE: return "uintptr_t";
    case TY_F32: return "float";
    case TY_F64: return "double";
    case TY_BOOL: return "bool";
    case TY_CHAR: return "uint32_t";
    case TY_STR: return "zov_str";
    case TY_PTR:
    case TY_REF:
    case TY_MUTREF: {
        /* base spelling then " *" so declarators read T *name */
        char *s = zalloc(128);
        snprintf(s, 128, "%s *", c_type(cg, t->base));
        return s;
    }
    case TY_SLICE:
        return slice_name(cg, t->base);
    case TY_ARRAY:
        return c_type(cg, t->base);
    case TY_STRUCT:
        return mangle_type(t->name);
    default:
        fatal("cannot emit type %s", type_spelling(t));
        return NULL;
    }
}

static void emit_var(Buf *b, CG *cg, Type *t, const char *name)
{
    buf_printf(b, "%s %s", c_type(cg, t), name);
    if (t->kind == TY_ARRAY)
        buf_printf(b, "[%lu]", (unsigned long)t->count);
}

/* ---- literals ---- */

static void emit_str_lit(Buf *b, const char *s, size_t len)
{
    buf_puts(b, "\"");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': buf_puts(b, "\\n"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        default:
            if (c >= 32 && c < 127)
                buf_printf(b, "%c", c);
            else
                /* octal escape: exactly 3 digits, immune to hex-run-on */
                buf_printf(b, "\\%03o", (unsigned)c);
        }
    }
    buf_puts(b, "\"");
}

static void emit_zov_str(Buf *b, const char *s, size_t len)
{
    buf_puts(b, "(zov_str){ (const uint8_t *)");
    emit_str_lit(b, s, len);
    buf_printf(b, ", %luu }", (unsigned long)len);
}

/* ---- expressions ---- */

static void emit_expr(CG *cg, Expr *e, int cstr);

static void emit_call(CG *cg, Expr *e)
{
    Expr *callee = e->u.call.callee;
    int extern_cstr = (callee->kind == EX_NS_IDENT);
    if (callee->kind == EX_NS_IDENT) {
        /* extern C symbol: unmangled, and str literals pass as raw char* */
        buf_printf(&*cg->cur, "%s(", callee->u.nident.name);
    } else {
        buf_printf(&*cg->cur, "%s(", mangle_fn(callee->u.ident));
    }
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        if (i)
            buf_puts(&*cg->cur, ", ");
        emit_expr(cg, e->u.call.args.items[i], extern_cstr);
    }
    buf_puts(&*cg->cur, ")");
}

static void emit_expr(CG *cg, Expr *e, int cstr)
{
    Buf *b = &*cg->cur;
    switch (e->kind) {
    case EX_INT:
        buf_printf(b, "((%s)%luu)", c_type(cg, e->type),
                   (unsigned long)e->u.ilit.v);
        break;
    case EX_FLOAT:
        if (e->type->kind == TY_F32)
            buf_printf(b, "((float)%.*gf)", 17, e->u.flit.v);
        else
            buf_printf(b, "((double)%.*g)", 17, e->u.flit.v);
        break;
    case EX_STR:
        if (cstr)
            emit_str_lit(b, e->u.slit.v, e->u.slit.len);
        else
            emit_zov_str(b, e->u.slit.v, e->u.slit.len);
        break;
    case EX_CHAR:
        buf_printf(b, "((uint32_t)%luu)", (unsigned long)e->u.clit.v);
        break;
    case EX_BOOL:
        buf_puts(b, e->u.blit.v ? "true" : "false");
        break;
    case EX_NULL:
        buf_puts(b, "NULL");
        break;
    case EX_IDENT:
        buf_puts(b, mangle_local(e->u.ident));
        break;
    case EX_NS_IDENT:
        buf_puts(b, e->u.nident.name);
        break;
    case EX_ASSIGN:
        emit_expr(cg, e->u.assign.lhs, 0);
        buf_puts(b, " = ");
        emit_expr(cg, e->u.assign.rhs, 0);
        break;
    case EX_BINARY: {
        const char *op = NULL;
        switch (e->u.binary.op) {
        case TK_PLUS: op = "+"; break;
        case TK_MINUS: op = "-"; break;
        case TK_STAR: op = "*"; break;
        case TK_SLASH: op = "/"; break;
        case TK_PERCENT: op = "%"; break;
        case TK_LT: op = "<"; break;
        case TK_GT: op = ">"; break;
        case TK_LE: op = "<="; break;
        case TK_GE: op = ">="; break;
        case TK_EQ: op = "=="; break;
        case TK_NEQ: op = "!="; break;
        case TK_ANDAND: op = "&&"; break;
        case TK_OROR: op = "||"; break;
        default: fatal("unsupported binary operator"); break;
        }
        buf_puts(b, "(");
        emit_expr(cg, e->u.binary.lhs, 0);
        buf_printf(b, " %s ", op);
        emit_expr(cg, e->u.binary.rhs, 0);
        buf_puts(b, ")");
        break;
    }
    case EX_UNARY:
        buf_puts(b, "(");
        if (e->u.unary.op == TK_MINUS)
            buf_puts(b, "-");
        else if (e->u.unary.op == TK_BANG)
            buf_puts(b, "!");
        else if (e->u.unary.op == TK_AMP)
            buf_puts(b, "&");
        else
            fatal("unsupported unary operator");
        emit_expr(cg, e->u.unary.e, 0);
        buf_puts(b, ")");
        break;
    case EX_CALL:
        emit_call(cg, e);
        break;
    case EX_INDEX: {
        cg->needs_bounds = 1;
        /* sizeof operand is unevaluated, so duplicating the base expr
         * for the length computation is side-effect free */
        Type *bt = e->u.index.base->type;
        if (bt && bt->kind == TY_ARRAY) {
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, "[zov_bounds((size_t)(");
            emit_expr(cg, e->u.index.index, 0);
            buf_puts(b, "), sizeof(");
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ") / sizeof((");
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ")[0]))]");
        } else {
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ".data[zov_bounds((size_t)(");
            emit_expr(cg, e->u.index.index, 0);
            buf_puts(b, "), ");
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ".len)]");
        }
        break;
    }
    case EX_CAST:
        buf_printf(b, "((%s)(", c_type(cg, e->u.cast.t));
        emit_expr(cg, e->u.cast.e, 0);
        buf_puts(b, "))");
        break;
    case EX_GENERIC:
        fatal("generic instantiation is not supported in v0.1");
        break;
    case EX_MATCH:
    case EX_TRY:
    case EX_UNWRAP:
    case EX_COERCE:
        fatal("match/?/ lowering not implemented yet (v0.2 milestone 5)");
        break;
    }
}

/* ---- defer unrolling ---- */

/* Emit active defers from stack top down to `base` in LIFO order. Each
 * runs inside its own braced scope so inner lets cannot collide. */
static int emit_block(CG *cg, Block *blk);
static int emit_stmt(CG *cg, Stmt *s);

static void emit_defers_down_to(CG *cg, size_t base)
{
    for (size_t i = cg->defers.len; i > base; i--) {
        Stmt *ds = cg->defers.items[i - 1].stmt;
        buf_puts(&*cg->cur, "{ ");
        cg->terminated = 0;
        emit_stmt(cg, ds);
        cg->terminated = 0;
        buf_puts(&*cg->cur, " }\n");
    }
}

/* Returns 1 when every path through the block terminates in a jump. */
static int emit_block(CG *cg, Block *blk);

static int emit_stmt(CG *cg, Stmt *s)
{
    switch (s->kind) {
    case ST_LET: {
        emit_var(&*cg->cur, cg, s->u.let.ty, mangle_local(s->u.let.name));
        buf_puts(&*cg->cur, " = ");
        emit_expr(cg, s->u.let.init, 0);
        /* unused locals are legal in Zov; silence -Wall in the C output */
        buf_printf(&*cg->cur, ";\n(void)%s;\n", mangle_local(s->u.let.name));
        return 0;
    }
    case ST_DEFER: {
        DeferEntry de;
        de.stmt = s->u.defer;
        da_push(&cg->defers, de);
        return 0;
    }
    case ST_IF: {
        buf_puts(&*cg->cur, "if (");
        emit_expr(cg, s->u.iff.cond, 0);
        buf_puts(&*cg->cur, ") {\n");
        int t = emit_block(cg, s->u.iff.then_blk);
        buf_puts(&*cg->cur, "}\n");
        if (s->u.iff.else_stmt) {
            if (s->u.iff.else_stmt->kind == ST_IF) {
                buf_puts(&*cg->cur, "else ");
                int te = emit_stmt(cg, s->u.iff.else_stmt);
                t = t && te;
            } else {
                buf_puts(&*cg->cur, "else {\n");
                int te = emit_block(cg, s->u.iff.else_stmt->u.block);
                buf_puts(&*cg->cur, "}\n");
                t = t && te;
            }
        } else {
            t = 0;
        }
        return t;
    }
    case ST_WHILE: {
        buf_puts(&*cg->cur, "while (");
        emit_expr(cg, s->u.whilee.cond, 0);
        buf_puts(&*cg->cur, ") {\n");
        da_push(&cg->loop_marks, cg->defers.len);
        emit_block(cg, s->u.whilee.body);
        cg->loop_marks.len--;
        buf_puts(&*cg->cur, "}\n");
        return 0;
    }
    case ST_RETURN: {
        emit_defers_down_to(cg, 0);
        buf_puts(&*cg->cur, "return ");
        if (s->u.ret)
            emit_expr(cg, s->u.ret, 0);
        buf_puts(&*cg->cur, ";\n");
        cg->terminated = 1;
        return 1;
    }
    case ST_BREAK: {
        emit_defers_down_to(cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
        buf_puts(&*cg->cur, "break;\n");
        cg->terminated = 1;
        return 1;
    }
    case ST_CONTINUE: {
        emit_defers_down_to(cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
        buf_puts(&*cg->cur, "continue;\n");
        cg->terminated = 1;
        return 1;
    }
    case ST_EXPR:
        emit_expr(cg, s->u.expr, 0);
        buf_puts(&*cg->cur, ";\n");
        return 0;
    case ST_BLOCK:
        buf_puts(&*cg->cur, "{\n");
        emit_block(cg, s->u.block);
        buf_puts(&*cg->cur, "}\n");
        return 0;
    }
    return 0;
}

static int emit_block(CG *cg, Block *blk)
{
    size_t base = cg->defers.len;
    int prev = cg->terminated;
    cg->terminated = 0;
    int term = 0;
    for (size_t i = 0; i < blk->stmts.len; i++) {
        if (term)
            break; /* unreachable after an unconditional jump */
        term = emit_stmt(cg, blk->stmts.items[i]);
    }
    if (!term) {
        /* natural scope exit: run this block's defers LIFO */
        emit_defers_down_to(cg, base);
    }
    cg->defers.len = base;
    int t = term;
    cg->terminated = prev;
    return t;
}

/* ---- top level ---- */

static void emit_params(CG *cg, Decl *d)
{
    if (d->u.fn.params.len == 0) {
        buf_puts(&*cg->cur, "void");
        return;
    }
    for (size_t i = 0; i < d->u.fn.params.len; i++) {
        if (i)
            buf_puts(&*cg->cur, ", ");
        emit_var(&*cg->cur, cg, d->u.fn.params.items[i].ty,
                 mangle_local(d->u.fn.params.items[i].name));
    }
}

static void emit_fn(CG *cg, Decl *d, int proto_only)
{
    if (proto_only) {
        cg->cur = &cg->head;
        emit_var(&*cg->cur, cg, d->u.fn.ret, mangle_fn(d->u.fn.name));
        buf_puts(&*cg->cur, "(");
        emit_params(cg, d);
        buf_puts(&*cg->cur, ");\n");
        return;
    }

    cg->cur = &cg->body;
    emit_var(&*cg->cur, cg, d->u.fn.ret, mangle_fn(d->u.fn.name));
    buf_puts(&*cg->cur, "(");
    emit_params(cg, d);
    buf_puts(&*cg->cur, ")\n{\n");
    cg->defers.len = 0;
    int term = emit_block(cg, d->u.fn.body);
    if (d->u.fn.ret->kind != TY_VOID && !term)
        buf_puts(&*cg->cur, "return 0;\n");
    buf_puts(&*cg->cur, "}\n\n");
}

char *codegen_generate(Program *prog)
{
    CG *cg = zalloc(sizeof(CG));

    cg->cur = &cg->head;
    buf_puts(&*cg->cur,
             "#include <stdint.h>\n"
             "#include <stddef.h>\n"
             "#include <stdbool.h>\n");
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_USE && d->u.use.is_c_header)
            buf_printf(&*cg->cur, "#include <%s>\n", d->u.use.path);
    }
    buf_puts(&*cg->cur,
             "typedef struct { const uint8_t *data; size_t len; } zov_str;\n");

    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_TYPE)
            continue;
        cg->cur = &cg->head;
        buf_printf(&*cg->cur, "typedef struct {\n");
        for (size_t j = 0; j < d->u.type.fields.len; j++) {
            Field *f = &d->u.type.fields.items[j];
            buf_puts(&*cg->cur, "    ");
            emit_var(&*cg->cur, cg, f->ty, mangle_local(f->name));
            buf_puts(&*cg->cur, ";\n");
        }
        buf_printf(&*cg->cur, "} %s;\n", mangle_type(d->u.type.name));
    }

    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_FN)
            emit_fn(cg, d, 1);
    }
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_FN)
            emit_fn(cg, d, 0);
    }

    /* assemble final buffer: head + slice typedefs + body */
    Buf out = {0};
    buf_puts(&out, cg->head.data);
    if (cg->needs_bounds) {
        buf_puts(&out, "#include <stdio.h>\n#include <stdlib.h>\n"
                       "static size_t zov_bounds(size_t i, size_t n) {\n"
                       "    if (i >= n) {\n"
                       "        fprintf(stderr, \"zov: index out of bounds\\n\");\n"
                       "        abort();\n"
                       "    }\n"
                       "    return i;\n"
                       "}\n");
    }
    for (size_t i = 0; i < cg->slices.len; i++) {
        Type *s = cg->slices.items[i];
        buf_puts(&out, "typedef struct { ");
        emit_var(&out, cg, s->base, "*data");
        buf_puts(&out, "; size_t len; } zov_slice_");
        buf_printf(&out, "%lu", (unsigned long)i);
        buf_puts(&out, ";\n");
    }
    buf_puts(&out, cg->body.data);
    char *result = malloc(out.len + 1);
    if (!result)
        fatal("out of memory");
    memcpy(result, out.data, out.len + 1);
    free(out.data);
    free(cg->head.data);
    free(cg->body.data);
    return result;
}
