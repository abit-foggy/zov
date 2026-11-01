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
    if (!s || !*s)
        return;
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
    Buf head; /* includes, struct typedefs */
    Buf protos; /* function prototypes (after union typedefs) */
    Buf body; /* function definitions */
    Buf str_helpers; /* generated C99 concat helpers */
    int needs_string_owned; /* zov_type_String runtime */
    int needs_string_init;
    int needs_string_view;
    int needs_string_append;
    int needs_string_new;
    int needs_string_from_str;
    int needs_string_free;
    int needs_print; /* direct std::io streaming */
    Buf *cur; /* active expression/statement target */
    Buf prebuf; /* statement-local prelude for hoisted expressions */
    Buf *pre;   /* active prelude target */
    DA(DeferEntry) defers;
    DA(size_t) loop_marks; /* defer-stack base per active loop */
    DA(int) loop_ids;      /* label id per active loop */
    int terminated; /* current block ended in an unconditional jump */
    DA(Type *) slices;
    DA(Type *) unions;
    DA(const char *) enum_names; /* enums needing a name table */
    int needs_bounds;
    int needs_abort;
    int needs_panic; /* hosted debug panic helper referenced */
    int needs_io;   /* str-concat and diagnostic emission */
    int needs_utf8; /* char formatting helper */
    int needs_string; /* memcmp emission for native str equality */
    int needs_string_eq;
    int needs_hash_str;
    int needs_str_index; /* checked byte access helper for String */
    int needs_slice; /* runtime slice bounds helper */
    int needs_env;   /* std::env runtime helpers */
    int needs_strcmp; /* lexical str ordering helper */
    int needs_hash;  /* hash_val helpers for std::map */
    int freestanding; /* bare-metal: no hosted headers or main scaffolding */
    int debug;       /* 1 = pretty runtime panics with file:line:col */
    Type *fn_ret;    /* enclosing function return type */
    int switch_depth; /* lowered match switch nesting */
    int tmp_id;       /* unique temp/label counter */
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

/* Tagged unions register deterministically: tag == member index. */
static const char *union_name(CG *cg, Type *u)
{
    for (size_t i = 0; i < cg->unions.len; i++)
        if (type_eq(cg->unions.items[i], u)) {
            char *s = zalloc(32);
            snprintf(s, 32, "zov_union_%lu", (unsigned long)i);
            return s;
        }
    da_push(&cg->unions, u);
    char *s = zalloc(32);
    snprintf(s, 32, "zov_union_%lu", (unsigned long)(cg->unions.len - 1));
    return s;
}

static int member_has_payload(Type *m)
{
    return m->kind != TY_VOID && !(m->kind == TY_ERR
                                   && m->base->kind == TY_VOID);
}

static Type *union_ok_member(Type *u)
{
    for (size_t i = 0; i < u->members.len; i++)
        if (!u->members.items[i]->is_error)
            return u->members.items[i];
    return NULL;
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
    case TY_ERR:
        /* error payloads lower to their storage type */
        return t->base->kind == TY_VOID ? "uint8_t" : c_type(cg, t->base);
    case TY_UNION:
        return union_name(cg, t);
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
    case TY_ENUM:
        return mangle_type(t->name);
    case TY_STRUCT:
        if (t->c_name)
            return t->c_name; /* imported C type: emit verbatim */
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
        if (c == '\x01')
            c = '{'; /* '\{' marker from the lexer */
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

/* '\{' marker from the lexer decodes back to a literal '{' in C text */

static int ty_is_owned_string(Type *t);
static int ty_is_stringish(Type *t);

/* str/String concat flattening: left-assoc '+' chains collapse into an
 * ordered segment list before any allocation or I/O is emitted */
static void flatten_concat(CG *cg, Expr *e, ExprList *segs)
{
    (void)cg;
    if (e->kind == EX_BINARY && e->u.binary.op == TK_PLUS && e->type
        && ty_is_stringish(e->type)) {
        flatten_concat(cg, e->u.binary.lhs, segs);
        flatten_concat(cg, e->u.binary.rhs, segs);
        return;
    }
    da_push(segs, e);
}

/* ---- expressions ---- */

static void emit_expr(CG *cg, Expr *e, int cstr);
static int emit_block(CG *cg, Block *blk);
static int emit_stmt(CG *cg, Stmt *s);
static int emit_stmt_body(CG *cg, Stmt *s);
static void emit_defers_down_to(CG *cg, size_t base);

/* Emit e's C text into out while routing any hoisted prelude to pre. */
static void buf_append(Buf *dst, Buf *src);

static void emit_expr_capture(CG *cg, Expr *e, Buf *out, Buf *pre, int cstr)
{
    Buf *saved_cur = cg->cur;
    Buf *saved_pre = cg->pre;
    Buf local_pre = {0};
    cg->cur = out;
    cg->pre = &local_pre;
    emit_expr(cg, e, cstr);
    cg->cur = saved_cur;
    cg->pre = saved_pre;
    buf_append(pre, &local_pre);
    free(local_pre.data);
}

/* Capture e into out and append its prelude to the active statement. */
static void emit_expr_into(CG *cg, Expr *e, Buf *out, int cstr)
{
    emit_expr_capture(cg, e, out, cg->pre, cstr);
}

/* Emit two operands left-to-right.  When the right operand needs a
 * statement prelude, the left operand is spilled to a temp first so its
 * side effects stay ahead of the hoisted statements. */
static int emit_ordered_operands(CG *cg, Expr *lhs, Expr *rhs, Buf *lout,
                                 Buf *rout)
{
    Buf left = {0};
    Buf right = {0};
    Buf pre_left = {0};
    Buf pre_right = {0};
    emit_expr_capture(cg, lhs, &left, &pre_left, 0);
    emit_expr_capture(cg, rhs, &right, &pre_right, 0);
    buf_append(cg->pre, &pre_left);
    if (pre_right.len > 0) {
        int id = cg->tmp_id++;
        buf_printf(cg->pre, "%s _bo%d = ", c_type(cg, lhs->type), id);
        buf_append(cg->pre, &left);
        buf_puts(cg->pre, ";\n");
        buf_printf(lout, "_bo%d", id);
    } else {
        buf_append(lout, &left);
    }
    buf_append(cg->pre, &pre_right);
    buf_append(rout, &right);
    int hoisted = pre_right.len > 0;
    free(left.data);
    free(right.data);
    free(pre_left.data);
    free(pre_right.data);
    return hoisted;
}

/* Wrap an already-emitted expression in a zov_str view when needed. */
static void emit_view_text(CG *cg, Expr *e, Buf *text, Buf *out)
{
    if (ty_is_owned_string(e->type)) {
        cg->needs_string_owned = 1;
        cg->needs_string_view = 1;
        buf_puts(out, "zov_string_view(");
        buf_append(out, text);
        buf_puts(out, ")");
        return;
    }
    buf_append(out, text);
}

/* Emit defers into the active prelude (used by conditional returns). */
static void emit_defers_to_pre(CG *cg, size_t base)
{
    Buf *saved_cur = cg->cur;
    Buf *saved_pre = cg->pre;
    Buf local_pre = {0};
    cg->cur = cg->pre;
    cg->pre = &local_pre;
    emit_defers_down_to(cg, base);
    cg->cur = saved_cur;
    cg->pre = saved_pre;
    free(local_pre.data);
}

static void buf_append(Buf *dst, Buf *src)
{
    if (src && src->data)
        buf_puts(dst, src->data);
}

static void append_fmt_str(Buf *b, const char *s, size_t len)
{
    /* decoded bytes into printf format text: '%' doubles */
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '%') {
            buf_puts(b, "%%");
            continue;
        }
        if (c == '\x01')
            c = '{'; /* escaped '\{' marker */
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
                buf_printf(b, "\\%03o", (unsigned char)c);
        }
    }
}

static const char *int_spec(TypeKind k)
{
    switch (k) {
    case TY_I8: case TY_I16: case TY_I32: return "PRId32";
    case TY_I64: case TY_ISIZE: return "PRId64";
    case TY_U8: case TY_U16: case TY_U32: return "PRIu32";
    default: return "PRIu64";
    }
}

static int ty_is_owned_string(Type *t)
{
    return t && t->kind == TY_STRUCT && t->name
        && zstr_eq(t->name, "String");
}

static int ty_is_stringish(Type *t)
{
    return t && (t->kind == TY_STR || ty_is_owned_string(t));
}

/* Emit a zov_str view for either a borrowed str or an owned String. */
static void emit_str_view(CG *cg, Expr *e)
{
    if (ty_is_owned_string(e->type)) {
        cg->needs_string_owned = 1;
        cg->needs_string_view = 1;
        buf_puts(&*cg->cur, "zov_string_view(");
        emit_expr(cg, e, 0);
        buf_puts(&*cg->cur, ")");
        return;
    }
    emit_expr(cg, e, 0);
}

