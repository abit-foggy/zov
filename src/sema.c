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

#include "sema.h"
#include "cimport.h"

#include <ctype.h>

typedef enum SymKind { SYM_VAR, SYM_FN, SYM_NS, SYM_TYPE } SymKind;

typedef struct Sym {
    const char *name;
    SymKind kind;
    Span span;
    Type *type;      /* variable type, alias type, or struct type */
    int mut;
    DA(Param) params; /* function signature */
    Type *ret;
    CHeader *hdr;     /* imported C namespace members */
    int is_zov;       /* namespace of a loaded .zov module */
    StrList zsyms;    /* exported symbol names of the module */
    int is_template;  /* generic decl, checked only via instantiation */
    Decl *decl;       /* owning decl for templates */
} Sym;

typedef struct Scope {
    struct Scope *parent;
    DA(Sym) syms;
} Scope;

typedef struct Sema {
    Scope *scope;
    Type *fn_ret;
    int loop_depth;
    int has_main;
    Program *prog;
    DA(struct Meth) methods;
} Sema;

typedef struct Meth {
    const char *sname; /* struct template name: Vec, Point */
    const char *mname; /* method name as written: push */
    Sym *sym;
} Meth;

static void push_scope(Sema *se)
{
    Scope *s = zalloc(sizeof(Scope));
    s->parent = se->scope;
    se->scope = s;
}

static void pop_scope(Sema *se)
{
    se->scope = se->scope->parent;
}

static Sym *scope_find(Scope *sc, const char *name)
{
    for (Scope *s = sc; s; s = s->parent)
        for (size_t i = s->syms.len; i > 0; i--)
            if (s->syms.items[i - 1].name == name)
                return &s->syms.items[i - 1];
    return NULL;
}

static void scope_add(Sema *se, Sym sym)
{
    for (size_t i = se->scope->syms.len; i > 0; i--)
        if (se->scope->syms.items[i - 1].name == sym.name)
            fatal_at(sym.span, "duplicate definition of '%s'", sym.name);
    da_push(&se->scope->syms, sym);
}

static Type *check_expr(Sema *se, Expr *e, Type *hint);
static void check_stmt(Sema *se, Stmt *s);
static void check_block(Sema *se, Block *b);

/* Field lists live on the decl, not the Type, so literals and field
 * access resolve through the owning DL_TYPE. */
static Decl *find_struct_decl(Sema *se, const char *name)
{
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind == DL_TYPE && d->u.type.name == name
            && !d->u.type.alias_ty && d->u.type.tparams.len == 0)
            return d;
    }
    return NULL;
}

static Type *unknown(void)
{
    return type_prim(TY_UNKNOWN);
}

static int is_unknown(Type *t)
{
    return t && t->kind == TY_UNKNOWN;
}

static int is_ptrish(Type *t)
{
    return t && (t->kind == TY_PTR || t->kind == TY_REF
                 || t->kind == TY_MUTREF);
}

/* printable covers every intrinsic format a print segment can emit */
static int is_printable(Type *t)
{
    if (!t)
        return 0;
    switch (t->kind) {
    case TY_I8: case TY_I16: case TY_I32: case TY_I64: case TY_ISIZE:
    case TY_U8: case TY_U16: case TY_U32: case TY_U64: case TY_USIZE:
    case TY_F32: case TY_F64: case TY_BOOL: case TY_CHAR: case TY_STR:
    case TY_PTR:
        return 1;
    default:
        return 0;
    }
}

static int union_has_error(Type *u)
{
    for (size_t i = 0; i < u->members.len; i++)
        if (u->members.items[i]->is_error)
            return 1;
    return 0;
}

static Type *union_payload(Type *u)
{
    Type *found = NULL;
    for (size_t i = 0; i < u->members.len; i++)
        if (!u->members.items[i]->is_error) {
            if (found)
                return NULL; /* ambiguous payload */
            found = u->members.items[i];
        }
    return found;
}

/* Resolve parser placeholders: ns::Type from C imports, local named
 * aliases/error types, and structural recursion into composites. */
static Type *resolve_type(Sema *se, Type *t);
static Type *instantiate_struct(Sema *se, const char *tname,
                                 TypeList *args);

static int zov_ns_member(Sema *se, const char *ns, const char *name)
{
    Sym *s = scope_find(se->scope, ns);
    if (!s || s->kind != SYM_NS || !s->is_zov)
        return 0;
    for (size_t i = 0; i < s->zsyms.len; i++)
        if (s->zsyms.items[i] == name)
            return 1;
    return 0;
}

static Type *resolve_ns_type(Sema *se, const char *qual)
{
    const char *sep = strstr(qual, "::");
    if (!sep)
        return NULL;
    char nbuf[128];
    if ((size_t)(sep - qual) >= sizeof(nbuf))
        return NULL;
    memcpy(nbuf, qual, (size_t)(sep - qual));
    nbuf[sep - qual] = '\0';
    Sym *ns = scope_find(se->scope, str_intern_c(nbuf));
    if (!ns || ns->kind != SYM_NS)
        return NULL;
    if (ns->is_zov) {
        /* loaded .zov module: member resolves to the global symbol */
        Sym *member = scope_find(se->scope, sep + 2);
        if (member && member->kind == SYM_TYPE && member->type)
            return member->type;
        return NULL;
    }
    if (!ns->hdr)
        return NULL;
    CMember *m = cimport_find(ns->hdr, sep + 2);
    if (!m || m->kind != CM_TYPE)
        return NULL;
    return m->ret;
}

static Type *resolve_type(Sema *se, Type *t)
{
    if (!t)
        return t;
    switch (t->kind) {
    case TY_STRUCT: {
        if (strstr(t->name, "::")) {
            Type *r = resolve_ns_type(se, t->name);
            if (!r)
                fatal("unknown imported type '%s'", t->name);
            return r;
        }
        /* named aliases: !T error types and unions registered earlier */
        Sym *sym = scope_find(se->scope, t->name);
        if (sym && sym->kind == SYM_TYPE && sym->type && sym->type != t)
            return sym->type;
        return t;
    }
    case TY_PTR:
        return type_ptr(resolve_type(se, t->base), t->nullable);
    case TY_REF:
    case TY_MUTREF:
        return type_ref(resolve_type(se, t->base), t->kind == TY_MUTREF);
    case TY_SLICE:
        return type_slice(resolve_type(se, t->base));
    case TY_ARRAY:
        return type_array(resolve_type(se, t->base), t->count);
    case TY_UNION: {
        TypeList ms = {0};
        for (size_t i = 0; i < t->members.len; i++)
            da_push(&ms, resolve_type(se, t->members.items[i]));
        Type *u = type_union(&ms);
        u->is_error = t->is_error;
        return u;
    }
    case TY_ERR:
        return type_err(t->name, resolve_type(se, t->base));
    case TY_INST: {
        for (size_t i = 0; i < t->members.len; i++)
            t->members.items[i] = resolve_type(se, t->members.items[i]);
        if (!t->name)
            return t;
        return instantiate_struct(se, t->name, &t->members);
    }
    default:
        return t;
    }
}

/* Member index of `t` inside union `u`; error variants key by name. */
static int union_member_index(Type *u, Type *t)
{
    for (size_t i = 0; i < u->members.len; i++) {
        Type *m = u->members.items[i];
        if (m->kind == TY_ERR && t->kind == TY_ERR && m->name == t->name)
            return (int)i;
        if (type_eq(m, t))
            return (int)i;
    }
    return -1;
}

/* Wrap e in EX_COERCE when target is a union and e holds a member type. */
static Expr *maybe_coerce(Expr *e, Type *target)
{
    if (!target || target->kind != TY_UNION || !e->type)
        return e;
    if (e->type->kind == TY_UNION || e->type->kind == TY_UNKNOWN)
        return e;
    if (union_member_index(target, e->type) < 0)
        return e;
    Expr *c = expr_new(EX_COERCE, e->span);
    c->u.coerce.e = e;
    c->u.coerce.target = target;
    c->type = target; /* already type-checked as a member above */
    return c;
}

/* ---- generic monomorphization ---- */

static void register_decl(Sema *se, Decl *d);
static void check_fn(Sema *se, Decl *d);

static Sym *find_method(Sema *se, const char *sname, const char *mname)
{
    for (size_t i = 0; i < se->methods.len; i++)
        if (se->methods.items[i].sname == sname
            && se->methods.items[i].mname == mname)
            return se->methods.items[i].sym;
    return NULL;
}

