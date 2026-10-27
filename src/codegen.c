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
    Buf *cur; /* active emission target */
    DA(DeferEntry) defers;
    DA(size_t) loop_marks; /* defer-stack base per active loop */
    DA(int) loop_ids;      /* label id per active loop */
    int terminated; /* current block ended in an unconditional jump */
    DA(Type *) slices;
    DA(Type *) unions;
    int needs_bounds;
    int needs_abort;
    int needs_io;   /* str-concat and diagnostic emission */
    int needs_utf8; /* char formatting helper */
    int needs_string; /* memcmp emission for native str equality */
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

/* str-concat flattening: left-assoc '+' chains over str collapse into an
 * ordered segment list before any I/O is emitted */
static void flatten_concat(CG *cg, Expr *e, ExprList *segs)
{
    (void)cg;
    if (e->kind == EX_BINARY && e->u.binary.op == TK_PLUS && e->type
        && e->type->kind == TY_STR) {
        flatten_concat(cg, e->u.binary.lhs, segs);
        flatten_concat(cg, e->u.binary.rhs, segs);
        return;
    }
    da_push(segs, e);
}

/* ---- expressions ---- */

static void emit_expr(CG *cg, Expr *e, int cstr);

static void emit_expr(CG *cg, Expr *e, int cstr);
static int emit_block(CG *cg, Block *blk);
static int emit_stmt(CG *cg, Stmt *s);
static void emit_defers_down_to(CG *cg, size_t base);