/* One raw helper parameter plus the statements that turn it into a view. */
static void emit_seg_param(CG *cg, Expr *e, int idx, Buf *sig, Buf *body)
{
    Type *t = e->type;
    if (t->kind == TY_STR) {
        buf_printf(sig, "%szov_str a%d", idx ? ", " : "", idx);
        buf_printf(body, "    zov_str _sz%d = a%d;\n", idx, idx);
        return;
    }
    if (ty_is_owned_string(t)) {
        cg->needs_string_owned = 1;
        cg->needs_string_view = 1;
        buf_printf(sig, "%szov_type_String a%d", idx ? ", " : "", idx);
        buf_printf(body, "    zov_str _sz%d = zov_string_view(a%d);\n",
                   idx, idx);
        return;
    }
    if (t->kind == TY_BOOL) {
        buf_printf(sig, "%sbool a%d", idx ? ", " : "", idx);
        buf_printf(body,
                   "    zov_str _sz%d = { (const uint8_t *)(a%d ? "
                   "\"true\" : \"false\"), a%d ? 4u : 5u };\n",
                   idx, idx, idx);
        return;
    }
    if (t->kind == TY_CHAR) {
        cg->needs_io = 1;
        cg->needs_utf8 = 1;
        buf_printf(sig, "%suint32_t a%d", idx ? ", " : "", idx);
        buf_printf(body,
                   "    char _scb%d[5]; int _scl%d = zov_utf8(_scb%d, a%d);\n"
                   "    zov_str _sz%d = { (const uint8_t *)_scb%d, "
                   "(size_t)_scl%d };\n",
                   idx, idx, idx, idx, idx, idx, idx);
        return;
    }
    if (t->kind == TY_ENUM) {
        cg->needs_string = 1;
        int seen = 0;
        for (size_t i = 0; i < cg->enum_names.len; i++)
            if (cg->enum_names.items[i] == t->name)
                seen = 1;
        if (!seen)
            da_push(&cg->enum_names, t->name);
        buf_printf(sig, "%s%s a%d", idx ? ", " : "", c_type(cg, t), idx);
        buf_printf(body,
                   "    const char *_snm%d = zov_enum_%s_name(a%d);\n"
                   "    zov_str _sz%d = { (const uint8_t *)_snm%d, "
                   "strlen(_snm%d) };\n",
                   idx, t->name, idx, idx, idx, idx);
        return;
    }
    if (t->kind == TY_PTR && t->nullable) {
        cg->needs_io = 1;
        buf_printf(sig, "%svoid *a%d", idx ? ", " : "", idx);
        buf_printf(body, "    char _spb%d[32];\n"
                         "    zov_str _sz%d;\n"
                         "    if (a%d == NULL) {\n"
                         "        _sz%d.data = (const uint8_t *)\"null\";\n"
                         "        _sz%d.len = 4u;\n"
                         "    } else {\n"
                         "        int _spl%d = snprintf(_spb%d, "
                         "sizeof _spb%d, \"%%p\", a%d);\n"
                         "        _sz%d.data = (const uint8_t *)_spb%d;\n"
                         "        _sz%d.len = _spl%d < 0 ? 0u : "
                         "(size_t)_spl%d;\n"
                         "    }\n",
                   idx, idx, idx, idx, idx, idx, idx, idx, idx, idx, idx,
                   idx, idx, idx);
        return;
    }
    if (ty_is_num(t->kind) || t->kind == TY_PTR) {
        cg->needs_io = 1;
        buf_printf(sig, "%s%s a%d", idx ? ", " : "", c_type(cg, t), idx);
        buf_printf(body, "    char _snb%d[128]; int _snl%d = snprintf("
                         "_snb%d, sizeof _snb%d, ", idx, idx, idx, idx);
        if (t->kind == TY_PTR)
            buf_printf(body, "\"%%p\", (void *)a%d);\n", idx);
        else if (ty_is_float(t->kind))
            buf_printf(body, "\"%%g\", (double)a%d);\n", idx);
        else
            buf_printf(body, "\"%%\" %s, a%d);\n", int_spec(t->kind), idx);
        buf_printf(body,
                   "    zov_str _sz%d = { (const uint8_t *)_snb%d, "
                   "(size_t)(_snl%d < 0 ? 0 : _snl%d) };\n",
                   idx, idx, idx, idx);
        return;
    }
    fatal("internal: unprintable concat segment");
}

static void emit_seg_args(CG *cg, ExprList *segs, Buf *b)
{
    for (size_t i = 0; i < segs->len; i++) {
        if (i)
            buf_puts(b, ", ");
        emit_expr(cg, segs->items[i], 0);
    }
}

/* Concatenation returns an owned String built by a generated C99 helper.
 * Each operand is evaluated once at the call site, then appended by length;
 * embedded NUL bytes survive and there is no fixed-size truncation. */
static void emit_str_value(CG *cg, Expr *e)
{
    Buf *b = &*cg->cur;
    ExprList segs = {0};
    flatten_concat(cg, e, &segs);
    Buf sig = {0};
    Buf body = {0};
    for (size_t i = 0; i < segs.len; i++)
        emit_seg_param(cg, segs.items[i], (int)i, &sig, &body);
    int helper = cg->tmp_id++;
    buf_printf(b, "_zov_cat%d(", helper);
    emit_seg_args(cg, &segs, b);
    buf_puts(b, ")");

    cg->needs_string_owned = 1;
    cg->needs_string_init = 1;
    cg->needs_string_append = 1;
    buf_printf(&cg->str_helpers, "static zov_type_String _zov_cat%d(",
               helper);
    buf_puts(&cg->str_helpers, sig.len ? sig.data : "void");
    buf_puts(&cg->str_helpers, ")\n{\n"
                              "    zov_type_String out;\n"
                              "    zov_string_init(&out);\n");
    if (body.len)
        buf_puts(&cg->str_helpers, body.data);
    for (size_t i = 0; i < segs.len; i++)
        buf_printf(&cg->str_helpers,
                   "    zov_string_append(&out, _sz%lu);\n",
                   (unsigned long)i);
    buf_puts(&cg->str_helpers, "    return out;\n}\n\n");
    free(segs.items);
    free(sig.data);
    free(body.data);
}

/* std::io::{print,println} stream directly: no temporary String is built. */
static void emit_print_call(CG *cg, Expr *e, int newline)
{
    Expr *arg = e->u.call.args.items[0];
    Buf *b = &*cg->cur;
    ExprList segs = {0};
    if (ty_is_stringish(arg->type))
        flatten_concat(cg, arg, &segs);
    else
        da_push(&segs, arg);
    Buf sig = {0};
    Buf body = {0};
    for (size_t i = 0; i < segs.len; i++)
        emit_seg_param(cg, segs.items[i], (int)i, &sig, &body);
    int helper = cg->tmp_id++;
    cg->needs_io = 1;
    buf_printf(b, "_zov_print%d(", helper);
    emit_seg_args(cg, &segs, b);
    buf_puts(b, ")");

    buf_printf(&cg->str_helpers, "static void _zov_print%d(", helper);
    buf_puts(&cg->str_helpers, sig.len ? sig.data : "void");
    buf_puts(&cg->str_helpers, ")\n{\n");
    if (body.len)
        buf_puts(&cg->str_helpers, body.data);
    for (size_t i = 0; i < segs.len; i++)
        buf_printf(&cg->str_helpers,
                   "    if (_sz%lu.len) fwrite(_sz%lu.data, 1, "
                   "_sz%lu.len, stdout);\n",
                   (unsigned long)i, (unsigned long)i, (unsigned long)i);
    if (newline)
        buf_puts(&cg->str_helpers, "    fputc(10, stdout);\n");
    buf_puts(&cg->str_helpers, "}\n\n");
    free(segs.items);
    free(sig.data);
    free(body.data);
}

static int cg_union_idx(Type *u, Type *m)
{
    for (size_t i = 0; i < u->members.len; i++) {
        Type *x = u->members.items[i];
        if (x->kind == TY_ERR && m->kind == TY_ERR && x->name == m->name)
            return (int)i;
        if (type_eq(x, m))
            return (int)i;
    }
    return -1;
}

/* Resolve a pattern arm to its union member; NULL for null/wildcards. */
static Type *arm_member(Type *st, MatchArm *a)
{
    /* qualified error names (mod::NotFound) resolve by suffix */
    const char *aname = a->name;
    const char *qsep = a->name ? strstr(a->name, "::") : NULL;
    if (qsep)
        aname = qsep + 2;
    for (size_t k = 0; k < st->members.len; k++) {
        Type *m = st->members.items[k];
        if (a->kind == PAT_TYPE_VAL) {
            if (m->kind == TY_ERR && zstr_eq(m->name, aname))
                return m;
            if (zstr_eq(type_spelling(m), aname))
                return m;
        } else if (a->kind == PAT_TYPE_BIND && type_eq(m, a->ty)) {
            return m;
        }
    }
    return NULL;
}

static void emit_arm_body(CG *cg, MatchArm *a, const char *res)
{
    if (a->jump) {
        emit_stmt(cg, a->jump);
        return;
    }
    Buf val = {0};
    Buf arm_pre = {0};
    emit_expr_capture(cg, a->val, &val, &arm_pre, 0);
    buf_append(cg->cur, &arm_pre);
    if (res)
        buf_printf(&*cg->cur, "%s = ", res);
    buf_append(cg->cur, &val);
    buf_puts(&*cg->cur, ";\n");
    free(val.data);
    free(arm_pre.data);
}

static void emit_union_cases(CG *cg, Expr *e, int id, const char *res)
{
    Buf *b = &*cg->cur;
    Type *st = e->u.match.scrutinee->type;
    buf_printf(b, "%s _m%d = (", union_name(cg, st), id);
    emit_expr(cg, e->u.match.scrutinee, 0);
    buf_printf(b, "); switch (_m%d.tag) {\n", id);
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        Type *m = arm_member(st, a);
        if (a->kind == PAT_WILDCARD) {
            buf_printf(b, "default: {\n");
        } else {
            int idx = cg_union_idx(st, m);
            buf_printf(b, "case %du: {\n", idx);
            if (a->kind == PAT_TYPE_BIND && a->name) {
                emit_var(b, cg, a->ty, mangle_local(a->name));
                buf_printf(b, " = _m%d.u.v%d;\n", id, idx);
            }
        }
        emit_arm_body(cg, a, res);
        buf_printf(b, "} break;\n");
    }
    buf_printf(b, "}\n");
}

static void emit_nullable_cases(CG *cg, Expr *e, int id, const char *res)
{
    Buf *b = &*cg->cur;
    Type *st = e->u.match.scrutinee->type;
    buf_printf(b, "%s *_m%d = (", c_type(cg, st->base), id);
    emit_expr(cg, e->u.match.scrutinee, 0);
    buf_printf(b, "); if (_m%d == NULL) {\n", id);
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        if (a->kind == PAT_NULL) {
            emit_arm_body(cg, a, res);
            break;
        }
    }
    buf_printf(b, "} else {\n");
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        if (a->kind == PAT_TYPE_BIND && a->name) {
            emit_var(b, cg, a->ty, mangle_local(a->name));
            buf_printf(b, " = _m%d;\n", id);
            emit_arm_body(cg, a, res);
            break;
        }
    }
    buf_printf(b, "}\n");
}

/* enum/scalar switch arms share the union body-emission shape */
static void emit_value_cases(CG *cg, Expr *e, int id, const char *res,
                             const char *ctype)
{
    Buf *b = &*cg->cur;
    Type *st = e->u.match.scrutinee->type;
    buf_printf(b, "%s _m%d = (", ctype, id);
    emit_expr(cg, e->u.match.scrutinee, 0);
    buf_printf(b, "); switch (_m%d) {\n", id);
    int emitted_default = 0;
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        if (a->kind == PAT_WILDCARD) {
            buf_printf(b, "default: {\n");
            emitted_default = 1;
        } else if (a->kind == PAT_LIT && a->pat->kind == EX_STR) {
            /* strings cannot ride a switch: emit as int length case? no */
            fatal("internal: str pattern in value switch");
        } else if (a->kind == PAT_LIT) {
            buf_puts(b, "case ");
            if (a->pat->kind == EX_CHAR)
                buf_printf(b, "%luu", (unsigned long)a->pat->u.clit.v);
            else if (a->pat->kind == EX_BOOL)
                buf_printf(b, "%d", a->pat->u.blit.v ? 1 : 0);
            else
                buf_printf(b, "%luu", (unsigned long)a->pat->u.ilit.v);
            buf_puts(b, ": {\n");
        } else if (a->variant_c) {
            buf_printf(b, "case %s: {\n", a->variant_c);
        } else {
            fatal("internal: unsupported match arm in value switch");
        }
        emit_arm_body(cg, a, res);
        buf_printf(b, "} break;\n");
    }
    (void)st;
    (void)emitted_default;
    buf_printf(b, "}\n");
}