/* Receiver template params: unresolved type names inside the receiver
 * (Vec[T] yields ["T"]), collected in syntactic order. */
static void collect_recv_tparams(Sema *se, Type *t, StrList *out)
{
    if (!t)
        return;
    switch (t->kind) {
    case TY_REF:
    case TY_MUTREF:
    case TY_PTR:
    case TY_SLICE:
        collect_recv_tparams(se, t->base, out);
        return;
    case TY_ARRAY:
        collect_recv_tparams(se, t->base, out);
        return;
    case TY_INST:
        for (size_t i = 0; i < t->members.len; i++)
            collect_recv_tparams(se, t->members.items[i], out);
        return;
    case TY_STRUCT:
        if (t->name && !strstr(t->name, "::")
            && !scope_find(se->scope, t->name)) {
            for (size_t i = 0; i < out->len; i++)
                if (out->items[i] == t->name)
                    return;
            da_push(out, t->name);
        }
        return;
    default:
        return;
    }
}

/* cache key component: sanitize a type spelling into [A-Za-z0-9_.] */
static char *mangle_spelling(const char *s)
{
    char *out = zalloc(strlen(s) + 1);
    size_t n = 0;
    for (const char *p = s; *p; p++)
        out[n++] = (isalnum((unsigned char)*p) || *p == '_') ? *p : '_';
    out[n] = '\0';
    return out;
}

static Decl *find_decl_by_name(Sema *se, const char *name)
{
    for (size_t i = 0; i < se->prog->decls.len; i++)
        if (se->prog->decls.items[i]->kind == DL_TYPE
                ? se->prog->decls.items[i]->u.type.name == name
                : se->prog->decls.items[i]->u.fn.name == name)
            return se->prog->decls.items[i];
    return NULL;
}

static int tparam_index(StrList *tps, const char *name)
{
    for (size_t i = 0; i < tps->len; i++)
        if (tps->items[i] == name)
            return (int)i;
    return -1;
}

static Type *subst_type(Sema *se, Type *t, StrList *tps,
                        TypeList *args)
{
    if (!t)
        return t;
    switch (t->kind) {
    case TY_STRUCT: {
        int idx = tparam_index(tps, t->name);
        if (idx >= 0)
            return args->items[idx];
        return t;
    }
    case TY_PTR:
        return type_ptr(subst_type(se, t->base, tps, args), t->nullable);
    case TY_REF:
    case TY_MUTREF:
        return type_ref(subst_type(se, t->base, tps, args),
                        t->kind == TY_MUTREF);
    case TY_SLICE:
        return type_slice(subst_type(se, t->base, tps, args));
    case TY_ARRAY:
        return type_array(subst_type(se, t->base, tps, args), t->count);
    case TY_UNION: {
        TypeList ms = {0};
        for (size_t i = 0; i < t->members.len; i++)
            da_push(&ms, subst_type(se, t->members.items[i], tps, args));
        Type *u = type_union(&ms);
        u->is_error = t->is_error;
        return u;
    }
    case TY_ERR:
        return type_err(t->name, subst_type(se, t->base, tps, args));
    case TY_INST: {
        Type *inst = zalloc(sizeof(Type));
        inst->kind = TY_INST;
        inst->name = t->name;
        for (size_t i = 0; i < t->members.len; i++)
            da_push(&inst->members,
                    subst_type(se, t->members.items[i], tps, args));
        return resolve_type(se, inst);
    }
    default:
        return t;
    }
}

/* canonical key: <template>__<arg1>_<arg2>...; dedupes instantiations */
static char *inst_key(const char *base, TypeList *args)
{
    size_t cap = strlen(base) + 4;
    for (size_t i = 0; i < args->len; i++)
        cap += strlen(type_spelling(args->items[i])) + 2;
    char *key = zalloc(cap);
    size_t off = 0;
    off += (size_t)sprintf(key + off, "%s", base);
    for (size_t i = 0; i < args->len; i++)
        off += (size_t)sprintf(key + off, "__%s",
                               mangle_spelling(type_spelling(
                                   args->items[i])));
    return key;
}

static Type *instantiate_struct(Sema *se, const char *tname, TypeList *args)
{
    Sym *sym = scope_find(se->scope, tname);
    if (!sym && strstr(tname, "::")) {
        /* qualified template: vec::Vec[i32] -> the module's Vec template */
        const char *sep = strstr(tname, "::");
        char nbuf[128];
        if ((size_t)(sep - tname) < sizeof(nbuf)) {
            memcpy(nbuf, tname, (size_t)(sep - tname));
            nbuf[sep - tname] = '\0';
            Sym *ns = scope_find(se->scope, str_intern_c(nbuf));
            if (ns && ns->kind == SYM_NS && ns->is_zov)
                sym = scope_find(se->scope, sep + 2);
        }
    }
    if (!sym || sym->kind != SYM_TYPE || !sym->is_template || !sym->decl)
        fatal("'%s' is not a generic type", tname);
    StrList *tps = &sym->decl->u.type.tparams;
    if (tps->len != args->len)
        fatal("'%s' expects %lu type argument(s), got %lu", tname,
              (unsigned long)tps->len, (unsigned long)args->len);
    char *key = inst_key(tname, args);
    Decl *existing = find_decl_by_name(se, str_intern_c(key));
    if (existing)
        return type_struct(existing->u.type.name);
    Decl *d = decl_new(DL_TYPE, sym->decl->span);
    d->u.type.name = str_intern_c(key);
    for (size_t i = 0; i < sym->decl->u.type.fields.len; i++) {
        Field *f = &sym->decl->u.type.fields.items[i];
        Field nf;
        nf.name = f->name;
        nf.span = f->span;
        nf.ty = subst_type(se, f->ty, tps, args);
        da_push(&d->u.type.fields, nf);
    }
    if (sym->decl->u.type.is_resource)
        d->u.type.is_resource = 1;
    da_push(&se->prog->decls, d);
    register_decl(se, d);
    Type *it = type_struct(d->u.type.name);
    it->tmpl = sym->decl->u.type.name;
    for (size_t i = 0; i < args->len; i++)
        da_push(&it->inst_args, args->items[i]);
    return it;
}

/* deep AST clone with tparam substitution; types are re-checked */
static Expr *clone_expr(Sema *se, Expr *e, StrList *tps,
                        TypeList *args);
static Stmt *clone_stmt(Sema *se, Stmt *s, StrList *tps,
                        TypeList *args);

static Block *clone_block(Sema *se, Block *b, StrList *tps,
                          TypeList *args)
{
    if (!b)
        return NULL;
    Block *nb = block_new();
    for (size_t i = 0; i < b->stmts.len; i++)
        da_push(&nb->stmts, clone_stmt(se, b->stmts.items[i], tps, args));
    return nb;
}

static Stmt *clone_stmt(Sema *se, Stmt *s, StrList *tps,
                       TypeList *args)
{
    Stmt *n = stmt_new(s->kind, s->span);
    switch (s->kind) {
    case ST_LET:
        n->u.let.name = s->u.let.name;
        n->u.let.mut = s->u.let.mut;
        n->u.let.ty = s->u.let.ty
                          ? subst_type(se, s->u.let.ty, tps, args) : NULL;
        n->u.let.init = clone_expr(se, s->u.let.init, tps, args);
        break;
    case ST_DEFER:
        n->u.defer = clone_stmt(se, s->u.defer, tps, args);
        break;
    case ST_IF:
        n->u.iff.cond = clone_expr(se, s->u.iff.cond, tps, args);
        n->u.iff.then_blk = clone_block(se, s->u.iff.then_blk, tps, args);
        if (s->u.iff.else_stmt)
            n->u.iff.else_stmt = clone_stmt(se, s->u.iff.else_stmt, tps,
                                            args);
        break;
    case ST_WHILE:
        n->u.whilee.cond = clone_expr(se, s->u.whilee.cond, tps, args);
        n->u.whilee.body = clone_block(se, s->u.whilee.body, tps, args);
        break;
    case ST_RETURN:
        n->u.ret = clone_expr(se, s->u.ret, tps, args);
        break;
    case ST_EXPR:
        n->u.expr = clone_expr(se, s->u.expr, tps, args);
        break;
    case ST_BLOCK:
        n->u.block = clone_block(se, s->u.block, tps, args);
        break;
    default:
        break;
    }
    return n;
}