/* pure segment: safe to evaluate twice (nullable/char always hoist) */
static int seg_is_trivial(Expr *e)
{
    switch (e->kind) {
    case EX_STR: case EX_INT: case EX_FLOAT: case EX_BOOL: case EX_CHAR:
    case EX_NULL: case EX_IDENT: case EX_NS_IDENT: case EX_FIELD:
    case EX_DEREF:
        return 1;
    default:
        return 0;
    }
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

/* one prepared output unit: temps hoist side effects in order */
typedef struct PSeg {
    Expr *e;
    int tmp;      /* value lives in _p<tmp> */
    int char_tmp; /* utf8 bytes in _c<char_tmp>, length _cl<char_tmp> */
    int nul_tmp;  /* nullable pointer lives in _p<nul_tmp> */
} PSeg;

static void prepare_seg(CG *cg, Expr *e, PSeg *ps, Buf *pre)
{
    memset(ps, 0, sizeof(*ps));
    ps->e = e;
    Type *t = e->type;
    int hoist = !seg_is_trivial(e);
    if (t->kind == TY_CHAR)
        hoist = 1; /* encode order must precede all I/O calls */
    if (t->kind == TY_PTR && t->nullable)
        hoist = 1; /* the null check reads the value twice */
    if (t->kind == TY_STR && e->kind != EX_STR)
        hoist = 1; /* .len/.data would evaluate the expr twice */
    if (!hoist)
        return;
    ps->tmp = ++cg->tmp_id;
    Buf *saved = cg->cur; /* segment exprs stream into the pre buffer */
    cg->cur = pre;
    if (t->kind == TY_CHAR) {
        cg->needs_utf8 = 1;
        buf_printf(pre, "uint32_t _p%d = (uint32_t)(", ps->tmp);
        emit_expr(cg, e, 0);
        buf_printf(pre, ");\n");
        ps->char_tmp = ps->tmp;
        buf_printf(pre, "char _c%d[5]; int _cl%d = zov_utf8(_c%d, _p%d);\n",
                   ps->tmp, ps->tmp, ps->tmp, ps->tmp);
        cg->cur = saved;
        return;
    }
    if (t->kind == TY_PTR && t->nullable) {
        ps->nul_tmp = ps->tmp;
        buf_printf(pre, "%s *_p%d = (", c_type(cg, t), ps->tmp);
        emit_expr(cg, e, 0);
        buf_printf(pre, ");\n");
        cg->cur = saved;
        return;
    }
    buf_printf(pre, "%s _p%d = (", c_type(cg, t), ps->tmp);
    emit_expr(cg, e, 0);
    buf_printf(pre, ");\n");
    cg->cur = saved;
}

/* Case B: standalone str value rides a frame-local scratch buffer */
static void emit_str_value(CG *cg, Expr *e)
{
    cg->needs_io = 1;
    Buf *b = &*cg->cur;
    ExprList segs = {0};
    flatten_concat(cg, e, &segs);
    struct { PSeg *items; size_t len, cap; } ps = {0};
    Buf pre = {0};
    Buf txt = {0};   /* pending literal text */
    Buf args = {0};
    char texts[128][64]; /* text chunk per 'T' piece */
    char macros[128][16]; /* macro name per 'S' piece */
    char pieces[128];
    size_t npieces = 0;
    for (size_t i = 0; i < segs.len; i++) {
        Expr *seg = segs.items[i];
        PSeg one;
        prepare_seg(cg, seg, &one, &pre);
        da_push(&ps, one);
        if (seg->kind == EX_STR) {
            append_fmt_str(&txt, seg->u.slit.v, seg->u.slit.len);
            continue;
        }
        Type *t = seg->type;
        if (ty_is_num(t->kind)) {
            if (ty_is_float(t->kind)) {
                buf_puts(&txt, "%g");
                buf_puts(&args, ", (double)(");
            } else {
                /* inttypes macros splice outside the literal: the '%' of
                 * the conversion stays inside the preceding text chunk */
                buf_puts(&txt, "%");
                if (npieces < 128) {
                    snprintf(texts[npieces], 64, "%s",
                             txt.len ? txt.data : "");
                    pieces[npieces++] = 'T';
                }
                if (npieces < 128) {
                    snprintf(macros[npieces], 16, "%s", int_spec(t->kind));
                    pieces[npieces++] = 'S';
                }
                txt.len = 0;
                if (txt.data)
                    txt.data[0] = '\0';
                buf_printf(&args, ", (%s)(", c_type(cg, t));
            }
        } else if (t->kind == TY_BOOL) {
            buf_puts(&txt, "%s");
            buf_puts(&args, ", ((");
        } else if (t->kind == TY_ENUM) {
            buf_puts(&txt, "%s");
            buf_printf(&args, ", zov_enum_%s_name((", t->name);
            if (one.tmp)
                buf_printf(&args, "_p%d", one.tmp);
            else {
                Buf *saved2 = cg->cur;
                cg->cur = &args;
                emit_expr(cg, seg, 0);
                cg->cur = saved2;
            }
            buf_puts(&args, "))");
            continue;
        } else if (t->kind == TY_CHAR) {
            buf_puts(&txt, "%.*s");
            buf_printf(&args, ", _cl%d, _c%d", one.char_tmp, one.char_tmp);
            continue;
        } else if (t->kind == TY_STR) {
            buf_puts(&txt, "%.*s");
            buf_printf(&args, ", (int)_p%d.len, _p%d.data", one.tmp,
                       one.tmp);
            continue;
        } else if (t->kind == TY_PTR && t->nullable) {
            /* print "null" or the address without double evaluation */
            buf_printf(&pre, "char _pb%d[32]; int _pn%d;\n", one.nul_tmp,
                       one.nul_tmp);
            buf_printf(&pre, "if (_p%d == NULL) { _pn%d = -1; }\n"
                             "else { _pn%d = snprintf(_pb%d, sizeof(_pb%d), "
                             "\"%%p\", (void *)_p%d); }\n",
                       one.nul_tmp, one.nul_tmp, one.nul_tmp, one.nul_tmp,
                       one.nul_tmp, one.nul_tmp);
            buf_puts(&txt, "%.*s");
            buf_printf(&args,
                       ", (_pn%d < 0 ? 4 : _pn%d), (_pn%d < 0 ? \"null\" : "
                       "_pb%d)", one.nul_tmp, one.nul_tmp, one.nul_tmp,
                       one.nul_tmp);
            continue;
        } else if (t->kind == TY_PTR) {
            buf_puts(&txt, "%p");
            buf_puts(&args, ", (void *)(");
        } else {
            fatal("internal: unprintable concat segment");
        }
        if (one.tmp) {
            buf_printf(&args, "_p%d", one.tmp);
        } else {
            Buf *saved = cg->cur; /* inline args stream into args buf */
            cg->cur = &args;
            emit_expr(cg, seg, 0);
            cg->cur = saved;
        }
        if (t->kind == TY_BOOL)
            buf_puts(&args, ") ? \"true\" : \"false\"");
        buf_puts(&args, ")");
    }
    int id = cg->tmp_id++;
    // 256-byte cap: snprintf truncation is the defined overflow policy
    buf_printf(b, "({ char _sbuf%d[256];\n", id);
    if (pre.len)
        buf_puts(b, pre.data);
    buf_printf(b, "int _slen%d = snprintf(_sbuf%d, sizeof(_sbuf%d), ", id,
               id, id);
    /* emit alternating quoted text and macro splices */
    if (npieces == 0) {
        buf_puts(b, "\"");
        if (txt.len)
            buf_puts(b, txt.data);
        buf_puts(b, "\"");
    } else {
        for (size_t i = 0; i < npieces; i++) {
            if (pieces[i] == 'T') {
                buf_puts(b, "\"");
                buf_puts(b, texts[i]);
                buf_puts(b, "\" ");
            } else {
                buf_puts(b, macros[i]);
                buf_puts(b, " ");
            }
        }
        /* trailing literal text after the last splice */
        buf_puts(b, "\"");
        if (txt.len)
            buf_puts(b, txt.data);
        buf_puts(b, "\"");
    }
    buf_puts(b, " ");
    if (args.len)
        buf_puts(b, args.data);
    buf_printf(b, ");\n(zov_str){ (const uint8_t *)_sbuf%d, "
                  "(size_t)_slen%d }; })\n", id, id);
    free(segs.items);
    free(ps.items);
    free(pre.data);
    free(txt.data);
    free(args.data);
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
    if (res)
        buf_printf(&*cg->cur, "%s = ", res);
    emit_expr(cg, a->val, 0);
    buf_puts(&*cg->cur, ";\n");
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

/* GNU statement-expressions keep every lowering expression-local; the
 * match result rides through a block-local temp when used as a value. */
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
        /* result temp lives inside the statement-expression so the whole
         * match nests cleanly in any initializer or argument position */
        buf_printf(b, "({\n");
        emit_var(b, cg, e->type, res);
        buf_puts(b, ";\n");
        if (st->kind == TY_UNION)
            emit_union_cases(cg, e, id, res);
        else if (st->kind == TY_PTR && st->nullable)
            emit_nullable_cases(cg, e, id, res);
        else if (st->kind == TY_STR)
            emit_str_cases(cg, e, id, res);
        else
            emit_value_cases(cg, e, id, res, c_type(cg, st));
        buf_printf(b, "%s; })\n", res);
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
    if (!as_stmt)
        buf_puts(b, "({ ");
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
        buf_puts(b, ")) { __zov_panic(\"assert failed: ");
        append_fmt_str(b, cond->span.start,
                       (size_t)(cond->span.end - cond->span.start));
        buf_puts(b, "\", ");
        emit_str_lit(b, cond->span.file, strlen(cond->span.file));
        buf_printf(b, ", %luu, 0); }\n", (unsigned long)cond->span.line);
    } else {
        buf_puts(b, ")) { __builtin_trap(); }\n");
    }
    if (!as_stmt)
        buf_puts(b, "0; })\n");
}