/* str match: literals compare via len + memcmp, wildcard closes */
static void emit_str_cases(CG *cg, Expr *e, int id, const char *res)
{
    Buf *b = &*cg->cur;
    cg->needs_string = 1;
    buf_printf(b, "zov_str _m%d = (", id);
    emit_expr(cg, e->u.match.scrutinee, 0);
    buf_printf(b, ");\n");
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        if (a->kind == PAT_WILDCARD) {
            buf_printf(b, "{\n");
            emit_arm_body(cg, a, res);
            buf_printf(b, "}\n");
            return;
        }
        buf_printf(b, "if (_m%d.len == %luu && memcmp(_m%d.data, \"", id,
                   (unsigned long)a->pat->u.slit.len, id);
        append_fmt_str(b, a->pat->u.slit.v, a->pat->u.slit.len);
        buf_printf(b, "\", %luu) == 0) {\n",
                   (unsigned long)a->pat->u.slit.len);
        emit_arm_body(cg, a, res);
        buf_printf(b, "} else ");
    }
    /* no wildcard: sema rejects, but close the chain defensively */
    buf_printf(b, "{ }\n");
}

/* Value matches hoist a result temp and switch into the statement prelude;
 * statement matches emit the switch in place.  No braced groups. */
static void emit_match(CG *cg, Expr *e, int want_value)
{
    Type *st = e->u.match.scrutinee->type;
    int id = cg->tmp_id++;
    Buf *b = &*cg->cur;
    if (st && st->kind == TY_ENUM) {
        /* rewrite PAT_TYPE_VAL arms to their C variant constants */
        for (size_t i = 0; i < e->u.match.arms.len; i++) {
            MatchArm *a = &e->u.match.arms.items[i];
            if (a->kind == PAT_TYPE_VAL && a->name) {
                const char *sep = strstr(a->name, "::");
                char cbuf[256];
                snprintf(cbuf, sizeof(cbuf), "Zov_%s_%s", st->name,
                         sep ? sep + 2 : a->name);
                a->variant_c = str_intern_c(cbuf);
            }
        }
    }
    int as_value = want_value && e->type && e->type->kind != TY_VOID;
    cg->switch_depth++;
    if (as_value) {
        char res[24];
        snprintf(res, sizeof(res), "_zv_mres%d", id);
        emit_var(cg->pre, cg, e->type, res);
        buf_puts(cg->pre, ";\n");
        Buf sw = {0};
        Buf *saved = cg->cur;
        cg->cur = &sw;
        if (st->kind == TY_UNION)
            emit_union_cases(cg, e, id, res);
        else if (st->kind == TY_PTR && st->nullable)
            emit_nullable_cases(cg, e, id, res);
        else if (st->kind == TY_STR)
            emit_str_cases(cg, e, id, res);
        else
            emit_value_cases(cg, e, id, res, c_type(cg, st));
        cg->cur = saved;
        buf_append(cg->pre, &sw);
        free(sw.data);
        buf_puts(b, res);
    } else {
        buf_puts(b, "{\n");
        if (st->kind == TY_UNION)
            emit_union_cases(cg, e, id, NULL);
        else if (st->kind == TY_PTR && st->nullable)
            emit_nullable_cases(cg, e, id, NULL);
        else if (st->kind == TY_STR)
            emit_str_cases(cg, e, id, NULL);
        else
            emit_value_cases(cg, e, id, NULL, c_type(cg, st));
        buf_puts(b, "}\n");
    }
    cg->switch_depth--;
}

static void emit_assert(CG *cg, Expr *e, int as_stmt)
{
    Buf *b = &*cg->cur;
    cg->needs_abort = 1;
    Expr *cond = e->u.call.args.items[0];
    if (!as_stmt) {
        int id = cg->tmp_id++;
        Buf ct = {0};
        emit_expr_into(cg, cond, &ct, 0);
        buf_printf(&cg->protos, "static int zov_assert_%d(bool ok) {\n", id);
        buf_puts(&cg->protos, "    if (ok) return 0;\n    ");
        if (cg->freestanding) {
            buf_puts(&cg->protos, "zov_panic(\"assert failed: ");
            append_fmt_str(&cg->protos, cond->span.start,
                           (size_t)(cond->span.end - cond->span.start));
            buf_puts(&cg->protos, "\", ");
            emit_str_lit(&cg->protos, cond->span.file,
                         strlen(cond->span.file));
            buf_printf(&cg->protos, ", %luu);\n",
                       (unsigned long)cond->span.line);
        } else if (cg->debug) {
            cg->needs_io = 1;
            cg->needs_panic = 1;
            buf_puts(&cg->protos, "__zov_panic(\"assert failed: ");
            append_fmt_str(&cg->protos, cond->span.start,
                           (size_t)(cond->span.end - cond->span.start));
            buf_puts(&cg->protos, "\", ");
            emit_str_lit(&cg->protos, cond->span.file,
                         strlen(cond->span.file));
            buf_printf(&cg->protos, ", %luu, 0);\n",
                       (unsigned long)cond->span.line);
        } else {
            buf_puts(&cg->protos, "abort();\n");
        }
        buf_puts(&cg->protos, "    return 0;\n}\n");
        buf_printf(b, "zov_assert_%d(", id);
        buf_append(b, &ct);
        buf_puts(b, ")");
        free(ct.data);
        return;
    }
    buf_puts(b, "if (!(");
    emit_expr(cg, cond, 0);
    if (cg->freestanding) {
        buf_puts(b, ")) { zov_panic(\"assert failed: ");
        append_fmt_str(b, cond->span.start,
                       (size_t)(cond->span.end - cond->span.start));
        buf_puts(b, "\", ");
        emit_str_lit(b, cond->span.file, strlen(cond->span.file));
        buf_printf(b, ", %luu); }\n", (unsigned long)cond->span.line);
    } else if (cg->debug) {
        cg->needs_io = 1;
        cg->needs_panic = 1;
        buf_puts(b, ")) { __zov_panic(\"assert failed: ");
        append_fmt_str(b, cond->span.start,
                       (size_t)(cond->span.end - cond->span.start));
        buf_puts(b, "\", ");
        emit_str_lit(b, cond->span.file, strlen(cond->span.file));
        buf_printf(b, ", %luu, 0); }\n", (unsigned long)cond->span.line);
    } else {
        cg->needs_abort = 1;
        buf_puts(b, ")) { abort(); }\n");
    }
}

/* Evaluate call arguments left-to-right.  A later argument that hoists a
 * statement prelude forces every earlier argument text into a temp first. */
static void emit_call_args(CG *cg, Expr *e, int extern_cstr)
{
    size_t n = e->u.call.args.len;
    Buf *texts = n ? calloc(n, sizeof(Buf)) : NULL;
    if (n && !texts)
        fatal("out of memory");
    size_t unsaved = 0;
    for (size_t i = 0; i < n; i++) {
        Expr *arg = e->u.call.args.items[i];
        int literal_cstr = extern_cstr && arg->type
                           && arg->type->kind == TY_STR
                           && arg->kind == EX_STR;
        int value_cstr = extern_cstr && arg->type
                         && arg->type->kind == TY_STR && !literal_cstr;
        Buf txt = {0};
        Buf pre = {0};
        emit_expr_capture(cg, arg, &txt, &pre, literal_cstr);
        if (pre.len > 0) {
            for (size_t j = unsaved; j < i; j++) {
                Expr *prev = e->u.call.args.items[j];
                int prev_lit = extern_cstr && prev->type
                               && prev->type->kind == TY_STR
                               && prev->kind == EX_STR;
                const char *ty = prev_lit ? "char *" : c_type(cg, prev->type);
                int id = cg->tmp_id++;
                if (texts[j].len == 0)
                    continue;
                buf_printf(cg->pre, "%s _ca%d = ", ty, id);
                buf_append(cg->pre, &texts[j]);
                buf_puts(cg->pre, ";\n");
                free(texts[j].data);
                memset(&texts[j], 0, sizeof(texts[j]));
                buf_printf(&texts[j], "_ca%d", id);
            }
            buf_append(cg->pre, &pre);
            unsaved = i + 1;
        }
        free(pre.data);
        texts[i] = txt;
        (void)value_cstr;
    }
    for (size_t i = 0; i < n; i++) {
        Expr *arg = e->u.call.args.items[i];
        if (i)
            buf_puts(&*cg->cur, ", ");
        if (extern_cstr && arg->type && arg->type->kind == TY_STR
            && arg->kind != EX_STR) {
            buf_puts(&*cg->cur, "(char *)(");
            buf_append(&*cg->cur, &texts[i]);
            buf_puts(&*cg->cur, ").data");
        } else {
            buf_append(&*cg->cur, &texts[i]);
        }
        free(texts[i].data);
    }
    free(texts);
}