static MatchArm clone_arm(Sema *se, MatchArm *a, StrList *tps,
                          TypeList *args)
{
    MatchArm n;
    memset(&n, 0, sizeof(n));
    n.kind = a->kind;
    n.span = a->span;
    n.name = a->name;
    n.ty = a->ty ? subst_type(se, a->ty, tps, args) : NULL;
    n.val = clone_expr(se, a->val, tps, args);
    n.jump = a->jump ? clone_stmt(se, a->jump, tps, args) : NULL;
    return n;
}

static Expr *clone_expr(Sema *se, Expr *e, StrList *tps,
                        TypeList *args)
{
    if (!e)
        return NULL;
    Expr *n = expr_new(e->kind, e->span);
    n->borrow_mut = e->borrow_mut;
    switch (e->kind) {
    case EX_INT: n->u.ilit = e->u.ilit; break;
    case EX_FLOAT: n->u.flit = e->u.flit; break;
    case EX_STR: n->u.slit = e->u.slit; break;
    case EX_CHAR: n->u.clit = e->u.clit; break;
    case EX_BOOL: n->u.blit = e->u.blit; break;
    case EX_IDENT: n->u.ident = e->u.ident; break;
    case EX_NS_IDENT: n->u.nident = e->u.nident; break;
    case EX_ASSIGN:
        n->u.assign.lhs = clone_expr(se, e->u.assign.lhs, tps, args);
        n->u.assign.rhs = clone_expr(se, e->u.assign.rhs, tps, args);
        break;
    case EX_BINARY:
        n->u.binary.op = e->u.binary.op;
        n->u.binary.lhs = clone_expr(se, e->u.binary.lhs, tps, args);
        n->u.binary.rhs = clone_expr(se, e->u.binary.rhs, tps, args);
        break;
    case EX_UNARY:
        n->u.unary.op = e->u.unary.op;
        n->u.unary.e = clone_expr(se, e->u.unary.e, tps, args);
        break;
    case EX_CALL:
        n->u.call.callee = clone_expr(se, e->u.call.callee, tps, args);
        for (size_t i = 0; i < e->u.call.args.len; i++)
            da_push(&n->u.call.args,
                    clone_expr(se, e->u.call.args.items[i], tps, args));
        break;
    case EX_INDEX:
        n->u.index.base = clone_expr(se, e->u.index.base, tps, args);
        n->u.index.index = clone_expr(se, e->u.index.index, tps, args);
        break;
    case EX_CAST:
        n->u.cast.e = clone_expr(se, e->u.cast.e, tps, args);
        n->u.cast.t = subst_type(se, e->u.cast.t, tps, args);
        break;
    case EX_MATCH:
        n->u.match.scrutinee =
            clone_expr(se, e->u.match.scrutinee, tps, args);
        for (size_t i = 0; i < e->u.match.arms.len; i++) {
            MatchArm a = clone_arm(se, &e->u.match.arms.items[i], tps,
                                   args);
            da_push(&n->u.match.arms, a);
        }
        break;
    case EX_TRY: n->u.tryy = clone_expr(se, e->u.tryy, tps, args); break;
    case EX_UNWRAP:
        n->u.unwrap = clone_expr(se, e->u.unwrap, tps, args);
        break;
    case EX_COERCE:
        n->u.coerce.e = clone_expr(se, e->u.coerce.e, tps, args);
        n->u.coerce.target = subst_type(se, e->u.coerce.target, tps, args);
        break;
    case EX_GENERIC:
        n->u.generic.base = clone_expr(se, e->u.generic.base, tps, args);
        for (size_t i = 0; i < e->u.generic.targs.len; i++)
            da_push(&n->u.generic.targs,
                    subst_type(se, e->u.generic.targs.items[i], tps, args));
        break;
    case EX_DEREF: n->u.deref = clone_expr(se, e->u.deref, tps, args); break;
    case EX_FIELD:
        n->u.field.base = clone_expr(se, e->u.field.base, tps, args);
        n->u.field.name = e->u.field.name;
        break;
    case EX_STRUCT_LIT:
        n->u.stlit.name = e->u.stlit.name;
        for (size_t i = 0; i < e->u.stlit.targs.len; i++)
            da_push(&n->u.stlit.targs,
                    subst_type(se, e->u.stlit.targs.items[i], tps, args));
        for (size_t i = 0; i < e->u.stlit.fields.len; i++) {
            FieldInit fi;
            fi.name = e->u.stlit.fields.items[i].name;
            fi.span = e->u.stlit.fields.items[i].span;
            fi.val = clone_expr(se, e->u.stlit.fields.items[i].val, tps,
                                args);
            da_push(&n->u.stlit.fields, fi);
        }
        break;
    default:
        break;
    }
    return n;
}

static Sym *instantiate_fn_from(Sema *se, Sym *sym, StrList *tps,
                               TypeList *args, const char *key)
{
    const char *keyi = str_intern_c(key);
    Decl *existing = find_decl_by_name(se, keyi);
    if (existing)
        return scope_find(se->scope, keyi);
    Decl *d = decl_new(DL_FN, sym->decl->span);
    d->u.fn.name = keyi;
    for (size_t i = 0; i < sym->decl->u.fn.params.len; i++) {
        Param *pm = &sym->decl->u.fn.params.items[i];
        Param np;
        np.name = pm->name;
        np.span = pm->span;
        np.ty = subst_type(se, pm->ty, tps, args);
        da_push(&d->u.fn.params, np);
    }
    d->u.fn.ret = subst_type(se, sym->decl->u.fn.ret, tps, args);
    d->u.fn.body = clone_block(se, sym->decl->u.fn.body, tps, args);
    da_push(&se->prog->decls, d);
    register_decl(se, d);
    check_fn(se, d);
    return scope_find(se->scope, keyi);
}

static Sym *instantiate_fn(Sema *se, const char *fname, TypeList *args,
                           Span span)
{
    Sym *sym = scope_find(se->scope, fname);
    if (!sym || sym->kind != SYM_FN || !sym->is_template || !sym->decl)
        fatal_at(span, "'%s' is not a generic function", fname);
    StrList *tps = &sym->decl->u.fn.tparams;
    if (tps->len != args->len)
        fatal_at(span, "'%s' expects %lu type argument(s), got %lu", fname,
                 (unsigned long)tps->len, (unsigned long)args->len);
    char *key = inst_key(fname, args);
    return instantiate_fn_from(se, sym, tps, args, key);
}

/* Receiver args follow the struct template's tparam order, so the
 * placeholder -> concrete mapping is purely positional. */
static Sym *instantiate_method(Sema *se, Sym *m, const char *key,
                              const char *tmpl_sname, TypeList *inst_args)
{
    Decl *td = NULL;
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind == DL_TYPE && d->u.type.name == tmpl_sname
            && d->u.type.tparams.len > 0)
            td = d;
    }
    if (!td)
        fatal("unknown generic struct '%s'", tmpl_sname);
    if (td->u.type.tparams.len != inst_args->len)
        fatal("'%s' instantiation arity mismatch", tmpl_sname);
    TypeList margs = {0};
    for (size_t i = 0; i < m->decl->u.fn.tparams.len; i++) {
        const char *want = m->decl->u.fn.tparams.items[i];
        int found = 0;
        for (size_t j = 0; j < td->u.type.tparams.len; j++)
            if (td->u.type.tparams.items[j] == want) {
                da_push(&margs, inst_args->items[j]);
                found = 1;
                break;
            }
        if (!found)
            fatal("method type parameter '%s' is not bound by the receiver",
                  want);
    }
    StrList *tps = &m->decl->u.fn.tparams;
    return instantiate_fn_from(se, m, tps, &margs, key);
}

static void require_bool(Expr *cond)
{
    if (cond->type && cond->type->kind == TY_BOOL)
        return;
    fatal_at(cond->span, "condition must be bool, got %s",
             type_spelling(cond->type));
}

/* Signed/unsigned mixing and width mismatches are hard errors; users must
 * route through an explicit `as` cast. */
static void require_same(Expr *a, Expr *b, const char *op)
{
    if (is_unknown(a->type) || is_unknown(b->type))
        return;
    if (type_eq(a->type, b->type))
        return;
    fatal_at(a->span, "type mismatch for '%s': %s vs %s (use an 'as' cast)",
             op, type_spelling(a->type), type_spelling(b->type));
}