static void emit_call(CG *cg, Expr *e)
{
    Expr *callee = e->u.call.callee;
    if (e->hash_ty) {
        cg->needs_hash = 1;
        if (e->hash_ty->kind == TY_STR) {
            buf_puts(&*cg->cur, "zov_hash_str((");
            emit_expr(cg, e->u.call.args.items[0], 0);
            buf_puts(&*cg->cur, "))");
            return;
        }
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
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        if (i)
            buf_puts(&*cg->cur, ", ");
        Expr *arg = e->u.call.args.items[i];
        if (extern_cstr && arg->type && arg->type->kind == TY_STR
            && arg->kind != EX_STR) {
            // str value into a char* param: pass the fat pointer's data
            buf_puts(&*cg->cur, "(");
            emit_expr(cg, arg, 0);
            buf_puts(&*cg->cur, ").data");
        } else {
            emit_expr(cg, arg, extern_cstr);
        }
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
        if (e->type && e->type->kind == TY_STR
            && e->u.binary.op == TK_PLUS) {
            /* str + T scripting chain: scratch-buffer zov_str value */
            emit_str_value(cg, e);
            break;
        }
        if ((e->u.binary.op == TK_LT || e->u.binary.op == TK_LE
             || e->u.binary.op == TK_GT || e->u.binary.op == TK_GE)
            && e->u.binary.lhs->type && e->u.binary.rhs->type
            && e->u.binary.lhs->type->kind == TY_STR
            && e->u.binary.rhs->type->kind == TY_STR) {
            /* lexical ordering via memcmp helper over hoisted temps */
            cg->needs_strcmp = 1;
            const char *op = e->u.binary.op == TK_LT ? "<"
                           : e->u.binary.op == TK_LE ? "<="
                           : e->u.binary.op == TK_GT ? ">" : ">=";
            int a_id = cg->tmp_id++;
            int b_id = cg->tmp_id++;
            buf_printf(b, "({ zov_str _so%d = (", a_id);
            emit_expr(cg, e->u.binary.lhs, 0);
            buf_printf(b, "); zov_str _so%d = (", b_id);
            emit_expr(cg, e->u.binary.rhs, 0);
            buf_printf(b, "); (zov_str_cmp(_so%d, _so%d) %s 0); })",
                       a_id, b_id, op);
            break;
        }
        if ((e->u.binary.op == TK_EQ || e->u.binary.op == TK_NEQ)
            && e->u.binary.lhs->type && e->u.binary.rhs->type
            && e->u.binary.lhs->type->kind == TY_STR
            && e->u.binary.rhs->type->kind == TY_STR) {
            /* native str ==/!=: length + memcmp over hoisted fat ptrs */
            int a_id = cg->tmp_id++;
            int b_id = cg->tmp_id++;
            cg->needs_string = 1;
            buf_printf(b, "({ zov_str _se%d = (", a_id);
            emit_expr(cg, e->u.binary.lhs, 0);
            buf_printf(b, "); zov_str _se%d = (", b_id);
            emit_expr(cg, e->u.binary.rhs, 0);
            buf_printf(b, "); %s(_se%d.len == _se%d.len && memcmp(_se%d.data, "
                          "_se%d.data, _se%d.len) == 0); })",
                       e->u.binary.op == TK_EQ ? "" : "!",
                       a_id, b_id, a_id, b_id, a_id);
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
        Buf *saved = cg->cur;
        Buf pre = {0};
        cg->cur = &pre;
        buf_printf(&pre, "%s _or%d = (", c_type(cg, lt), id);
        emit_expr(cg, e->u.orelse.lhs, 0);
        buf_printf(&pre, ");\n");
        cg->cur = saved;
        int is_ptr = (lt->kind == TY_PTR);
        /* fail test: NULL for pointers, any error tag for unions */
        buf_printf(b, "({ %s ", pre.data ? pre.data : "");
        /* result declared before the branch so both paths assign it */
        emit_var(b, cg, e->type, "_orr");
        buf_puts(b, ";\n");
        buf_printf(b, "if (");
        if (is_ptr) {
            buf_printf(b, "_or%d == NULL", id);
        } else {
            int first = 1;
            for (size_t i = 0; i < lt->members.len; i++) {
                if (!lt->members.items[i]->is_error)
                    continue;
                if (!first)
                    buf_printf(b, " || ");
                buf_printf(b, "_or%d.tag == %luu", id, (unsigned long)i);
                first = 0;
            }
        }
        buf_printf(b, ") {\n");
        if (e->u.orelse.jump) {
            Stmt *j = e->u.orelse.jump;
            if (j->kind == ST_RETURN) {
                emit_defers_down_to(cg, 0);
                buf_puts(b, "return ");
                if (j->u.ret)
                    emit_expr(cg, j->u.ret, 0);
                buf_puts(b, ";\n");
            } else if (j->kind == ST_BREAK) {
                emit_defers_down_to(
                    cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
                if (cg->switch_depth > 0)
                    buf_printf(b, "goto zov_brk_%d;\n",
                               cg->loop_ids.items[cg->loop_ids.len - 1]);
                else
                    buf_puts(b, "break;\n");
            } else {
                emit_defers_down_to(
                    cg, cg->loop_marks.items[cg->loop_marks.len - 1]);
                if (cg->switch_depth > 0)
                    buf_printf(b, "goto zov_cont_%d;\n",
                               cg->loop_ids.items[cg->loop_ids.len - 1]);
                else
                    buf_puts(b, "continue;\n");
            }
        } else {
            buf_puts(b, "_orr = ");
            emit_expr(cg, e->u.orelse.val, 0);
            buf_puts(b, ";\n");
        }
        buf_printf(b, "} else {\n");
        buf_puts(b, "_orr = ");
        if (is_ptr)
            buf_printf(b, "_or%d;", id);
        else
            buf_printf(b, "_or%d.u.v%d;", id, e->coerce_tag);
        buf_puts(b, "\n}\n");
        buf_puts(b, "_orr; })");
        free(pre.data);
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
        if (bt->kind == TY_STR) {
            /* str sub-slice: same fat-pointer type, offset data */
            buf_printf(b, "({ zov_str _sl%d = (", id);
            emit_expr(cg, e->u.slice.base, 0);
            buf_printf(b, "); size_t _sa%d = (size_t)(", id);
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_printf(b, "), _sb%d = (size_t)(", id);
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_printf(b, "_sl%d.len", id);
            buf_printf(b, "); zov_slice_check(_sa%d, _sb%d, _sl%d.len, ", id,
                       id, id);
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu); ", (unsigned long)e->span.line);
            buf_printf(b, "(zov_str){ _sl%d.data + _sa%d, _sb%d - _sa%d }; "
                          "})", id, id, id, id);
            break;
        }
        if (bt->kind == TY_SLICE) {
            buf_printf(b, "({ %s _sl%d = (", c_type(cg, bt), id);
            emit_expr(cg, e->u.slice.base, 0);
            buf_printf(b, "); size_t _sa%d = (size_t)(", id);
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_printf(b, "), _sb%d = (size_t)(", id);
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_printf(b, "_sl%d.len", id);
            buf_printf(b, "); zov_slice_check(_sa%d, _sb%d, _sl%d.len, ", id,
                       id, id);
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu); ", (unsigned long)e->span.line);
            buf_printf(b, "(%s){ _sl%d.data + _sa%d, _sb%d - _sa%d }; })",
                       c_type(cg, e->type), id, id, id, id);
            break;
        }
        if (bt->kind == TY_ARRAY) {
            /* arrays decay: length is the compile-time element count */
            buf_printf(b, "(%s){ (", c_type(cg, e->type));
            emit_expr(cg, e->u.slice.base, 0);
            buf_printf(b, ") + zov_slice_check((size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_printf(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_printf(b, "%luu", (unsigned long)bt->count);
            buf_printf(b, "), %luu, ", (unsigned long)bt->count);
            emit_str_lit(b, e->span.file, strlen(e->span.file));
            buf_printf(b, ", %luu), (size_t)(", (unsigned long)e->span.line);
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_printf(b, "%luu", (unsigned long)bt->count);
            buf_puts(b, ") - (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, ") }");
            break;
        }
        if (bt->kind == TY_PTR) {
            /* raw pointer slice: no base length, end is caller-trusted */
            buf_printf(b, "(%s){ (", c_type(cg, e->type));
            emit_expr(cg, e->u.slice.base, 0);
            buf_printf(b, ") + (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_printf(b, "), (size_t)(");
            if (e->u.slice.end)
                emit_expr(cg, e->u.slice.end, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, ") - (size_t)(");
            if (e->u.slice.start)
                emit_expr(cg, e->u.slice.start, 0);
            else
                buf_puts(b, "0u");
            buf_puts(b, ") }");
            break;
        }
        fatal("internal: unprintable slice base");
        break;
    }
    case EX_INDEX: {
        cg->needs_bounds = 1;
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
        if (t->kind == TY_UNION) {
            const char *un = union_name(cg, t);
            buf_printf(b, "({ %s _q%d = (", un, id);
            emit_expr(cg, e->u.tryy, 0);
            buf_printf(b, ");\n");
            for (size_t i = 0; i < t->members.len; i++) {
                Type *m = t->members.items[i];
                if (!m->is_error)
                    continue;
                buf_printf(b, "if (_q%d.tag == %luu) {\n", id, (unsigned long)i);
                // reuse the lexical defer stack: errors unroll LIFO
                emit_defers_down_to(cg, 0);
                Type *rt = cg->fn_ret;
                int ridx = rt ? cg_union_idx(rt, m) : -1;
                buf_printf(b, "return ((%s){ %du", union_name(cg, rt), ridx);
                if (member_has_payload(m))
                    buf_printf(b, ", _q%d.u.v%lu", id, (unsigned long)i);
                buf_printf(b, " });\n}\n");
            }
            Type *ok = union_ok_member(t);
            if (member_has_payload(ok))
                buf_printf(b, "_q%d.u.v%d; })", id, cg_union_idx(t, ok));
            else
                /* void payload: the try itself yields nothing */
                buf_printf(b, "})");
        } else {
            buf_printf(b, "({ %s *_q%d = (", c_type(cg, t->base), id);
            emit_expr(cg, e->u.tryy, 0);
            buf_printf(b, "); if (_q%d == NULL) {\n", id);
            emit_defers_down_to(cg, 0);
            buf_printf(b, "return NULL;\n}\n_q%d; })", id);
        }
        break;
    }
    case EX_UNWRAP: {
        cg->needs_abort = 1;
        Type *t = e->u.unwrap->type;
        int id = cg->tmp_id++;
        if (t->kind == TY_UNION) {
            const char *un = union_name(cg, t);
            buf_printf(b, "({ %s _u%d = (", un, id);
            emit_expr(cg, e->u.unwrap, 0);
            if (cg->freestanding) {
                cg->needs_abort = 1;
                buf_printf(b, "); if (_u%d.tag != %du) { zov_panic("
                              "\"unwrap of error value\", ", id,
                           cg_union_idx(t, union_ok_member(t)));
                emit_str_lit(b, e->span.file, strlen(e->span.file));
                buf_printf(b, ", %luu); }\n",
                           (unsigned long)e->span.line);
            } else {
                cg->needs_abort = 1;
                buf_printf(b, "); if (_u%d.tag != %du) { abort(); }\n", id,
                           cg_union_idx(t, union_ok_member(t)));
            }
            buf_printf(b, "_u%d.u.v%d; })", id,
                       cg_union_idx(t, union_ok_member(t)));
        } else {
            buf_printf(b, "({ %s *_u%d = (", c_type(cg, t->base), id);
            emit_expr(cg, e->u.unwrap, 0);
            if (cg->freestanding) {
                cg->needs_abort = 1;
                buf_printf(b, "); if (_u%d == NULL) { zov_panic("
                              "\"unwrap of null\", ", id);
                emit_str_lit(b, e->span.file, strlen(e->span.file));
                buf_printf(b, ", %luu); }\n",
                           (unsigned long)e->span.line);
            } else if (cg->debug) {
                cg->needs_io = 1;
                buf_printf(b, "); if (_u%d == NULL) { __zov_panic("
                              "\"unwrap of null\", ", id);
                emit_str_lit(b, e->span.file, strlen(e->span.file));
                buf_printf(b, ", %luu, 0); }\n",
                           (unsigned long)e->span.line);
            } else {
                cg->needs_abort = 1;
                buf_printf(b, "); if (_u%d == NULL) "
                              "{ __builtin_trap(); }\n", id);
            }
            buf_printf(b, "_u%d; })", id);
        }
        break;
    }
    case EX_COERCE: {
        Type *u = e->u.coerce.target;
        Type *m = u->members.items[e->coerce_tag];
        buf_printf(b, "((%s){ %du", union_name(cg, u), e->coerce_tag);
        if (e->u.coerce.e && member_has_payload(m)) {
            buf_puts(b, ", ");
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

static int emit_stmt(CG *cg, Stmt *s)
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
        buf_printf(&*cg->cur, "zov_cont_%d: ;\n}\n", id);
        buf_printf(&*cg->cur, "zov_brk_%d: ;\n", id);
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
        buf_printf(cur, "zov_cont_%d: ;\n}\n", id);
        buf_printf(cur, "zov_brk_%d: ;\n}\n", id);
        return 0;
    }
    case ST_WHILE: {
        buf_puts(&*cg->cur, "while (");
        emit_expr(cg, s->u.whilee.cond, 0);
        buf_puts(&*cg->cur, ") {\n");
        int id = cg->tmp_id++;
        da_push(&cg->loop_marks, cg->defers.len);
        da_push(&cg->loop_ids, id);
        emit_block(cg, s->u.whilee.body);
        cg->loop_marks.len--;
        cg->loop_ids.len--;
        // labeled exits let break/continue jump out of lowered switches
        buf_printf(&*cg->cur, "zov_cont_%d: ;\n", id);
        buf_puts(&*cg->cur, "}\n");
        buf_printf(&*cg->cur, "zov_brk_%d: ;\n", id);
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
    if (cg->debug && !cg->freestanding) {
        buf_puts(&*cg->cur,
                 "#include <stdio.h>\n#include <stdlib.h>\n"
                 "static void __zov_panic(const char *msg, "
                 "const char *file, int line, int col) {\n"
                 "    fprintf(stderr, \"thread 'main' panicked at "
                 "'%s'\\n  --> %s:%d:%d\\n\", msg, file, line, col);\n"
                 "    exit(101);\n"
                 "}\n");
    }

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
            /* variant names: str-concat stringify rides these helpers */
            buf_printf(&*cg->cur, "static const char *zov_enum_%s_name("
                                  "%s v) { switch (v) { ",
                       d->u.type.name, mangle_type(d->u.type.name));
            for (size_t j = 0; j < d->u.type.variants.len; j++) {
                EnumVariant *v = &d->u.type.variants.items[j];
                buf_printf(&*cg->cur, "case Zov_%s_%s: return \"", d->u.type.name,
                           v->name);
                append_fmt_str(&*cg->cur, v->name, strlen(v->name));
                buf_printf(&*cg->cur, "\"; ");
            }
            buf_printf(&*cg->cur, "default: return \"<invalid>\"; } }\n");
            continue;
        }
        if (d->u.type.alias_ty)
            continue; /* aliases emit no C definition */
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
                           "        __builtin_trap();\n"
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
        if (cg->needs_string || cg->needs_strcmp || cg->needs_slice)
            /* memcmp stays a kernel-provided primitive at link time */
            buf_puts(&out, "#include <string.h>\n");
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
                     "        __builtin_trap();\n"
                     "    }\n"
                     "    return a;\n"
                     "}\n");
            }
        }
        if (cg->needs_string)
            buf_puts(&out, "#include <string.h>\n");
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
    if (cg->needs_hash) {
        /* FNV-1a over bytes; splitmix64 finalizer for scalars */
        buf_puts(&out,
                 "static uint64_t zov_hash_str(zov_str s) {\n"
                 "    uint64_t h = 1469598103934665603ULL;\n"
                 "    for (size_t i = 0; i < s.len; i++) {\n"
                 "        h ^= s.data[i]; h *= 1099511628211ULL;\n"
                 "    }\n"
                 "    return h;\n"
                 "}\n"
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
    return result;
}