static void emit_call(CG *cg, Expr *e)
{
    Expr *callee = e->u.call.callee;
    if (e->builtin_str_ctor) {
        cg->needs_string_owned = 1;
        if (e->builtin_str_ctor == 3) {
            buf_puts(&*cg->cur, "((zov_type_String){ .zv_data = "
                                "(uint8_t *)(");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "), .zv_len = (uintptr_t)(");
            emit_expr(cg, e->u.call.args.items[1], 0);
            buf_puts(&*cg->cur, "), .zv_cap = (uintptr_t)(");
            emit_expr(cg, e->u.call.args.items[2], 0);
            buf_puts(&*cg->cur, ") })");
            return;
        }
        cg->needs_string_init = 1;
        if (e->builtin_str_ctor == 2) {
            cg->needs_string_new = 1;
            buf_puts(&*cg->cur, "zov_string_new()");
        } else {
            cg->needs_string_from_str = 1;
            cg->needs_string_append = 1;
            buf_puts(&*cg->cur, "zov_string_from_str(");
            emit_str_view(cg, e->u.call.args.items[0]);
            buf_puts(&*cg->cur, ")");
        }
        return;
    }
    if (e->builtin_str_method) {
        Expr *recv = callee->u.field.base;
        cg->needs_string_owned = 1;
        if (e->builtin_str_method == 1) {
            cg->needs_string_init = 1;
            cg->needs_string_free = 1;
            buf_puts(&*cg->cur, "zov_string_free(&(");
            emit_expr(cg, recv, 0);
            buf_puts(&*cg->cur, "))");
        } else if (e->builtin_str_method == 3) {
            cg->needs_string_view = 1;
            buf_puts(&*cg->cur, "((size_t)zov_string_view(");
            emit_expr(cg, recv, 0);
            buf_puts(&*cg->cur, ").len)");
        } else {
            cg->needs_string_view = 1;
            buf_puts(&*cg->cur, "zov_string_view(");
            emit_expr(cg, recv, 0);
            buf_puts(&*cg->cur, ")");
        }
        return;
    }
    if (e->builtin_print) {
        emit_print_call(cg, e, e->builtin_print == 2);
        return;
    }
    if (e->hash_ty) {
        if (e->hash_ty->kind == TY_STR) {
            cg->needs_hash_str = 1;
            buf_puts(&*cg->cur, "zov_hash_str((");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "))");
            return;
        }
        cg->needs_hash = 1;
        buf_puts(&*cg->cur, "zov_hash_u64((uint64_t)(");
        emit_expr(cg, e->u.call.args.items[0], 0);
        buf_puts(&*cg->cur, "))");
        return;
    }
    if (e->str_raw_builtin) {
        buf_puts(&*cg->cur, "(zov_str){ (const uint8_t *)(");
        emit_expr(cg, e->u.call.args.items[0], 0);
        buf_puts(&*cg->cur, "), (size_t)(");
        emit_expr(cg, e->u.call.args.items[1], 0);
        buf_puts(&*cg->cur, ") }");
        return;
    }
    if (e->builtin_env_kind) {
        cg->needs_env = 1;
        switch (e->builtin_env_kind) {
        case 1:
            buf_puts(&*cg->cur, "zov_rt_args_count()");
            return;
        case 2:
            buf_puts(&*cg->cur, "zov_rt_arg_at((size_t)(");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "))");
            return;
        case 3:
            buf_puts(&*cg->cur, "zov_rt_env_has((");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "))");
            return;
        default:
            buf_puts(&*cg->cur, "zov_rt_env_get_str((");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "))");
            return;
        }
    }
    if (e->builtin_assert) {
        emit_assert(cg, e, 0);
        return;
    }
    if (e->szarg) {
        buf_printf(&*cg->cur, "((size_t)sizeof(%s))", c_type(cg, e->szarg));
        return;
    }
    int extern_cstr = (callee->kind == EX_NS_IDENT);
    if (callee->kind == EX_NS_IDENT) {
        /* extern C symbol: unmangled, and str literals pass as raw char* */
        buf_printf(&*cg->cur, "%s(", callee->u.nident.name);
    } else {
        buf_printf(&*cg->cur, "%s(", mangle_fn(callee->u.ident));
    }
    emit_call_args(cg, e, extern_cstr);
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
        if (e->variant_c) {
            buf_puts(b, e->variant_c);
            break;
        }
        buf_puts(b, e->u.nident.name);
        break;
    case EX_ASSIGN: {
        /* C compound forms evaluate the lvalue exactly once */
        const char *cop = NULL;
        switch (e->comp_op) {
        case TK_PLUS_EQ: cop = "+="; break;
        case TK_MINUS_EQ: cop = "-="; break;
        case TK_STAR_EQ: cop = "*="; break;
        case TK_SLASH_EQ: cop = "/="; break;
        case TK_PERCENT_EQ: cop = "%="; break;
        case TK_AMP_EQ: cop = "&="; break;
        case TK_PIPE_EQ: cop = "|="; break;
        case TK_CARET_EQ: cop = "^="; break;
        case TK_SHL_EQ: cop = "<<="; break;
        case TK_SHR_EQ: cop = ">>="; break;
        default: cop = NULL; break;
        }
        if (cop) {
            buf_puts(b, "(");
            emit_expr(cg, e->u.assign.lhs, 0);
            buf_printf(b, " %s ", cop);
            emit_expr(cg, e->u.assign.rhs, 0);
            buf_puts(b, ")");
            break;
        }
        emit_expr(cg, e->u.assign.lhs, 0);
        buf_puts(b, " = ");
        emit_expr(cg, e->u.assign.rhs, 0);
        break;
    }
    case EX_BINARY: {
        const char *op = NULL;
        if (e->type && ty_is_owned_string(e->type)
            && e->u.binary.op == TK_PLUS) {
            /* str/String + T: one owned String, no fixed-size buffer */
            emit_str_value(cg, e);
            break;
        }
        if ((e->u.binary.op == TK_LT || e->u.binary.op == TK_LE
             || e->u.binary.op == TK_GT || e->u.binary.op == TK_GE)
            && e->u.binary.lhs->type && e->u.binary.rhs->type
            && ty_is_stringish(e->u.binary.lhs->type)
            && ty_is_stringish(e->u.binary.rhs->type)) {
            /* lexical ordering via memcmp helper over hoisted views */
            cg->needs_strcmp = 1;
            const char *op = e->u.binary.op == TK_LT ? "<"
                           : e->u.binary.op == TK_LE ? "<="
                           : e->u.binary.op == TK_GT ? ">" : ">=";
            Buf lo = {0};
            Buf ro = {0};
            emit_ordered_operands(cg, e->u.binary.lhs, e->u.binary.rhs, &lo,
                                  &ro);
            buf_puts(b, "(zov_str_cmp((");
            emit_view_text(cg, e->u.binary.lhs, &lo, b);
            buf_puts(b, "), (");
            emit_view_text(cg, e->u.binary.rhs, &ro, b);
            buf_printf(b, ")) %s 0)", op);
            free(lo.data);
            free(ro.data);
            break;
        }
        if ((e->u.binary.op == TK_EQ || e->u.binary.op == TK_NEQ)
            && e->u.binary.lhs->type && e->u.binary.rhs->type
            && ty_is_stringish(e->u.binary.lhs->type)
            && ty_is_stringish(e->u.binary.rhs->type)) {
            /* native string ==/!=: C99 helper over one evaluation per side */
            cg->needs_string = 1;
            cg->needs_string_eq = 1;
            Buf lo = {0};
            Buf ro = {0};
            emit_ordered_operands(cg, e->u.binary.lhs, e->u.binary.rhs, &lo,
                                  &ro);
            buf_printf(b, "%szov_str_eq((",
                       e->u.binary.op == TK_EQ ? "" : "!");
            emit_view_text(cg, e->u.binary.lhs, &lo, b);
            buf_puts(b, "), (");
            emit_view_text(cg, e->u.binary.rhs, &ro, b);
            buf_puts(b, "))");
            free(lo.data);
            free(ro.data);
            break;
        }
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
        case TK_AMP: op = "&"; break;
        case TK_PIPE: op = "|"; break;
        case TK_CARET: op = "^"; break;
        case TK_SHL: op = "<<"; break;
        case TK_SHR: op = ">>"; break;
        default: fatal("unsupported binary operator"); break;
        }
        if ((e->u.binary.op == TK_SLASH || e->u.binary.op == TK_PERCENT)
            && e->type && ty_is_num(e->type->kind)
            && !ty_is_float(e->type->kind)
            && cg->debug && !cg->freestanding) {
            // integer div/mod: debug mode reports the .zov source site
            cg->needs_io = 1;
            cg->needs_panic = 1;
            const char *ct = c_type(cg, e->type);
            int id = cg->tmp_id++;
            buf_printf(&cg->protos,
                       "static %s zov_div_%d(%s a, %s b, "
                       "const char *f, unsigned long l) {\n"
                       "    if (b == 0) { __zov_panic(\"integer "
                       "division by zero\", f, l, 0); }\n"
                       "    return a %s b;\n}\n",
                       ct, id, ct, ct,
                       e->u.binary.op == TK_PERCENT ? "%" : "/");
            Buf lo = {0};
            Buf ro = {0};
            emit_ordered_operands(cg, e->u.binary.lhs, e->u.binary.rhs, &lo,
                                  &ro);
            buf_printf(b, "zov_div_%d(", id);
            buf_append(b, &lo);
            buf_puts(b, ", ");
            buf_append(b, &ro);
            buf_puts(b, ", ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu)", (unsigned long)e->span.line);
            free(lo.data);
            free(ro.data);
            break;
        }
        Buf lo = {0};
        Buf ro = {0};
        int right_hoisted =
            emit_ordered_operands(cg, e->u.binary.lhs, e->u.binary.rhs, &lo,
                                  &ro);
        if (right_hoisted
            && (e->u.binary.op == TK_ANDAND
                || e->u.binary.op == TK_OROR)) {
            fatal_at(e->span,
                     "cannot lower '?'/'orelse' inside a short-circuit "
                     "operand");
        }
        buf_puts(b, "(");
        buf_append(b, &lo);
        buf_printf(b, " %s ", op);
        buf_append(b, &ro);
        buf_puts(b, ")");
        free(lo.data);
        free(ro.data);
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
        else if (e->u.unary.op == TK_TILDE)
            buf_puts(b, "~");
        else
            fatal("unsupported unary operator");
        emit_expr(cg, e->u.unary.e, 0);
        buf_puts(b, ")");
        break;
    case EX_CALL:
        emit_call(cg, e);
        break;
    case EX_ARR_LIT: {
        /* C99 compound literal: elements evaluated left to right */
        Type *et = e->type->base;
        buf_printf(b, "((%s[%lu]){ ", c_type(cg, et),
                   (unsigned long)e->u.elems.len);
        for (size_t i = 0; i < e->u.elems.len; i++) {
            if (i)
                buf_puts(b, ", ");
            emit_expr(cg, e->u.elems.items[i], 0);
        }
        buf_puts(b, " })");
        break;
    }
    case EX_ORELSE: {
        /* coalescing: hoist lhs once; jumps unroll defers before leaving */
        Type *lt = e->u.orelse.lhs->type;
        int id = cg->tmp_id++;
        Buf lhs = {0};
        emit_expr_into(cg, e->u.orelse.lhs, &lhs, 0);
        buf_printf(cg->pre, "%s _or%d = ", c_type(cg, lt), id);
        buf_append(cg->pre, &lhs);
        buf_puts(cg->pre, ";\n");
        free(lhs.data);
        int is_ptr = (lt->kind == TY_PTR);
        buf_printf(cg->pre, "%s _orr%d", c_type(cg, e->type), id);
        buf_puts(cg->pre, ";\n");
        buf_puts(cg->pre, "if (");
        if (is_ptr) {
            buf_printf(cg->pre, "_or%d == NULL", id);
        } else {
            int first = 1;
            for (size_t i = 0; i < lt->members.len; i++) {
                if (!lt->members.items[i]->is_error)
                    continue;
                if (!first)
                    buf_puts(cg->pre, " || ");
                buf_printf(cg->pre, "_or%d.tag == %luu", id,
                           (unsigned long)i);
                first = 0;
            }
        }
        buf_puts(cg->pre, ") {\n");
        if (e->u.orelse.jump) {
            Stmt *j = e->u.orelse.jump;
            if (j->kind == ST_RETURN) {
                emit_defers_to_pre(cg, 0);
                buf_puts(cg->pre, "return ");
                if (j->u.ret) {
                    Buf rv = {0};
                    emit_expr_into(cg, j->u.ret, &rv, 0);
                    buf_append(cg->pre, &rv);
                    free(rv.data);
                }
                buf_puts(cg->pre, ";\n");
            } else if (j->kind == ST_BREAK) {
                emit_defers_to_pre(
                    cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
                if (cg->switch_depth > 0)
                    buf_printf(cg->pre, "goto zov_brk_%d;\n",
                               cg->loop_ids.items[cg->loop_ids.len - 1]);
                else
                    buf_puts(cg->pre, "break;\n");
            } else {
                emit_defers_to_pre(
                    cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
                if (cg->switch_depth > 0)
                    buf_printf(cg->pre, "goto zov_cont_%d;\n",
                               cg->loop_ids.items[cg->loop_ids.len - 1]);
                else
                    buf_puts(cg->pre, "continue;\n");
            }
        } else {
            Buf val = {0};
            emit_expr_into(cg, e->u.orelse.val, &val, 0);
            buf_printf(cg->pre, "_orr%d = ", id);
            buf_append(cg->pre, &val);
            buf_puts(cg->pre, ";\n");
            free(val.data);
        }
        buf_printf(cg->pre, "} else {\n_orr%d = ", id);
        if (is_ptr)
            buf_printf(cg->pre, "_or%d;", id);
        else
            buf_printf(cg->pre, "_or%d.u.v%d;", id, e->coerce_tag);
        buf_puts(cg->pre, "\n}\n");
        buf_printf(b, "_orr%d", id);
        break;
    }
    case EX_SLICE: {
        cg->needs_slice = 1;
        if (!cg->freestanding)
            /* hosted bounds helper reports via fprintf; ring-0 panics */
            cg->needs_io = 1;
        cg->needs_abort = 1;
        Type *bt = e->u.slice.base->type;
        int id = cg->tmp_id++;
        if (ty_is_stringish(bt)) {
            /* str/String sub-slice: borrow the underlying bytes */
            buf_printf(&cg->protos,
                       "static zov_str zov_strsub_%d(zov_str s, size_t a, "
                       "size_t b, const char *f, unsigned long l) {\n"
                       "    if (b == SIZE_MAX) b = s.len;\n"
                       "    zov_slice_check(a, b, s.len, f, l);\n"
                       "    return (zov_str){ s.data + a, b - a };\n}\n",
                       id);
            buf_printf(b, "zov_strsub_%d((", id);
            emit_str_view(cg, e->u.slice.base);
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_puts(b, "SIZE_MAX");
            buf_puts(b, "), ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu)", (unsigned long)e->span.line);
            break;
        }
        if (bt->kind == TY_SLICE) {
            const char *st = c_type(cg, e->type);
            buf_printf(&cg->protos,
                       "static %s zov_subslice_%d(%s s, size_t a, size_t b, "
                       "const char *f, unsigned long l) {\n"
                       "    if (b == SIZE_MAX) b = s.len;\n"
                       "    zov_slice_check(a, b, s.len, f, l);\n"
                       "    return (%s){ s.data + a, b - a };\n}\n",
                       st, id, st, st);
            buf_printf(b, "zov_subslice_%d((", id);
            emit_expr(cg, e->u.slice.base, 0);
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_puts(b, "SIZE_MAX");
            buf_puts(b, "), ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu)", (unsigned long)e->span.line);
            break;
        }
        if (bt->kind == TY_ARRAY) {
            const char *st = c_type(cg, e->type);
            const char *et = c_type(cg, bt->base);
            buf_printf(&cg->protos,
                       "static %s zov_arrslice_%d(%s *p, size_t n, "
                       "size_t a, size_t b, const char *f, unsigned long l) {\n"
                       "    if (b == SIZE_MAX) b = n;\n"
                       "    zov_slice_check(a, b, n, f, l);\n"
                       "    return (%s){ p + a, b - a };\n}\n",
                       st, id, et, st);
            buf_printf(b, "zov_arrslice_%d((", id);
            emit_expr(cg, e->u.slice.base, 0);
            buf_printf(b, "), %luu, (size_t)(", (unsigned long)bt->count);
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_puts(b, "SIZE_MAX");
            buf_puts(b, "), ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu)", (unsigned long)e->span.line);
            break;
        }
        if (bt->kind == TY_PTR) {
            const char *st = c_type(cg, e->type);
            const char *et = c_type(cg, bt->base);
            buf_printf(&cg->protos,
                       "static %s zov_ptrslice_%d(%s *p, size_t a, "
                       "size_t b) {\n"
                       "    return (%s){ p + a, b - a };\n}\n",
                       st, id, et, st);
            buf_printf(b, "zov_ptrslice_%d((", id);
            emit_expr(cg, e->u.slice.base, 0);
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, "))");
            break;
        }
        fatal("internal: unprintable slice base");
        break;
    }
    case EX_INDEX: {
        /* sizeof operand is unevaluated, so duplicating the base expr
         * for the length computation is side-effect free */
        Type *bt = e->u.index.base->type;
        if (bt && bt->kind == TY_PTR) {
            /* raw pointer indexing: unchecked, emitted directly */
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, "[");
            emit_expr(cg, e->u.index.index, 0);
            buf_puts(b, "]");
            break;
        }
        if (bt && bt->kind == TY_ARRAY) {
            cg->needs_bounds = 1;
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, "[zov_bounds((size_t)(");
            emit_expr(cg, e->u.index.index, 0);
            buf_puts(b, "), sizeof(");
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ") / sizeof((");
            emit_expr(cg, e->u.index.base, 0);
            buf_puts(b, ")[0]))]");
        } else if (ty_is_owned_string(bt)) {
            cg->needs_string = 1;
            cg->needs_str_index = 1;
            cg->needs_bounds = 1;
            buf_puts(b, "zov_str_byte((");
            emit_str_view(cg, e->u.index.base);
            buf_puts(b, "), (size_t)(");
            emit_expr(cg, e->u.index.index, 0);
            buf_puts(b, "))");
        } else {
            cg->needs_bounds = 1;
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
        fatal("generic instantiation requires a call or struct literal");
        break;
    case EX_DEREF:
        buf_puts(b, "(*");
        emit_expr(cg, e->u.deref, 0);
        buf_puts(b, ")");
        break;
    case EX_FIELD: {
        Type *fbt = e->u.field.base->type;
        if (fbt && (fbt->kind == TY_STR || fbt->kind == TY_SLICE)) {
            /* fat-pointer halves: .ptr maps to the C data member */
            buf_puts(b, "(");
            emit_expr(cg, e->u.field.base, 0);
            if (zstr_eq(e->u.field.name, "ptr"))
                buf_puts(b, ").data");
            else
                buf_printf(b, ").%s", e->u.field.name);
            break;
        }
        if (fbt && fbt->kind == TY_ARRAY
            && zstr_eq(e->u.field.name, "len")) {
            /* compile-time element count; sizeof never evaluates */
            buf_puts(b, "(sizeof(");
            emit_expr(cg, e->u.field.base, 0);
            buf_puts(b, ") / sizeof((");
            emit_expr(cg, e->u.field.base, 0);
            buf_puts(b, ")[0]))");
            break;
        }
        if (e->u.field.base->type && e->u.field.base->type->kind == TY_STR) {
            /* str fat-pointer halves: raw C member names */
            buf_puts(b, "(");
            emit_expr(cg, e->u.field.base, 0);
            buf_printf(b, ").%s", e->u.field.name);
            break;
        }
        buf_puts(b, "(");
        emit_expr(cg, e->u.field.base, 0);
        buf_printf(b, ")%s%s", e->via_ref ? "->" : ".",
                   mangle_local(e->u.field.name));
        break;
    }
    case EX_STRUCT_LIT:
        buf_printf(b, "((%s){ ", c_type(cg, e->type));
        for (size_t i = 0; i < e->u.stlit.fields.len; i++) {
            if (i)
                buf_puts(b, ", ");
            buf_printf(b, ".%s = ",
                       mangle_local(e->u.stlit.fields.items[i].name));
            emit_expr(cg, e->u.stlit.fields.items[i].val, 0);
        }
        buf_puts(b, " })");
        break;
    case EX_MATCH:
        emit_match(cg, e, 1);
        break;
    case EX_TRY: {
        Type *t = e->u.tryy->type;
        int id = cg->tmp_id++;
        Buf val = {0};
        emit_expr_into(cg, e->u.tryy, &val, 0);
        if (t->kind == TY_UNION) {
            const char *un = union_name(cg, t);
            buf_printf(cg->pre, "%s _q%d = ", un, id);
            buf_append(cg->pre, &val);
            buf_puts(cg->pre, ";\n");
            for (size_t i = 0; i < t->members.len; i++) {
                Type *m = t->members.items[i];
                if (!m->is_error)
                    continue;
                buf_printf(cg->pre, "if (_q%d.tag == %luu) {\n", id,
                           (unsigned long)i);
                // reuse the lexical defer stack: errors unroll LIFO
                emit_defers_to_pre(cg, 0);
                Type *rt = cg->fn_ret;
                int ridx = rt ? cg_union_idx(rt, m) : -1;
                buf_printf(cg->pre, "return ((%s){ .tag = %du",
                           union_name(cg, rt), ridx);
                if (member_has_payload(m))
                    buf_printf(cg->pre, ", .u.v%lu = _q%d.u.v%lu",
                               (unsigned long)ridx, id, (unsigned long)i);
                buf_puts(cg->pre, " });\n}\n");
            }
            Type *ok = union_ok_member(t);
            if (member_has_payload(ok))
                buf_printf(b, "_q%d.u.v%d", id,
                           cg_union_idx(t, ok));
        } else {
            buf_printf(cg->pre, "%s *_q%d = ", c_type(cg, t->base), id);
            buf_append(cg->pre, &val);
            buf_puts(cg->pre, ";\n");
            buf_printf(cg->pre, "if (_q%d == NULL) {\n", id);
            emit_defers_to_pre(cg, 0);
            buf_puts(cg->pre, "return NULL;\n}\n");
            buf_printf(b, "_q%d", id);
        }
        free(val.data);
        break;
    }
    case EX_UNWRAP: {
        cg->needs_abort = 1;
        Type *t = e->u.unwrap->type;
        int id = cg->tmp_id++;
        if (t->kind == TY_UNION) {
            const char *un = union_name(cg, t);
            int ok = cg_union_idx(t, union_ok_member(t));
            buf_printf(&cg->protos,
                       "static %s zov_unwrapu_%d(%s u, const char *f, "
                       "int l) {\n"
                       "    (void)f; (void)l;\n"
                       "    if (u.tag != %du) {\n",
                       un, id, un, ok);
            if (cg->freestanding)
                buf_puts(&cg->protos,
                         "        zov_panic(\"unwrap of error value\", f, l);\n");
            else {
                cg->needs_abort = 1;
                buf_puts(&cg->protos, "        abort();\n");
            }
            buf_puts(&cg->protos, "    }\n    return u;\n}\n");
            buf_printf(b, "zov_unwrapu_%d((", id);
            emit_expr(cg, e->u.unwrap, 0);
            buf_puts(b, "), ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %lu).u.v%d", (unsigned long)e->span.line, ok);
        } else {
            const char *pt = c_type(cg, t->base);
            buf_printf(&cg->protos,
                       "static void *zov_unwrapp_%d(void *p, const char *f, "
                       "int l) {\n"
                       "    (void)f; (void)l;\n"
                       "    if (p == NULL) {\n", id);
            if (cg->freestanding)
                buf_puts(&cg->protos,
                         "        zov_panic(\"unwrap of null\", f, l);\n");
            else if (cg->debug) {
                cg->needs_io = 1;
                cg->needs_panic = 1;
                buf_puts(&cg->protos,
                         "        __zov_panic(\"unwrap of null\", f, l, 0);\n");
            } else {
                cg->needs_abort = 1;
                buf_puts(&cg->protos, "        abort();\n");
            }
            buf_puts(&cg->protos, "    }\n    return p;\n}\n");
            buf_printf(b, "((%s *)zov_unwrapp_%d((void *)(", pt, id);
            emit_expr(cg, e->u.unwrap, 0);
            buf_puts(b, "), ");
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %lu))", (unsigned long)e->span.line);
        }
        break;
    }
    case EX_COERCE: {
        Type *u = e->u.coerce.target;
        Type *m = u->members.items[e->coerce_tag];
        buf_printf(b, "((%s){ .tag = %du", union_name(cg, u),
                   e->coerce_tag);
        if (e->u.coerce.e && member_has_payload(m)) {
            buf_printf(b, ", .u.v%d = ", e->coerce_tag);
            emit_expr(cg, e->u.coerce.e, 0);
        }
        buf_printf(b, " })");
        break;
    }
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