static Type *check_call_user(Sema *se, Expr *e, Sym *fn)
{
    if (e->u.call.args.len != fn->params.len)
        fatal_at(e->span, "'%s' expects %lu argument(s), got %lu", fn->name,
                 (unsigned long)fn->params.len,
                 (unsigned long)e->u.call.args.len);
    for (size_t i = 0; i < fn->params.len; i++) {
        Expr *arg = e->u.call.args.items[i];
        Type *pt = fn->params.items[i].ty;
        check_expr(se, arg, pt);
        if (pt->kind == TY_UNION) {
            e->u.call.args.items[i] = maybe_coerce(arg, pt);
            continue;
        }
        if (is_unknown(arg->type))
            continue;
        if (!type_eq(arg->type, pt))
            fatal_at(arg->span, "argument %lu of '%s': expected %s, got %s",
                     (unsigned long)(i + 1), fn->name, type_spelling(pt),
                     type_spelling(arg->type));
    }
    return fn->ret;
}

/* C call-site rule: pointer params take *T / &T / &mut T (or a str for
 * *i8 / *u8); a nullable *T must be narrowed first. */
static void check_c_arg(Sema *se, Expr *arg, Type *pt, const char *fname,
                        size_t idx)
{
    check_expr(se, arg, pt);
    Type *at = arg->type;
    if (!at || is_unknown(at))
        return;
    if (pt->kind == TY_PTR && !pt->nullable) {
        if (at->kind == TY_PTR && !at->nullable && type_eq(at->base, pt->base))
            return;
        if ((at->kind == TY_REF || at->kind == TY_MUTREF)
            && type_eq(at->base, pt->base))
            return;
        if (at->kind == TY_STR
            && (pt->base->kind == TY_I8 || pt->base->kind == TY_U8))
            return;
        if (at->kind == TY_PTR && at->nullable)
            fatal_at(arg->span,
                     "cannot pass nullable %s to '%s' parameter %lu: "
                     "narrow it via match or unwrap",
                     type_spelling(at), fname, (unsigned long)idx);
        fatal_at(arg->span, "argument %lu of '%s': expected %s, got %s",
                 (unsigned long)idx, fname, type_spelling(pt),
                 type_spelling(at));
    }
    if (pt->kind == TY_PTR && pt->nullable) {
        if (at->kind == TY_PTR && type_eq(at->base, pt->base))
            return;
        if ((at->kind == TY_REF || at->kind == TY_MUTREF)
            && type_eq(at->base, pt->base))
            return;
        if (at->kind == TY_STR
            && (pt->base->kind == TY_I8 || pt->base->kind == TY_U8))
            return;
        fatal_at(arg->span, "argument %lu of '%s': expected %s, got %s",
                 (unsigned long)idx, fname, type_spelling(pt),
                 type_spelling(at));
    }
    if (type_eq(at, pt))
        return;
    fatal_at(arg->span, "argument %lu of '%s': expected %s, got %s",
             (unsigned long)idx, fname, type_spelling(pt), type_spelling(at));
}

static Type *check_call_ns(Sema *se, Expr *e, Expr *callee)
{
    Sym *ns = scope_find(se->scope, callee->u.nident.ns);
    if (!ns)
        fatal_at(callee->span, "unknown namespace '%s'", callee->u.nident.ns);
    if (ns->kind != SYM_NS || !ns->hdr)
        fatal_at(callee->span, "'%s' is not a C namespace",
                 callee->u.nident.ns);
    CMember *fn = cimport_find(ns->hdr, callee->u.nident.name);
    if (!fn)
        fatal_at(callee->span, "unknown C symbol '%s::%s'",
                 callee->u.nident.ns, callee->u.nident.name);
    if (fn->kind != CM_FN)
        fatal_at(callee->span, "'%s::%s' is not a function",
                 callee->u.nident.ns, callee->u.nident.name);
    if (e->u.call.args.len < fn->params.len
        || (!fn->variadic && e->u.call.args.len > fn->params.len))
        fatal_at(e->span, "'%s' expects %lu argument(s)%s, got %lu",
                 fn->name, (unsigned long)fn->params.len,
                 fn->variadic ? " or more" : "",
                 (unsigned long)e->u.call.args.len);
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        Expr *arg = e->u.call.args.items[i];
        if (i < fn->params.len) {
            check_c_arg(se, arg, fn->params.items[i], fn->name, i + 1);
        } else {
            /* variadic tail: primitives, pointers, and str are admissible */
            check_expr(se, arg, NULL);
            Type *at = arg->type;
            if (at && !is_unknown(at) && !ty_is_num(at->kind)
                && at->kind != TY_STR && !is_ptrish(at)
                && at->kind != TY_CHAR && at->kind != TY_BOOL
                && at->kind != TY_UNION)
                fatal_at(arg->span, "cannot pass %s through '...'",
                         type_spelling(at));
        }
    }
    return fn->ret;
}

