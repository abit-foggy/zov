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

typedef enum SymKind { SYM_VAR, SYM_FN, SYM_NS, SYM_TYPE } SymKind;

typedef struct Sym {
    const char *name;
    SymKind kind;
    Span span;
    Type *type;      /* variable type or struct type */
    int mut;
    DA(Param) params; /* function signature */
    Type *ret;
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
} Sema;

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
        check_expr(se, arg, fn->params.items[i].ty);
        if (is_unknown(arg->type))
            continue;
        if (!type_eq(arg->type, fn->params.items[i].ty))
            fatal_at(arg->span, "argument %lu of '%s': expected %s, got %s",
                     (unsigned long)(i + 1), fn->name,
                     type_spelling(fn->params.items[i].ty),
                     type_spelling(arg->type));
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
        if (ns->kind != SYM_NS)
            fatal_at(e->span, "'%s' is not a namespace", e->u.nident.ns);
        e->type = unknown();
        return e->type;
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
            /* shared borrow; &mut T on an immutable binding is rejected */
            e->type = type_ref(t, 0);
            if (e->u.unary.e->kind == EX_IDENT) {
                Sym *sym = scope_find(se->scope, e->u.unary.e->u.ident);
                if (sym && !sym->mut)
                    break;
            }
            break;
        default:
            fatal_at(e->span, "unsupported unary operator");
        }
        return e->type;
    }
    case EX_CALL: {
        Expr *callee = e->u.call.callee;
        if (callee->kind == EX_IDENT) {
            Sym *fn = scope_find(se->scope, callee->u.ident);
            if (!fn)
                fatal_at(callee->span, "undefined function '%s'",
                         callee->u.ident);
            if (fn->kind != SYM_FN)
                fatal_at(callee->span, "'%s' is not callable",
                         callee->u.ident);
            e->type = check_call_user(se, e, fn);
            return e->type;
        }
        if (callee->kind == EX_NS_IDENT) {
            /* C namespace passthrough: signature comes from the header */
            check_expr(se, callee, NULL);
            for (size_t i = 0; i < e->u.call.args.len; i++)
                check_expr(se, e->u.call.args.items[i], NULL);
            e->type = unknown();
            return e->type;
        }
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
        fatal_at(e->span, "generic instantiation is not supported in v0.1");
        return NULL;
    }
    fatal_at(e->span, "invalid expression");
    return NULL;
}

static void check_let(Sema *se, Stmt *s)
{
    if (s->u.let.init == NULL)
        fatal_at(s->span, "let requires an initializer in v0.1");
    Type *it = check_expr(se, s->u.let.init, s->u.let.ty);
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
    case ST_EXPR:
        check_expr(se, s->u.expr, NULL);
        break;
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
        if (!d->u.use.is_c_header)
            return; /* std module paths carry no symbols in v0.1 */
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.use.alias;
        sym.kind = SYM_NS;
        sym.span = d->span;
        scope_add(se, sym);
        break;
    }
    case DL_TYPE: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.type.name;
        sym.kind = SYM_TYPE;
        sym.span = d->span;
        sym.type = type_struct(d->u.type.name);
        scope_add(se, sym);
        break;
    }
    case DL_FN: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.fn.name;
        sym.kind = SYM_FN;
        sym.span = d->span;
        sym.params.items = d->u.fn.params.items;
        sym.params.len = d->u.fn.params.len;
        sym.params.cap = d->u.fn.params.cap;
        sym.ret = d->u.fn.ret;
        scope_add(se, sym);
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
    se->fn_ret = d->u.fn.ret;
    check_block(se, d->u.fn.body);
    se->fn_ret = NULL;
    pop_scope(se);
}

void sema_check(Program *prog)
{
    Sema *se = zalloc(sizeof(Sema));
    push_scope(se); /* global scope */
    for (size_t i = 0; i < prog->decls.len; i++)
        register_decl(se, prog->decls.items[i]);
    if (!se->has_main)
        fatal("no 'fn main' found in program");
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_FN)
            check_fn(se, d);
    }
}