/* Every statement gets its own prelude buffer.  Expressions that need
 * declarations or conditional returns append there; the prelude is emitted
 * before the statement text, which keeps generated C free of GNU braced
 * groups while preserving left-to-right evaluation. */
static int emit_stmt(CG *cg, Stmt *s)
{
    Buf stmt = {0};
    Buf pre = {0};
    Buf *saved_cur = cg->cur;
    Buf *saved_pre = cg->pre;
    cg->cur = &stmt;
    cg->pre = &pre;
    int r = emit_stmt_body(cg, s);
    cg->cur = saved_cur;
    cg->pre = saved_pre;
    buf_append(saved_cur, &pre);
    buf_append(saved_cur, &stmt);
    free(pre.data);
    free(stmt.data);
    return r;
}

static int emit_stmt_body(CG *cg, Stmt *s)
{
    switch (s->kind) {
    case ST_LET: {
        emit_var(&*cg->cur, cg, s->u.let.ty, mangle_local(s->u.let.name));
        if (!s->u.let.init) {
            /* init-less let: C99 {0} zero-fills every member/element */
            buf_puts(&*cg->cur, " = {0};\n");
            return 0;
        }
        if (s->u.let.init->kind == EX_ARR_LIT && s->u.let.ty
            && s->u.let.ty->kind == TY_ARRAY) {
            /* arrays take initializer lists, not compound literals */
            buf_puts(&*cg->cur, " = { ");
            for (size_t i = 0; i < s->u.let.init->u.elems.len; i++) {
                if (i)
                    buf_puts(&*cg->cur, ", ");
                emit_expr(cg, s->u.let.init->u.elems.items[i], 0);
            }
            buf_puts(&*cg->cur, " };\n");
            return 0;
        }
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
    case ST_FOR_RANGE: {
        // half-open range; the bound is evaluated exactly once
        int id = cg->tmp_id++;
        char ub[24];
        snprintf(ub, sizeof(ub), "_zv_fub%d", id);
        buf_puts(&*cg->cur, "{\n");
        emit_var(&*cg->cur, cg, s->u.forr.ty, ub);
        buf_puts(&*cg->cur, " = ");
        emit_expr(cg, s->u.forr.end, 0);
        buf_printf(&*cg->cur, ";\n");
        buf_puts(&*cg->cur, "for (");
        emit_var(&*cg->cur, cg, s->u.forr.ty, mangle_local(s->u.forr.name));
        buf_puts(&*cg->cur, " = ");
        emit_expr(cg, s->u.forr.start, 0);
        buf_printf(&*cg->cur, "; %s < %s; %s = %s + 1) {\n",
                   mangle_local(s->u.forr.name), ub,
                   mangle_local(s->u.forr.name), mangle_local(s->u.forr.name));
        da_push(&cg->loop_marks, cg->defers.len);
        da_push(&cg->loop_ids, id);
        emit_block(cg, s->u.forr.body);
        cg->loop_marks.len--;
        cg->loop_ids.len--;
        buf_printf(&*cg->cur, "zov_cont_%d: ; if (0) goto zov_cont_%d;\n}\n",
                   id, id);
        buf_printf(&*cg->cur, "zov_brk_%d: ; if (0) goto zov_brk_%d;\n",
                   id, id);
        buf_puts(&*cg->cur, "}\n");
        return 0;
    }
    case ST_FOR_EACH: {
        Type *it = s->u.foreach.iter->type;
        int id = cg->tmp_id++;
        Buf *cur = &*cg->cur;
        buf_puts(cur, "{\n");
        if (it->kind == TY_ARRAY) {
            /* arrays decay once: pointer + compile-time count */
            emit_var(cur, cg, type_ptr(it->base, 0), "_zv_itp");
            buf_puts(cur, " = (");
            emit_expr(cg, s->u.foreach.iter, 0);
            buf_printf(cur, ");\nfor (size_t _i%d = 0; _i%d < %luu; "
                             "_i%d++) {\n", id, id,
                       (unsigned long)it->count, id);
        } else if (ty_is_owned_string(it)) {
            buf_puts(cur, "zov_str _zv_it = zov_string_view(");
            emit_expr(cg, s->u.foreach.iter, 0);
            buf_printf(cur, ");\nfor (size_t _i%d = 0; _i%d < "
                             "(size_t)_zv_it.len; _i%d++) {\n", id, id, id);
        } else {
            emit_var(cur, cg, it, "_zv_it");
            buf_puts(cur, " = (");
            emit_expr(cg, s->u.foreach.iter, 0);
            buf_printf(cur, ");\nfor (size_t _i%d = 0; _i%d < "
                             "(size_t)_zv_it.len; _i%d++) {\n",
                       id, id, id);
        }
        buf_puts(cur, "size_t zov_cont_guard = 0; (void)zov_cont_guard;\n");
        emit_var(cur, cg, s->u.foreach.ty, mangle_local(s->u.foreach.name));
        if (it->kind == TY_ARRAY)
            buf_printf(cur, " = _zv_itp[_i%d];\n", id);
        else
            buf_printf(cur, " = _zv_it.data[_i%d];\n", id);
        da_push(&cg->loop_marks, cg->defers.len);
        da_push(&cg->loop_ids, id);
        emit_block(cg, s->u.foreach.body);
        cg->loop_marks.len--;
        cg->loop_ids.len--;
        buf_printf(cur, "zov_cont_%d: ; if (0) goto zov_cont_%d;\n}\n",
                   id, id);
        buf_printf(cur, "zov_brk_%d: ; if (0) goto zov_brk_%d;\n}\n",
                   id, id);
        return 0;
    }
    case ST_WHILE: {
        Buf cond = {0};
        Buf cond_pre = {0};
        emit_expr_capture(cg, s->u.whilee.cond, &cond, &cond_pre, 0);
        int wrapped = cond_pre.len > 0;
        if (wrapped) {
            /* a hoisted condition prelude must re-run on every iteration */
            buf_puts(&*cg->cur, "for (;;) {\n");
            buf_append(&*cg->cur, &cond_pre);
            buf_puts(&*cg->cur, "if (!(");
            buf_append(&*cg->cur, &cond);
            buf_puts(&*cg->cur, ")) break;\n");
        } else {
            buf_puts(&*cg->cur, "while (");
            buf_append(&*cg->cur, &cond);
            buf_puts(&*cg->cur, ") {\n");
        }
        free(cond.data);
        free(cond_pre.data);
        int id = cg->tmp_id++;
        da_push(&cg->loop_marks, cg->defers.len);
        da_push(&cg->loop_ids, id);
        emit_block(cg, s->u.whilee.body);
        cg->loop_marks.len--;
        cg->loop_ids.len--;
        // labeled exits let break/continue jump out of lowered switches
        buf_printf(&*cg->cur, "zov_cont_%d: ; if (0) goto zov_cont_%d;\n",
                   id, id);
        buf_puts(&*cg->cur, "}\n");
        buf_printf(&*cg->cur, "zov_brk_%d: ; if (0) goto zov_brk_%d;\n",
                   id, id);
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
        if (cg->switch_depth > 0) {
            // plain break would only exit the lowered match switch
            buf_printf(&*cg->cur, "goto zov_brk_%d;\n",
                       cg->loop_ids.items[cg->loop_ids.len - 1]);
        } else {
            buf_puts(&*cg->cur, "break;\n");
        }
        cg->terminated = 1;
        return 1;
    }
    case ST_CONTINUE: {
        emit_defers_down_to(cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
        if (cg->switch_depth > 0)
            buf_printf(&*cg->cur, "goto zov_cont_%d;\n",
                       cg->loop_ids.items[cg->loop_ids.len - 1]);
        else
            buf_puts(&*cg->cur, "continue;\n");
        cg->terminated = 1;
        return 1;
    }
    case ST_EXPR:
        if (s->u.expr->kind == EX_CALL && s->u.expr->builtin_assert) {
            emit_assert(cg, s->u.expr, 1);
            return 0;
        }
        if (s->u.expr->kind == EX_CALL && s->u.expr->builtin_print) {
            emit_print_call(cg, s->u.expr, s->u.expr->builtin_print == 2);
            buf_puts(&*cg->cur, ";\n");
            return 0;
        }
        if (s->u.expr->kind == EX_MATCH) {
            // statement-position match: switch lowering without result
            emit_match(cg, s->u.expr, 0);
            return 0;
        }
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
    /* freestanding: main emits with its declared signature only */
    int is_main = zstr_eq(d->u.fn.name, "main") && !d->u.fn.recv
                  && !cg->freestanding;
    if (proto_only) {
        cg->cur = &cg->protos;
        emit_var(&*cg->cur, cg, d->u.fn.ret,
                 mangle_fn(d->regname ? d->regname : d->u.fn.name));
        if (is_main)
            buf_puts(&*cg->cur, "(int argc, char **argv);\n");
        else {
            buf_puts(&*cg->cur, "(");
            emit_params(cg, d);
            buf_puts(&*cg->cur, ");\n");
        }
        return;
    }

    cg->cur = &cg->body;
    emit_var(&*cg->cur, cg, d->u.fn.ret,
             mangle_fn(d->regname ? d->regname : d->u.fn.name));
    if (is_main)
        buf_puts(&*cg->cur, "(int argc, char **argv)\n{\n"
                            "zov_rt_argc = argc; zov_rt_argv = argv;\n");
    else {
        buf_puts(&*cg->cur, "(");
        emit_params(cg, d);
        buf_puts(&*cg->cur, ")\n{\n");
    }
    cg->defers.len = 0;
    cg->fn_ret = d->u.fn.ret;
    cg->switch_depth = 0;
    int term = emit_block(cg, d->u.fn.body);
    cg->fn_ret = NULL;
    if (d->u.fn.ret->kind != TY_VOID && !term)
        buf_puts(&*cg->cur, "return 0;\n");
    buf_puts(&*cg->cur, "}\n\n");
}

/* --test synthesis: one static fn per block plus a counting runner */
static void emit_test_main(CG *cg, Program *prog)
{
    int n = 0;
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_TEST)
            continue;
        cg->cur = &cg->body;
        buf_printf(&cg->body, "static void zov_test_%d(void)\n{\n", n);
        cg->defers.len = 0;
        emit_block(cg, d->u.test.body);
        buf_puts(&cg->body, "}\n\n");
        n++;
    }
    cg->cur = &cg->body;
    buf_puts(&cg->body, "int main(int argc, char **argv)\n{\n"
                        "zov_rt_argc = argc; zov_rt_argv = argv;\n"
                        "int _zv_passed = 0;\n");
    n = 0;
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_TEST)
            continue;
        buf_printf(&cg->body, "zov_test_%d(); ", n);
        buf_puts(&cg->body, "fprintf(stdout, \"[PASS] ");
        append_fmt_str(&cg->body, d->u.test.desc, strlen(d->u.test.desc));
        buf_puts(&cg->body, "\\n\"); _zv_passed = _zv_passed + 1;\n");
        n++;
    }
    buf_printf(&cg->body,
               "fprintf(stdout, \"%%d passed, 0 failed\\n\", "
               "_zv_passed);\nreturn 0;\n}\n\n");
}