static Type *check_expr(Sema *se, Expr *e, Type *hint)
{
    switch (e->kind) {
    case EX_INT: {
        Type *suf = type_from_suffix(e->u.ilit.suffix);
        if (suf)
            e->type = suf;
        else if (hint && ty_is_int(hint->kind))
            e->type = hint; /* untyped literal coerces to expected width */
        else
            e->type = type_default_int();
        return e->type;
    }
    case EX_FLOAT: {
        Type *suf = type_from_suffix(e->u.flit.suffix);
        if (suf)
            e->type = suf;
        else if (hint && ty_is_float(hint->kind))
            e->type = hint;
        else
            e->type = type_default_float();
        return e->type;
    }
    case EX_STR:
        e->type = type_prim(TY_STR);
        return e->type;
    case EX_CHAR:
        e->type = type_prim(TY_CHAR);
        return e->type;
    case EX_BOOL:
        e->type = type_prim(TY_BOOL);
        return e->type;
    case EX_NULL:
        e->type = type_null();
        return e->type;
    case EX_IDENT: {
        Sym *sym = scope_find(se->scope, e->u.ident);
        if (sym && sym->kind == SYM_TYPE && sym->type
            && sym->type->kind == TY_ERR
            && sym->type->base->kind == TY_VOID) {
            // bare error name constructs a zero-sized !void error value
            e->type = sym->type;
            return e->type;
        }
        if (!sym)
            fatal_at(e->span, "undefined symbol '%s'", e->u.ident);
        if (sym->kind != SYM_VAR)
            fatal_at(e->span, "'%s' is not a variable", e->u.ident);
        e->type = sym->type;
        return e->type;
    }
    case EX_NS_IDENT: {
        Sym *ns = scope_find(se->scope, e->u.nident.ns);
        if (!ns)
            fatal_at(e->span, "unknown namespace '%s'", e->u.nident.ns);
        if (ns->kind != SYM_NS || !ns->hdr)
            fatal_at(e->span, "'%s' is not a namespace", e->u.nident.ns);
        CMember *m = cimport_find(ns->hdr, e->u.nident.name);
        if (!m)
            fatal_at(e->span, "unknown symbol '%s::%s'", e->u.nident.ns,
                     e->u.nident.name);
        if (m->kind == CM_CONST) {
            e->type = m->ret;
            return e->type;
        }
        fatal_at(e->span, "'%s::%s' is not a value", e->u.nident.ns,
                 e->u.nident.name);
        return NULL;
    }
    case EX_ASSIGN: {
        Expr *lhs = e->u.assign.lhs;
        Type *lt;
        if (lhs->kind == EX_IDENT) {
            Sym *sym = scope_find(se->scope, lhs->u.ident);
            if (!sym)
                fatal_at(lhs->span, "undefined symbol '%s'", lhs->u.ident);
            if (sym->kind != SYM_VAR)
                fatal_at(lhs->span, "'%s' is not a variable", lhs->u.ident);
            if (!sym->mut)
                fatal_at(lhs->span,
                         "cannot assign to immutable binding '%s'",
                         lhs->u.ident);
            lhs->type = sym->type;
            lt = sym->type;
        } else if (lhs->kind == EX_INDEX) {
            lt = check_expr(se, lhs, NULL);
        } else if (lhs->kind == EX_DEREF) {
            lt = check_expr(se, lhs, NULL);
        } else if (lhs->kind == EX_FIELD) {
            lt = check_expr(se, lhs, NULL);
        } else {
            fatal_at(lhs->span, "invalid assignment target");
            return NULL;
        }
        Type *rt = check_expr(se, e->u.assign.rhs, lt);
        if (!is_unknown(rt) && !is_unknown(lt) && !type_eq(lt, rt))
            fatal_at(e->span, "cannot assign %s to %s", type_spelling(rt),
                     type_spelling(lt));
        e->type = lt;
        return e->type;
    }
    case EX_BINARY: {
        TokenKind op = e->u.binary.op;
        if (op == TK_ANDAND || op == TK_OROR) {
            Type *lt = check_expr(se, e->u.binary.lhs, NULL);
            Type *rt = check_expr(se, e->u.binary.rhs, NULL);
            require_bool(e->u.binary.lhs);
            require_bool(e->u.binary.rhs);
            (void)lt; (void)rt;
            e->type = type_prim(TY_BOOL);
            return e->type;
        }
        int comparison = (op == TK_EQ || op == TK_NEQ || op == TK_LT
                          || op == TK_GT || op == TK_LE || op == TK_GE);
        Type *lt = check_expr(se, e->u.binary.lhs,
                             comparison ? NULL : hint);
        check_expr(se, e->u.binary.rhs, lt);
        if (op == TK_PLUS && (lt->kind == TY_STR
                              || e->u.binary.rhs->type->kind == TY_STR)) {
            /* str + T scripting chain: any printable T, result is str */
            if (!is_printable(lt) || !is_printable(e->u.binary.rhs->type))
                fatal_at(e->span, "cannot concatenate %s with %s",
                         type_spelling(lt),
                         type_spelling(e->u.binary.rhs->type));
            e->type = type_prim(TY_STR);
            return e->type;
        }
        require_same(e->u.binary.lhs, e->u.binary.rhs, "<binary>");
        if (comparison) {
            Type *t = e->u.binary.lhs->type;
            if (!is_unknown(t) && !ty_is_num(t->kind) && t->kind != TY_CHAR
                && !is_ptrish(t))
                fatal_at(e->span, "'%c%c' requires numeric operands",
                         op == TK_ANDAND ? '&' : ' ', ' ');
            e->type = type_prim(TY_BOOL);
        } else {
            Type *t = e->u.binary.lhs->type;
            if (!is_unknown(t) && !ty_is_num(t->kind))
                fatal_at(e->span, "arithmetic requires numeric operands");
            e->type = is_unknown(t) ? unknown() : t;
        }
        return e->type;
    }
    case EX_UNARY: {
        Type *t = check_expr(se, e->u.unary.e, hint);
        switch (e->u.unary.op) {
        case TK_MINUS:
            if (!is_unknown(t) && !ty_is_num(t->kind))
                fatal_at(e->span, "unary '-' requires a numeric operand");
            e->type = t;
            break;
        case TK_BANG:
            require_bool(e->u.unary.e);
            e->type = type_prim(TY_BOOL);
            break;
        case TK_AMP:
            /* second-class: refs live in locals/params/returns only */
            e->type = type_ref(t, e->borrow_mut);
            break;
        default:
            fatal_at(e->span, "unsupported unary operator");
        }
        return e->type;
    }
    case EX_CALL: {
        Expr *callee = e->u.call.callee;
        if (callee->kind == EX_NS_IDENT
            && zov_ns_member(se, callee->u.nident.ns,
                             callee->u.nident.name)) {
            /* module fn: rewrite to the merged global symbol */
            Expr *id = expr_new(EX_IDENT, callee->span);
            id->u.ident = callee->u.nident.name;
            e->u.call.callee = id;
            callee = id;
        }
        if (callee->kind == EX_GENERIC
            && callee->u.generic.base->kind == EX_NS_IDENT
            && zov_ns_member(se, callee->u.generic.base->u.nident.ns,
                             callee->u.generic.base->u.nident.name)) {
            Expr *id = expr_new(EX_IDENT, callee->u.generic.base->span);
            id->u.ident = callee->u.generic.base->u.nident.name;
            callee->u.generic.base = id;
        }
        if (callee->kind == EX_FIELD) {
            /* expr.method(...): receiver resolution with auto-borrowing */
            Type *bt = check_expr(se, callee->u.field.base, NULL);
            if (!bt || (bt->kind != TY_STRUCT && bt->kind != TY_REF
                        && bt->kind != TY_MUTREF))
                fatal_at(callee->span,
                         "method calls require a struct receiver");
            Type *st = bt;
            while (st && (st->kind == TY_REF || st->kind == TY_MUTREF))
                st = st->base;
            if (!st || st->kind != TY_STRUCT || !st->name)
                fatal_at(callee->span, "methods require a struct receiver");
            const char *sname = st->tmpl ? st->tmpl : st->name;
            Sym *m = find_method(se, sname, callee->u.field.name);
            if (!m)
                fatal_at(callee->span, "struct '%s' has no method '%s'",
                         sname, callee->u.field.name);
            if (m->is_template) {
                char key[512];
                snprintf(key, sizeof(key), "%s__%s", st->name,
                         callee->u.field.name);
                m = instantiate_method(se, m, key, sname, &st->inst_args);
            }
            Param *rp = &m->params.items[0];
            Expr *recv = callee->u.field.base;
            if (rp->ty->kind == TY_REF || rp->ty->kind == TY_MUTREF) {
                /* auto-borrow: &obj or &mut obj from a plain value */
                if (bt->kind == TY_REF || bt->kind == TY_MUTREF) {
                    if (!type_eq(bt, rp->ty))
                        fatal_at(recv->span,
                                 "receiver expects %s, got %s",
                                 type_spelling(rp->ty), type_spelling(bt));
                } else {
                    Expr *amp = expr_new(EX_UNARY, recv->span);
                    amp->u.unary.op = TK_AMP;
                    amp->u.unary.e = recv;
                    amp->borrow_mut = (rp->ty->kind == TY_MUTREF);
                    recv = amp;
                }
            } else if (bt->kind == TY_REF || bt->kind == TY_MUTREF) {
                /* auto-deref for by-value receivers */
                Expr *dr = expr_new(EX_DEREF, recv->span);
                dr->u.deref = recv;
                recv = dr;
            }
            ExprList nargs = {0};
            da_push(&nargs, recv);
            for (size_t i = 0; i < e->u.call.args.len; i++)
                da_push(&nargs, e->u.call.args.items[i]);
            e->u.call.args.items = nargs.items;
            e->u.call.args.len = nargs.len;
            e->u.call.args.cap = nargs.cap;
            Expr *id = expr_new(EX_IDENT, callee->span);
            id->u.ident = m->name;
            e->u.call.callee = id;
            e->type = check_call_user(se, e, m);
            return e->type;
        }
        if (callee->kind == EX_GENERIC
            && callee->u.generic.base->kind == EX_IDENT
            && zstr_eq(callee->u.generic.base->u.ident, "sizeof")
            && !scope_find(se->scope, "sizeof")) {
            if (callee->u.generic.targs.len != 1)
                fatal_at(callee->span, "sizeof[T]() takes one type argument");
            e->szarg = resolve_type(se, callee->u.generic.targs.items[0]);
            e->type = type_prim(TY_USIZE);
            return e->type;
        }
        if (callee->kind == EX_GENERIC) {
            /* swap in the specialized ident, then re-enter as a plain
             * user-fn call against the instantiated signature */
            if (callee->u.generic.base->kind != EX_IDENT)
                fatal_at(callee->span,
                         "generic callee must be a plain identifier");
            TypeList targs = {0};
            for (size_t i = 0; i < callee->u.generic.targs.len; i++)
                da_push(&targs,
                        resolve_type(se, callee->u.generic.targs.items[i]));
            Sym *inst = instantiate_fn(se, callee->u.generic.base->u.ident,
                                       &targs, callee->span);
            Expr *id = expr_new(EX_IDENT, callee->span);
            id->u.ident = inst->name;
            e->u.call.callee = id;
            e->type = check_call_user(se, e, inst);
            return e->type;
        }
        if (callee->kind == EX_IDENT) {
            Sym *fn = scope_find(se->scope, callee->u.ident);
            if (!fn) {
                if (zstr_eq(callee->u.ident, "print")
                    || zstr_eq(callee->u.ident, "println")) {
                    for (size_t i = 0; i < e->u.call.args.len; i++) {
                        Type *t = check_expr(se, e->u.call.args.items[i],
                                             NULL);
                        if (!is_printable(t))
                            fatal_at(e->u.call.args.items[i]->span,
                                     "cannot print %s", type_spelling(t));
                    }
                    e->builtin_io = 1;
                    e->type = type_prim(TY_VOID);
                    return e->type;
                }
                fatal_at(callee->span, "undefined function '%s'",
                         callee->u.ident);
            }
            if (fn->kind != SYM_FN)
                fatal_at(callee->span, "'%s' is not callable",
                         callee->u.ident);
            if (fn->is_template)
                fatal_at(callee->span,
                         "generic function '%s' requires explicit type "
                         "arguments, e.g. %s[T](...)",
                         callee->u.ident, callee->u.ident);
            e->type = check_call_user(se, e, fn);
            return e->type;
        }
        if (callee->kind == EX_NS_IDENT)
            return check_call_ns(se, e, callee);
        fatal_at(callee->span, "expression is not callable");
        return NULL;
    }
    case EX_INDEX: {
        Type *bt = check_expr(se, e->u.index.base, NULL);
        Type *it = check_expr(se, e->u.index.index, type_prim(TY_USIZE));
        if (!is_unknown(it) && !ty_is_int(it->kind))
            fatal_at(e->u.index.index->span,
                     "index must be an integer, got %s", type_spelling(it));
        if (is_unknown(bt))
            return unknown();
        if (bt->kind == TY_SLICE || bt->kind == TY_ARRAY) {
            e->type = bt->base;
            return e->type;
        }
        if (bt->kind == TY_PTR) {
            if (bt->nullable)
                fatal_at(e->span, "cannot index nullable %s; narrow it "
                                  "via match or unwrap",
                         type_spelling(bt));
            // raw pointer indexing: unchecked, for low-level containers
            e->type = bt->base;
            return e->type;
        }
        fatal_at(e->span, "cannot index %s", type_spelling(bt));
        return NULL;
    }
    case EX_CAST: {
        Type *t = check_expr(se, e->u.cast.e, e->u.cast.t);
        Type *dst = e->u.cast.t;
        if (!is_unknown(t) && !ty_is_num(t->kind) && t->kind != TY_CHAR
            && t->kind != TY_BOOL && !is_ptrish(t))
            fatal_at(e->span, "cannot cast %s", type_spelling(t));
        if (!ty_is_num(dst->kind) && dst->kind != TY_CHAR
            && dst->kind != TY_BOOL && !is_ptrish(dst))
            fatal_at(e->span, "cannot cast to %s", type_spelling(dst));
        e->type = dst;
        return e->type;
    }
    case EX_GENERIC:
        /* call/literal sites handle their own instantiation */
        fatal_at(e->span, "generic instantiation requires a call or struct "
                          "literal");
        return NULL;
    case EX_DEREF: {
        Type *t = check_expr(se, e->u.deref, NULL);
        if (t->kind == TY_PTR && t->nullable)
            fatal_at(e->span,
                     "cannot dereference nullable pointer; narrow it first");
        if (t->kind == TY_PTR || t->kind == TY_REF || t->kind == TY_MUTREF) {
            e->type = t->base;
            return e->type;
        }
        fatal_at(e->span, "cannot dereference %s", type_spelling(t));
        return NULL;
    }
    case EX_FIELD: {
        Type *t = check_expr(se, e->u.field.base, NULL);
        if (is_unknown(t))
            return unknown();
        if (t->kind == TY_STR) {
            // str exposes its fat-pointer halves as intrinsic fields
            if (zstr_eq(e->u.field.name, "data")) {
                e->type = type_ptr(type_prim(TY_U8), 0);
                return e->type;
            }
            if (zstr_eq(e->u.field.name, "len")) {
                e->type = type_prim(TY_USIZE);
                return e->type;
            }
            fatal_at(e->span, "str has no field '%s'", e->u.field.name);
        }
        /* references auto-deref for field access */
        while (t && (t->kind == TY_REF || t->kind == TY_MUTREF)) {
            e->via_ref = 1;
            t = t->base;
        }
        if (t->kind != TY_STRUCT || !t->name)
            fatal_at(e->span, "%s has no fields", type_spelling(t));
        Decl *d = find_struct_decl(se, t->name);
        if (!d)
            fatal_at(e->span, "unknown struct type '%s'", t->name);
        for (size_t i = 0; i < d->u.type.fields.len; i++)
            if (d->u.type.fields.items[i].name == e->u.field.name) {
                e->type = d->u.type.fields.items[i].ty;
                return e->type;
            }
        fatal_at(e->span, "struct '%s' has no field '%s'", t->name,
                 e->u.field.name);
        return NULL;
    }
    case EX_STRUCT_LIT: {
        Type *st = NULL;
        if (e->u.stlit.targs.len > 0) {
            TypeList targs = {0};
            for (size_t i = 0; i < e->u.stlit.targs.len; i++)
                da_push(&targs,
                        resolve_type(se, e->u.stlit.targs.items[i]));
            st = instantiate_struct(se, e->u.stlit.name, &targs);
        } else {
            Sym *sym = scope_find(se->scope, e->u.stlit.name);
            if (sym && sym->kind == SYM_TYPE && sym->type
                && sym->type->kind == TY_STRUCT && !sym->is_template)
                st = sym->type;
        }
        if (!st)
            fatal_at(e->span, "'%s' is not a struct type", e->u.stlit.name);
        Decl *d = find_struct_decl(se, st->name);
        if (!d)
            fatal_at(e->span, "unknown struct type '%s'", st->name);
        for (size_t i = 0; i < e->u.stlit.fields.len; i++) {
            FieldInit *fi = &e->u.stlit.fields.items[i];
            Type *ft = NULL;
            for (size_t j = 0; j < d->u.type.fields.len; j++)
                if (d->u.type.fields.items[j].name == fi->name)
                    ft = d->u.type.fields.items[j].ty;
            if (!ft)
                fatal_at(fi->span, "struct '%s' has no field '%s'",
                         st->name, fi->name);
            Type *it = check_expr(se, fi->val, ft);
            if (!is_unknown(it) && !type_eq(it, ft))
                fatal_at(fi->span, "field '%s' expects %s, got %s", fi->name,
                         type_spelling(ft), type_spelling(it));
        }
        for (size_t j = 0; j < d->u.type.fields.len; j++) {
            const char *fn = d->u.type.fields.items[j].name;
            int found = 0;
            for (size_t i = 0; i < e->u.stlit.fields.len; i++)
                if (e->u.stlit.fields.items[i].name == fn)
                    found = 1;
            if (!found)
                fatal_at(e->span, "missing field '%s' in '%s' literal", fn,
                         st->name);
        }
        e->type = st;
        return e->type;
    }
    case EX_MATCH: {
        Type *st = check_expr(se, e->u.match.scrutinee, NULL);
        if (st->kind == TY_UNION) {
            // exhaustiveness bitmask over deterministic member indices
            size_t cover = 0;
            int wild = 0;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                int idx = -1;
                if (a->kind == PAT_WILDCARD)
                    wild = 1;
                else if (a->kind == PAT_NULL)
                    fatal_at(a->span,
                             "'null' pattern requires a nullable pointer");
                else if (a->kind == PAT_TYPE_VAL) {
                    Sym *sym = scope_find(se->scope, a->name);
                    if (sym && sym->kind == SYM_TYPE && sym->type)
                        idx = union_member_index(st, sym->type);
                    else
                        for (size_t k = 0; k < st->members.len; k++)
                            if (st->members.items[k]->kind == TY_ERR
                                && st->members.items[k]->name == a->name)
                                idx = (int)k;
                    if (idx < 0)
                        fatal_at(a->span, "'%s' is not a variant of %s",
                                 a->name, type_spelling(st));
                } else {
                    a->ty = resolve_type(se, a->ty);
                    idx = union_member_index(st, a->ty);
                    if (idx < 0)
                        fatal_at(a->span, "%s has no variant %s",
                                 type_spelling(st), type_spelling(a->ty));
                    if (st->members.items[idx]->is_error)
                        fatal_at(a->span,
                                 "cannot bind error variant %s; match it "
                                 "by name",
                                 type_spelling(st->members.items[idx]));
                }
                if (idx >= 0) {
                    if (cover & ((size_t)1 << idx))
                        fatal_at(a->span, "duplicate match arm for %s",
                                 type_spelling(st->members.items[idx]));
                    cover |= (size_t)1 << idx;
                }
            }
            if (!wild && cover != ((size_t)1 << st->members.len) - 1)
                for (size_t k = 0; k < st->members.len; k++)
                    if (!(cover & ((size_t)1 << k)))
                        fatal_at(e->span,
                                 "non-exhaustive match: missing variant %s",
                                 type_spelling(st->members.items[k]));
            e->type = type_prim(TY_VOID);
            Type *mty = NULL;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                push_scope(se);
                int idx = -1;
                if (a->kind == PAT_TYPE_BIND) {
                    idx = union_member_index(st, a->ty);
                    Sym b;
                    memset(&b, 0, sizeof(b));
                    b.name = a->name;
                    b.kind = SYM_VAR;
                    b.span = a->span;
                    b.type = st->members.items[idx];
                    b.mut = 0;
                    scope_add(se, b);
                }
                if (a->jump) {
                    check_stmt(se, a->jump);
                } else {
                    Type *at = check_expr(se, a->val, mty ? mty : hint);
                    if (!mty)
                        mty = at;
                    else if (!is_unknown(at) && !type_eq(mty, at))
                        fatal_at(a->span, "match arm type %s differs from %s",
                                 type_spelling(at), type_spelling(mty));
                }
                pop_scope(se);
            }
            e->type = mty ? mty : type_prim(TY_VOID);
            return e->type;
        }
        if (st->kind == TY_PTR && st->nullable) {
            int has_null = 0, has_val = 0, wild = 0;
            Type *nonnull = type_ptr(st->base, 0);
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                if (a->kind == PAT_WILDCARD)
                    wild = 1;
                else if (a->kind == PAT_NULL)
                    has_null = 1;
                else if (a->kind == PAT_TYPE_BIND) {
                    a->ty = resolve_type(se, a->ty);
                    if (!type_eq(a->ty, nonnull))
                        fatal_at(a->span, "bind pattern must be %s, got %s",
                                 type_spelling(nonnull),
                                 type_spelling(a->ty));
                    has_val = 1;
                } else {
                    fatal_at(a->span, "pointer match has no named variants");
                }
            }
            if (!wild && (!has_null || !has_val))
                fatal_at(e->span,
                         "non-exhaustive match on nullable: must cover "
                         "'null' and '%s' (or use '_')",
                         type_spelling(nonnull));
            e->type = type_prim(TY_VOID);
            Type *mty = NULL;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                push_scope(se);
                if (a->kind == PAT_TYPE_BIND) {
                    Sym b;
                    memset(&b, 0, sizeof(b));
                    b.name = a->name;
                    b.kind = SYM_VAR;
                    b.span = a->span;
                    b.type = nonnull;
                    b.mut = 0;
                    scope_add(se, b);
                }
                if (a->jump) {
                    check_stmt(se, a->jump);
                } else {
                    Type *at = check_expr(se, a->val, mty ? mty : hint);
                    if (!mty)
                        mty = at;
                    else if (!is_unknown(at) && !type_eq(mty, at))
                        fatal_at(a->span, "match arm type %s differs from %s",
                                 type_spelling(at), type_spelling(mty));
                }
                pop_scope(se);
            }
            e->type = mty ? mty : type_prim(TY_VOID);
            return e->type;
        }
        fatal_at(e->span, "match requires a tagged union or nullable "
                          "pointer, got %s", type_spelling(st));
        return NULL;
    }
    case EX_TRY: {
        Type *t = check_expr(se, e->u.tryy, NULL);
        Type *rt = se->fn_ret;
        if (t->kind == TY_UNION) {
            if (union_has_error(t) && (!rt || rt->kind != TY_UNION))
                fatal_at(e->span,
                         "'?' cannot propagate: enclosing function returns %s",
                         rt ? type_spelling(rt) : "void");
            for (size_t i = 0; i < t->members.len; i++) {
                Type *m = t->members.items[i];
                if (m->is_error && rt && union_member_index(rt, m) < 0)
                    fatal_at(e->span,
                             "'?' error variant %s is not in the enclosing "
                             "return type %s", type_spelling(m),
                             type_spelling(rt));
            }
            Type *payload = union_payload(t);
            if (!payload)
                fatal_at(e->span,
                         "'?' requires exactly one non-error variant");
            e->type = payload;
            return e->type;
        }
        if (t->kind == TY_PTR && t->nullable) {
            if (!rt || rt->kind != TY_PTR || !rt->nullable
                || !type_eq(rt->base, t->base))
                fatal_at(e->span,
                         "'?' on a nullable requires the enclosing function "
                         "to return %s", type_spelling(type_ptr(t->base, 1)));
            e->type = type_ptr(t->base, 0);
            return e->type;
        }
        fatal_at(e->span, "'?' requires a tagged union or nullable pointer");
        return NULL;
    }
    case EX_UNWRAP: {
        Type *t = check_expr(se, e->u.unwrap, NULL);
        if (t->kind == TY_UNION) {
            Type *payload = union_payload(t);
            if (!payload)
                fatal_at(e->span,
                         "'!' requires exactly one non-error variant");
            e->type = payload;
            return e->type;
        }
        if (t->kind == TY_PTR && t->nullable) {
            e->type = type_ptr(t->base, 0);
            return e->type;
        }
        fatal_at(e->span, "'!' requires a tagged union or nullable pointer");
        return NULL;
    }
    case EX_COERCE:
        check_expr(se, e->u.coerce.e, NULL);
        e->type = e->u.coerce.target;
        return e->type;
    }
    fatal_at(e->span, "invalid expression");
    return NULL;
}

static void check_let(Sema *se, Stmt *s)
{
    if (s->u.let.ty)
        s->u.let.ty = resolve_type(se, s->u.let.ty);
    if (s->u.let.init == NULL)
        fatal_at(s->span, "let requires an initializer in v0.1");
    Type *it = check_expr(se, s->u.let.init, s->u.let.ty);
    if (s->u.let.ty && s->u.let.ty->kind == TY_UNION) {
        s->u.let.init = maybe_coerce(s->u.let.init, s->u.let.ty);
        it = s->u.let.init->type;
    }
    Type *declared = s->u.let.ty;
    if (!declared) {
        if (is_unknown(it))
            fatal_at(s->span,
                     "cannot infer type from extern call; "
                     "add an explicit type annotation");
        declared = it;
    } else {
        if (!is_unknown(it) && !type_eq(declared, it))
            fatal_at(s->span, "cannot bind %s to %s", type_spelling(it),
                     type_spelling(declared));
    }
    s->u.let.ty = declared; /* writeback so codegen sees the resolved type */
    Sym sym;
    memset(&sym, 0, sizeof(sym));
    sym.name = s->u.let.name;
    sym.kind = SYM_VAR;
    sym.span = s->span;
    sym.type = declared;
    sym.mut = s->u.let.mut;
    scope_add(se, sym);
}