/* Does a by-value field mention this struct?  Pointers only need the
 * forward typedef, so they do not impose definition ordering. */
static int type_mentions_struct(Type *t, const char *name)
{
    if (!t)
        return 0;
    switch (t->kind) {
    case TY_STRUCT:
        return t->name == name;
    case TY_ARRAY:
        return type_mentions_struct(t->base, name);
    default:
        return 0;
    }
}

static int decl_is_plain_struct(Decl *d)
{
    return d->kind == DL_TYPE && d->u.type.tparams.len == 0
           && !d->u.type.alias_ty && !d->u.type.is_enum;
}

static void emit_struct_definition(CG *cg, Decl *d)
{
    cg->cur = &cg->head;
    buf_printf(&*cg->cur, "struct %s {\n", mangle_type(d->u.type.name));
    for (size_t j = 0; j < d->u.type.fields.len; j++) {
        Field *f = &d->u.type.fields.items[j];
        buf_puts(&*cg->cur, "    ");
        emit_var(&*cg->cur, cg, f->ty, mangle_local(f->name));
        buf_puts(&*cg->cur, ";\n");
    }
    buf_printf(&*cg->cur, "};\n");
}

/* Emit struct definitions after the structs their by-value fields require. */
static void emit_type_deps(CG *cg, Program *prog, size_t idx,
                           unsigned char *state)
{
    if (state[idx] != 0)
        return; /* done, or a by-value cycle C will reject later */
    state[idx] = 1;
    Decl *d = prog->decls.items[idx];
    for (size_t j = 0; j < prog->decls.len; j++) {
        Decl *dep = prog->decls.items[j];
        if (!decl_is_plain_struct(dep) || j == idx)
            continue;
        for (size_t k = 0; k < d->u.type.fields.len; k++)
            if (type_mentions_struct(d->u.type.fields.items[k].ty,
                                     dep->u.type.name)) {
                emit_type_deps(cg, prog, j, state);
                break;
            }
    }
    state[idx] = 2;
    emit_struct_definition(cg, d);
}