static void check_stmt(Sema *se, Stmt *s)
{
    switch (s->kind) {
    case ST_LET:
        check_let(se, s);
        break;
    case ST_DEFER:
        check_stmt(se, s->u.defer);
        break;
    case ST_IF: {
        check_expr(se, s->u.iff.cond, NULL);
        require_bool(s->u.iff.cond);
        push_scope(se);
        check_block(se, s->u.iff.then_blk);
        pop_scope(se);
        if (s->u.iff.else_stmt) {
            if (s->u.iff.else_stmt->kind == ST_IF) {
                check_stmt(se, s->u.iff.else_stmt);
            } else {
                push_scope(se);
                check_block(se, s->u.iff.else_stmt->u.block);
                pop_scope(se);
            }
        }
        break;
    }
    case ST_FOR_RANGE: {
        Type *st = check_expr(se, s->u.forr.start, NULL);
        Type *et = check_expr(se, s->u.forr.end, st);
        if (!is_unknown(st) && !ty_is_int(st->kind))
            fatal_at(s->u.forr.start->span,
                     "range start must be an integer, got %s",
                     type_spelling(st));
        if (is_unknown(st) || is_unknown(et))
            fatal_at(s->span, "range bounds must have concrete types");
        require_same(s->u.forr.start, s->u.forr.end, "range");
        s->u.forr.ty = st;
        push_scope(se);
        Sym iv;
        memset(&iv, 0, sizeof(iv));
        iv.name = s->u.forr.name;
        iv.kind = SYM_VAR;
        iv.span = s->span;
        iv.type = st;
        iv.mut = 0;
        scope_add(se, iv);
        se->loop_depth++;
        check_block(se, s->u.forr.body);
        se->loop_depth--;
        pop_scope(se);
        break;
    }
    case ST_WHILE: {
        check_expr(se, s->u.whilee.cond, NULL);
        require_bool(s->u.whilee.cond);
        push_scope(se);
        se->loop_depth++;
        check_block(se, s->u.whilee.body);
        se->loop_depth--;
        pop_scope(se);
        break;
    }
    case ST_RETURN: {
        Type *ret = se->fn_ret;
        if (s->u.ret) {
            Type *t = check_expr(se, s->u.ret, ret);
            if (ret->kind == TY_VOID)
                fatal_at(s->span, "void function cannot return a value");
            if (ret->kind == TY_UNION)
                s->u.ret = maybe_coerce(s->u.ret, ret);
            t = s->u.ret->type;
            if (!is_unknown(t) && !type_eq(ret, t))
                fatal_at(s->span, "returning %s from a %s function",
                         type_spelling(t), type_spelling(ret));
        } else if (ret->kind != TY_VOID) {
            fatal_at(s->span, "missing return value");
        }
        break;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        if (se->loop_depth == 0)
            fatal_at(s->span, "%s outside of a loop",
                     s->kind == ST_BREAK ? "break" : "continue");
        break;
    case ST_EXPR: {
        Type *t = check_expr(se, s->u.expr, NULL);
        // #[must_use]: dropping an error-carrying union is a hard error
        if (t && (t->kind == TY_ERR
                  || (t->kind == TY_UNION && union_has_error(t))))
            fatal_at(s->span,
                     "error-typed value must be handled: use '?', 'match', "
                     "or 'let _ = ...'");
        break;
    }
    case ST_BLOCK:
        push_scope(se);
        check_block(se, s->u.block);
        pop_scope(se);
        break;
    }
}

static void check_block(Sema *se, Block *b)
{
    for (size_t i = 0; i < b->stmts.len; i++)
        check_stmt(se, b->stmts.items[i]);
}

static void register_decl(Sema *se, Decl *d)
{
    switch (d->kind) {
    case DL_USE: {
        if (d->u.use.zsyms.len > 0) {
            /* loaded .zov module: members resolve to global symbols */
            Sym sym;
            memset(&sym, 0, sizeof(sym));
            sym.name = d->u.use.alias;
            sym.kind = SYM_NS;
            sym.span = d->span;
            sym.is_zov = 1;
            sym.zsyms = d->u.use.zsyms;
            scope_add(se, sym);
            break;
        }
        if (!d->u.use.is_c_header)
            return; /* std module paths carry no symbols */
        CHeader *h = cimport_header(d->u.use.path, 1);
        if (!h)
            h = cimport_header(d->u.use.path, 0);
        if (!h)
            fatal_at(d->span, "cannot import C header '%s'", d->u.use.path);
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.use.alias;
        sym.kind = SYM_NS;
        sym.span = d->span;
        sym.hdr = h;
        scope_add(se, sym);
        break;
    }
    case DL_TYPE: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.type.name;
        sym.kind = SYM_TYPE;
        sym.span = d->span;
        sym.is_template = d->u.type.tparams.len > 0;
        sym.decl = d;
        if (d->u.type.alias_ty) {
            Type *r = resolve_type(se, d->u.type.alias_ty);
            if (r->kind == TY_ERR) {
                /* type Name = !T: clone with the name for variant identity */
                sym.type = type_err(d->u.type.name, r->base);
                sym.type->is_error = 1;
            } else {
                sym.type = r;
            }
        } else if (sym.is_template) {
            /* generic struct template: instantiated on first Type[T] use */
            sym.type = NULL;
        } else {
            for (size_t i = 0; i < d->u.type.fields.len; i++)
                d->u.type.fields.items[i].ty =
                    resolve_type(se, d->u.type.fields.items[i].ty);
            for (size_t i = 0; i < d->u.type.fields.len; i++) {
                TypeKind fk = d->u.type.fields.items[i].ty->kind;
                if (fk == TY_REF || fk == TY_MUTREF)
                    fatal_at(d->u.type.fields.items[i].span,
                             "borrowed reference cannot be stored in struct "
                             "field; use an owned value or *T");
            }
            sym.type = type_struct(d->u.type.name);
            if (d->u.type.is_resource)
                sym.type->is_move = 1;
        }
        scope_add(se, sym);
        break;
    }
    case DL_FN: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.fn.name;
        sym.kind = SYM_FN;
        sym.span = d->span;
        sym.decl = d;
        int is_method = d->u.fn.recv != NULL;
        if (is_method && !d->u.fn.sname)
            fatal_at(d->span, "method registration requires a struct name");
        if (is_method) {
            /* template methods keep their receiver unresolved until the
             * call site supplies concrete struct args */
            StrList rtps = {0};
            collect_recv_tparams(se, d->u.fn.recv->ty, &rtps);
            if (rtps.len > 0) {
                for (size_t i = 0; i < rtps.len; i++)
                    da_push(&d->u.fn.tparams, rtps.items[i]);
            } else {
                d->u.fn.recv->ty = resolve_type(se, d->u.fn.recv->ty);
            }
            /* receiver rides as the first parameter for uniform calls */
            {
                if (d->u.fn.params.len >= d->u.fn.params.cap) {
                    size_t cap = d->u.fn.params.cap
                                     ? d->u.fn.params.cap * 2 : 4;
                    Param *np = realloc(d->u.fn.params.items,
                                        cap * sizeof(Param));
                    if (!np)
                        fatal("out of memory");
                    d->u.fn.params.items = np;
                    d->u.fn.params.cap = cap;
                }
                memmove(d->u.fn.params.items + 1, d->u.fn.params.items,
                        d->u.fn.params.len * sizeof(Param));
                d->u.fn.params.items[0] = *d->u.fn.recv;
                d->u.fn.params.len++;
            }
        }
        sym.is_template = d->u.fn.tparams.len > 0;
        if (!sym.is_template && d->u.fn.tparams.len == 0 && !is_method) {
            for (size_t i = 0; i < d->u.fn.params.len; i++)
                d->u.fn.params.items[i].ty =
                    resolve_type(se, d->u.fn.params.items[i].ty);
            d->u.fn.ret = resolve_type(se, d->u.fn.ret);
        } else if (!sym.is_template && is_method) {
            /* non-template method: params (incl. recv) resolve now, but
             * the ret may reference the struct only */
            for (size_t i = 0; i < d->u.fn.params.len; i++)
                d->u.fn.params.items[i].ty =
                    resolve_type(se, d->u.fn.params.items[i].ty);
            d->u.fn.ret = resolve_type(se, d->u.fn.ret);
        }
        if (!sym.is_template) {
            sym.params.items = d->u.fn.params.items;
            sym.params.len = d->u.fn.params.len;
            sym.params.cap = d->u.fn.params.cap;
            sym.ret = d->u.fn.ret;
        }
        scope_add(se, sym);
        Sym *reg = scope_find(se->scope, sym.name);
        if (is_method) {
            Meth m;
            m.sname = d->u.fn.sname;
            m.mname = d->u.fn.mname;
            m.sym = reg;
            da_push(&se->methods, m);
        }
        if (zstr_eq(d->u.fn.name, "main")) {
            se->has_main = 1;
            if (d->u.fn.params.len != 0)
                fatal_at(d->span, "'main' must take no parameters");
        }
        break;
    }
    }
}