char *codegen_generate(Program *prog)
{
    return codegen_generate_mode(prog, 0);
}

char *codegen_generate_mode(Program *prog, int test_mode)
{
    CG *cg = zalloc(sizeof(CG));
    cg->freestanding = prog->freestanding;
    cg->debug = prog->debug;

    cg->cur = &cg->head;
    cg->pre = &cg->prebuf;
    if (!cg->freestanding)
        /* hosted std::time/fs use POSIX symbols that strict C99 hides */
        buf_puts(&*cg->cur,
                 "#define _POSIX_C_SOURCE 200809L\n"
                 "#define _DEFAULT_SOURCE 1\n");
    buf_puts(&*cg->cur,
             "#include <stdint.h>\n"
             "#include <stddef.h>\n"
             "#include <stdbool.h>\n");
    if (!cg->freestanding) {
        /* argv capture exists only for hosted std::env */
        buf_puts(&*cg->cur,
                 "static int zov_rt_argc; static char **zov_rt_argv;\n");
    } else {
        /* declared before every helper; kernels own the definition */
        buf_puts(&*cg->cur,
                 "extern void zov_panic(const char *msg, "
                 "const char *file, int line);\n");
    }
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_USE && d->u.use.is_c_header)
            buf_printf(&*cg->cur, "#include <%s>\n", d->u.use.path);
    }
    buf_puts(&*cg->cur,
             "typedef struct { const uint8_t *data; size_t len; } zov_str;\n");

    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_TYPE || d->u.type.tparams.len > 0)
            continue;
        if (d->u.type.is_enum) {
            cg->cur = &cg->head;
            buf_printf(&*cg->cur, "typedef enum { ");
            for (size_t j = 0; j < d->u.type.variants.len; j++) {
                EnumVariant *v = &d->u.type.variants.items[j];
                buf_printf(&*cg->cur, "Zov_%s_%s = %lld%s ",
                           d->u.type.name, v->name, v->value,
                           j + 1 < d->u.type.variants.len ? "," : "");
            }
            buf_printf(&*cg->cur, "} %s;\n",
                       mangle_type(d->u.type.name));
            continue;
        }
        /* structs are emitted below in dependency order */
    }
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (!decl_is_plain_struct(d))
            continue;
        cg->cur = &cg->head;
        buf_printf(&*cg->cur, "typedef struct %s %s;\n",
                   mangle_type(d->u.type.name),
                   mangle_type(d->u.type.name));
    }
    unsigned char *type_state = calloc(prog->decls.len, 1);
    if (!type_state)
        fatal("out of memory");
    for (size_t i = 0; i < prog->decls.len; i++)
        if (decl_is_plain_struct(prog->decls.items[i]))
            emit_type_deps(cg, prog, i, type_state);
    free(type_state);

    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_FN || d->u.fn.tparams.len > 0)
            continue;
        if (test_mode && zstr_eq(d->u.fn.name, "main"))
            continue; /* --test replaces the entry point */
        emit_fn(cg, d, 1);
    }
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_FN || d->u.fn.tparams.len > 0)
            continue;
        if (test_mode && zstr_eq(d->u.fn.name, "main"))
            continue;
        emit_fn(cg, d, 0);
    }
    if (test_mode)
        emit_test_main(cg, prog);

    /* assemble final buffer: head + slice typedefs + body */
    Buf out = {0};
    buf_puts(&out, cg->head.data);
    /* variant-name tables, only for enums that are actually stringified */
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind != DL_TYPE || !d->u.type.is_enum)
            continue;
        int used = 0;
        for (size_t k = 0; k < cg->enum_names.len; k++)
            if (cg->enum_names.items[k] == d->u.type.name)
                used = 1;
        if (!used)
            continue;
        buf_printf(&out, "static const char *zov_enum_%s_name("
                         "%s v) { switch (v) { ",
                   d->u.type.name, mangle_type(d->u.type.name));
        for (size_t j = 0; j < d->u.type.variants.len; j++) {
            EnumVariant *v = &d->u.type.variants.items[j];
            buf_printf(&out, "case Zov_%s_%s: return \"", d->u.type.name,
                       v->name);
            append_fmt_str(&out, v->name, strlen(v->name));
            buf_puts(&out, "\"; ");
        }
        buf_puts(&out, "default: return \"<invalid>\"; } }\n");
    }
    if (!cg->freestanding && cg->debug
        && (cg->needs_panic || cg->needs_bounds || cg->needs_slice)) {
        buf_puts(&out,
                 "#include <stdio.h>\n#include <stdlib.h>\n"
                 "static void __zov_panic(const char *msg, "
                 "const char *file, int line, int col) {\n"
                 "    fprintf(stderr, \"thread 'main' panicked at "
                 "'%s'\\n  --> %s:%d:%d\\n\", msg, file, line, col);\n"
                 "    exit(101);\n"
                 "}\n");
    }
    if (cg->needs_bounds) {
        if (cg->freestanding) {
            cg->needs_abort = 1;
            buf_puts(&out,
                     "static inline size_t zov_bounds(size_t i, "
                     "size_t n) {\n"
                     "    if (i >= n) {\n"
                     "        zov_panic(\"index out of bounds\", \"?\", "
                     "0);\n"
                     "    }\n"
                     "    return i;\n"
                     "}\n");
        } else if (cg->debug) {
            buf_puts(&out, "#include <stdlib.h>\n"
                           "static size_t zov_bounds(size_t i, size_t n) {\n"
                           "    if (i >= n) {\n"
                           "        __zov_panic(\"index out of bounds\", "
                           "\"?\", 0, 0);\n"
                           "    }\n"
                           "    return i;\n"
                           "}\n");
        } else {
            buf_puts(&out, "#include <stdlib.h>\n"
                           "static size_t zov_bounds(size_t i, size_t n) {\n"
                           "    if (i >= n) {\n"
                           "        abort();\n"
                           "    }\n"
                           "    return i;\n"
                           "}\n");
        }
    }
    for (size_t i = 0; i < cg->slices.len; i++) {
        Type *s = cg->slices.items[i];
        buf_puts(&out, "typedef struct { ");
        emit_var(&out, cg, s->base, "*data");
        buf_puts(&out, "; size_t len; } zov_slice_");
        buf_printf(&out, "%lu", (unsigned long)i);
        buf_puts(&out, ";\n");
    }
    if (cg->freestanding) {
        if (cg->needs_slice)
            buf_puts(&out,
                     "static inline size_t zov_slice_check(size_t a, "
                     "size_t b, size_t n, const char *f, int l) {\n"
                     "    if (a > b || b > n) {\n"
                     "        zov_panic(\"slice out of bounds\", f, l);\n"
                     "    }\n"
                     "    return a;\n"
                     "}\n");
        if (cg->needs_string || cg->needs_string_eq || cg->needs_strcmp
            || cg->needs_slice)
            /* memcmp stays a kernel-provided primitive at link time */
            buf_puts(&out, "#include <string.h>\n");
        if (cg->needs_string_eq)
            buf_puts(&out,
                     "static int zov_str_eq(zov_str a, zov_str b) {\n"
                     "    return a.len == b.len\n"
                     "        && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n"
                     "}\n");
        if (cg->needs_str_index)
            buf_puts(&out,
                     "static uint8_t zov_str_byte(zov_str s, size_t i) {\n"
                     "    return s.data[zov_bounds(i, s.len)];\n"
                     "}\n");
    } else {
        if (cg->needs_abort)
            buf_puts(&out, "#include <stdlib.h>\n");
        if (cg->needs_slice) {
            if (cg->debug) {
            buf_puts(&out, "#include <string.h>\n");
            buf_puts(&out,
                     "static size_t zov_slice_check(size_t a, size_t b, "
                     "size_t n, const char *f, unsigned long l) {\n"
                     "    if (a > b || b > n) {\n"
                     "        __zov_panic(\"slice out of bounds\", f, "
                     "(int)l, 0);\n"
                     "    }\n"
                     "    return a;\n"
                     "}\n");
            } else {
            buf_puts(&out, "#include <string.h>\n");
            buf_puts(&out,
                     "static size_t zov_slice_check(size_t a, size_t b, "
                     "size_t n, const char *f, unsigned long l) {\n"
                     "    if (a > b || b > n) {\n"
                     "        abort();\n"
                     "    }\n"
                     "    return a;\n"
                     "}\n");
            }
        }
        if (cg->needs_string_eq || cg->needs_string) {
            buf_puts(&out, "#include <string.h>\n");
        }
        if (cg->needs_string_eq) {
            buf_puts(&out,
                     "static int zov_str_eq(zov_str a, zov_str b) {\n"
                     "    return a.len == b.len\n"
                     "        && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);\n"
                     "}\n");
        }
        if (cg->needs_str_index)
            buf_puts(&out,
                     "static uint8_t zov_str_byte(zov_str s, size_t i) {\n"
                     "    return s.data[zov_bounds(i, s.len)];\n"
                     "}\n");
    }
    if (cg->needs_strcmp) {
        buf_puts(&out, "#include <string.h>\n");
        buf_puts(&out,
                 "static int zov_str_cmp(zov_str a, zov_str b) {\n"
                 "    size_t n = a.len < b.len ? a.len : b.len;\n"
                 "    int r = memcmp(a.data, b.data, n);\n"
                 "    if (r != 0) return r;\n"
                 "    if (a.len < b.len) return -1;\n"
                 "    if (a.len > b.len) return 1;\n"
                 "    return 0;\n"
                 "}\n");
    }
    if (cg->needs_hash_str) {
        /* FNV-1a over bytes */
        buf_puts(&out,
                 "static uint64_t zov_hash_str(zov_str s) {\n"
                 "    uint64_t h = 1469598103934665603ULL;\n"
                 "    for (size_t i = 0; i < s.len; i++) {\n"
                 "        h ^= s.data[i]; h *= 1099511628211ULL;\n"
                 "    }\n"
                 "    return h;\n"
                 "}\n");
    }
    if (cg->needs_hash) {
        /* splitmix64 finalizer for scalar keys */
        buf_puts(&out,
                 "static uint64_t zov_hash_u64(uint64_t x) {\n"
                 "    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;\n"
                 "    x ^= x >> 27; x *= 0x94d049bb133111ebULL;\n"
                 "    x ^= x >> 31; return x;\n"
                 "}\n");
    }
    if (cg->needs_env) {
        buf_puts(&out, "#include <stdio.h>\n#include <stdlib.h>\n"
                       "#include <string.h>\n");
        buf_puts(&out,
                 "static size_t zov_rt_args_count(void) { "
                 "return (size_t)zov_rt_argc; }\n"
                 "static zov_str zov_rt_arg_at(size_t i) {\n"
                 "    if (i >= (size_t)zov_rt_argc) {\n"
                 "        fprintf(stderr, \"zov: arg index out of "
                 "bounds\\n\"); abort();\n"
                 "    }\n"
                 "    zov_str s; s.data = (const uint8_t *)zov_rt_argv[i];\n"
                 "    s.len = strlen(zov_rt_argv[i]); return s;\n"
                 "}\n"
                 "static int zov_rt_env_has(zov_str name) {\n"
                 "    char buf[4096];\n"
                 "    size_t n = name.len < sizeof buf - 1 ? name.len\n"
                 "                                            : "
                 "sizeof buf - 1;\n"
                 "    memcpy(buf, name.data, n); buf[n] = 0;\n"
                 "    return getenv(buf) != NULL;\n"
                 "}\n"
                 "static zov_str zov_rt_env_get_str(zov_str name) {\n"
                 "    char buf[4096];\n"
                 "    size_t n = name.len < sizeof buf - 1 ? name.len\n"
                 "                                            : "
                 "sizeof buf - 1;\n"
                 "    memcpy(buf, name.data, n); buf[n] = 0;\n"
                 "    const char *v = getenv(buf);\n"
                 "    zov_str s; s.data = (const uint8_t *)v;\n"
                 "    s.len = v ? strlen(v) : 0; return s;\n"
                 "}\n");
    }
    if (cg->needs_string_owned) {
        if (cg->freestanding) {
            buf_puts(&out,
                     "extern void *zov_rt_realloc(void *p, size_t n);\n"
                     "extern void zov_rt_free(void *p);\n");
        } else {
            buf_puts(&out, "#include <stdlib.h>\n");
        }
        if (cg->needs_string_append) {
            if (cg->freestanding) {
                buf_puts(&out,
                         "static void zov_string_oom(void) {\n"
                         "    zov_panic(\"string allocation failed\", "
                         "\"?\", 0);\n"
                         "    for (;;) { }\n"
                         "}\n");
            } else {
                buf_puts(&out,
                         "#include <string.h>\n#include <stdio.h>\n"
                         "static void zov_string_oom(void) {\n"
                         "    fprintf(stderr, \"zov: out of memory\\n\");\n"
                         "    abort();\n"
                         "}\n");
            }
        }
        if (cg->needs_string_init)
            buf_puts(&out,
                     "static void zov_string_init(zov_type_String *s) {\n"
                     "    s->zv_data = 0; s->zv_len = 0; s->zv_cap = 0;\n"
                     "}\n");
        if (cg->needs_string_free) {
            buf_puts(&out,
                     "static void zov_string_free(zov_type_String *s) {\n"
                     "    if (s->zv_data) {\n");
            if (cg->freestanding)
                buf_puts(&out, "        zov_rt_free(s->zv_data);\n");
            else
                buf_puts(&out, "        free(s->zv_data);\n");
            buf_puts(&out,
                     "    }\n"
                     "    zov_string_init(s);\n"
                     "}\n");
        }
        if (cg->needs_string_view)
            buf_puts(&out,
                     "static zov_str zov_string_view(zov_type_String s) {\n"
                     "    zov_str r; r.data = (const uint8_t *)s.zv_data;\n"
                     "    r.len = s.zv_len; return r;\n"
                     "}\n");
        if (cg->needs_string_append) {
            buf_puts(&out,
                     "static void zov_string_reserve(zov_type_String *s, "
                     "size_t extra) {\n"
                     "    size_t need, cap;\n"
                     "    if (extra > SIZE_MAX - s->zv_len) "
                     "zov_string_oom();\n"
                     "    need = s->zv_len + extra;\n"
                     "    if (need <= s->zv_cap) return;\n"
                     "    cap = s->zv_cap ? s->zv_cap : 32u;\n"
                     "    while (cap < need) {\n"
                     "        if (cap > SIZE_MAX / 2u) { cap = need; "
                     "break; }\n"
                     "        cap *= 2u;\n"
                     "    }\n");
            if (cg->freestanding)
                buf_puts(&out,
                         "    { void *p = zov_rt_realloc(s->zv_data, "
                         "cap);\n"
                         "      if (!p) zov_string_oom();\n"
                         "      s->zv_data = (uint8_t *)p; }\n");
            else
                buf_puts(&out,
                         "    { void *p = realloc(s->zv_data, cap);\n"
                         "      if (!p) zov_string_oom();\n"
                         "      s->zv_data = (uint8_t *)p; }\n");
            buf_puts(&out,
                     "    s->zv_cap = cap;\n"
                     "}\n"
                     "static void zov_string_append(zov_type_String *s, "
                     "zov_str v) {\n"
                     "    if (!v.len) return;\n"
                     "    zov_string_reserve(s, v.len);\n"
                     "    { size_t i; for (i = 0; i < v.len; i++)\n"
                     "        ((uint8_t *)s->zv_data)[s->zv_len + i] = "
                     "v.data[i]; }\n"
                     "    s->zv_len += v.len;\n"
                     "}\n");
        }
        if (cg->needs_string_new)
            buf_puts(&out,
                     "static zov_type_String zov_string_new(void) {\n"
                     "    zov_type_String s; zov_string_init(&s); return s;\n"
                     "}\n");
        if (cg->needs_string_from_str)
            buf_puts(&out,
                     "static zov_type_String zov_string_from_str(zov_str v) "
                     "{\n"
                     "    zov_type_String s; zov_string_init(&s);\n"
                     "    zov_string_append(&s, v); return s;\n"
                     "}\n");
    }
    if (cg->needs_io) {
        buf_puts(&out, "#include <stdio.h>\n#include <inttypes.h>\n");
        if (cg->needs_utf8) {
            buf_puts(&out,
                     "static int zov_utf8(char buf[5], uint32_t c) {\n"
                     "    if (c < 0x80) { buf[0] = (char)c; return 1; }\n"
                     "    if (c < 0x800) {\n"
                     "        buf[0] = (char)(0xC0 | (c >> 6));\n"
                     "        buf[1] = (char)(0x80 | (c & 0x3F));\n"
                     "        return 2;\n"
                     "    }\n"
                     "    if (c < 0x10000) {\n"
                     "        buf[0] = (char)(0xE0 | (c >> 12));\n"
                     "        buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));\n"
                     "        buf[2] = (char)(0x80 | (c & 0x3F));\n"
                     "        return 3;\n"
                     "    }\n"
                     "    buf[0] = (char)(0xF0 | (c >> 18));\n"
                     "    buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));\n"
                     "    buf[2] = (char)(0x80 | ((c >> 6) & 0x3F));\n"
                     "    buf[3] = (char)(0x80 | (c & 0x3F));\n"
                     "    return 4;\n"
                     "}\n");
        }
    }
    buf_puts(&out, cg->str_helpers.data);
    for (size_t i = 0; i < cg->unions.len; i++) {
        Type *u = cg->unions.items[i];
        buf_printf(&out, "typedef struct { uint32_t tag;");
        int any_field = 0;
        for (size_t k = 0; k < u->members.len; k++)
            if (member_has_payload(u->members.items[k]))
                any_field = 1;
        if (any_field) {
            buf_puts(&out, " union {");
            for (size_t k = 0; k < u->members.len; k++)
                if (member_has_payload(u->members.items[k])) {
                    buf_puts(&out, " ");
                    emit_var(&out, cg, u->members.items[k], "");
                    buf_printf(&out, "v%lu;", (unsigned long)k);
                }
            buf_puts(&out, " } u;");
        }
        buf_printf(&out, " } zov_union_%lu;\n", (unsigned long)i);
    }
    buf_puts(&out, cg->protos.data);
    buf_puts(&out, cg->body.data);
    char *result = malloc(out.len + 1);
    if (!result)
        fatal("out of memory");
    memcpy(result, out.data, out.len + 1);
    free(out.data);
    free(cg->head.data);
    free(cg->protos.data);
    free(cg->body.data);
    free(cg->str_helpers.data);
    return result;
}