static void check_fn(Sema *se, Decl *d)
{
    push_scope(se);
    for (size_t i = 0; i < d->u.fn.params.len; i++) {
        Param *pm = &d->u.fn.params.items[i];
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = pm->name;
        sym.kind = SYM_VAR;
        sym.span = pm->span;
        sym.type = pm->ty;
        sym.mut = 0; /* parameters are immutable in v0.1 */
        scope_add(se, sym);
    }
    /* instantiation can nest check_fn while an outer body is mid-check */
    Type *prev_ret = se->fn_ret;
    int prev_loop = se->loop_depth;
    se->loop_depth = 0;
    se->fn_ret = d->u.fn.ret;
    check_block(se, d->u.fn.body);
    se->fn_ret = prev_ret;
    se->loop_depth = prev_loop;
    pop_scope(se);
}

void sema_check(Program *prog)
{
    Sema *se = zalloc(sizeof(Sema));
    se->prog = prog;
    push_scope(se); /* global scope */
    for (size_t i = 0; i < prog->decls.len; i++)
        register_decl(se, prog->decls.items[i]);
    if (!se->has_main)
        fatal("no 'fn main' found in program");
    /* is_move fixpoint: a struct is linear if it is a resource or has a
     * linear field; iterate until the mark stops propagating */
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < prog->decls.len; i++) {
            Decl *d = prog->decls.items[i];
            if (d->kind != DL_TYPE || d->u.type.alias_ty
                || d->u.type.tparams.len > 0)
                continue;
            for (size_t j = 0; j < d->u.type.fields.len; j++) {
                Type *ft = d->u.type.fields.items[j].ty;
                if (ft->kind == TY_MUTREF
                    || (ft->kind == TY_STRUCT && ft->is_move)) {
                    Type *st = type_struct(d->u.type.name);
                    if (!st->is_move) {
                        st->is_move = 1;
                        changed = 1;
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_FN && d->u.fn.tparams.len == 0)
            check_fn(se, d);
    }
}
