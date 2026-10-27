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

#include "borrow.h"
#include "diag.h"

typedef struct BVar {
    const char *name;
    Type *ty;
    int mut;
    int moved;
    int is_param;
    int defer_used;
} BVar;

typedef struct Loan {
    const char *holder; /* ref binding owning the loan */
    const char *root;  /* borrowed variable */
    int exclusive;      /* &mut */
    int active;
    int last_use; /* statement position; dead once pos > last_use */
    Span origin;  /* source site where the loan was created */
} Loan;

typedef struct Borrow {
    Program *prog;
    DA(BVar) vars;
    DA(Loan) loans;
    int pos;
    DA(struct UseRec) uses; /* pre-scanned last-use position per name */
} Borrow;

typedef struct UseRec {
    const char *name;
    int pos;
} UseRec;

static void bfatal(Span sp, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    Diagnostic d;
    memset(&d, 0, sizeof(d));
    d.severity = DIAG_ERR;
    d.message = msg;
    d.primary.file = sp.file;
    d.primary.line = (int)sp.line;
    d.primary.col = (int)sp.col;
    d.primary.len = (int)(sp.end > sp.start ? sp.end - sp.start : 1);
    d.primary.src_ptr = sp.start;
    diag_emit(&d);
    exit(1);
}

static BVar *var_find(Borrow *b, const char *name)
{
    for (size_t i = b->vars.len; i > 0; i--)
        if (b->vars.items[i - 1].name == name)
            return &b->vars.items[i - 1];
    return NULL;
}

static BVar *var_add(Borrow *b, const char *name, Type *ty, int mut)
{
    BVar v;
    memset(&v, 0, sizeof(v));
    v.name = name;
    v.ty = ty;
    v.mut = mut;
    da_push(&b->vars, v);
    return &b->vars.items[b->vars.len - 1];
}

static int root_shared(Borrow *b, const char *root)
{
    int n = 0;
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].active && !b->loans.items[i].exclusive
            && b->loans.items[i].root == root)
            n++;
    return n;
}

static Loan *root_locked(Borrow *b, const char *root)
{
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].active && b->loans.items[i].exclusive
            && b->loans.items[i].root == root)
            return &b->loans.items[i];
    return NULL;
}

static Loan *holder_loan(Borrow *b, const char *holder)
{
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].active && b->loans.items[i].holder == holder)
            return &b->loans.items[i];
    return NULL;
}

static Loan *root_loan(Borrow *b, const char *root)
{
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].active && b->loans.items[i].root == root)
            return &b->loans.items[i];
    return NULL;
}

/* Linear types: resource structs, composites over resource fields, unions
 * carrying them, and &mut T bindings. Depth cap guards cyclic placeholders. */
static int ty_is_move(Borrow *b, Type *t, int depth);

static int struct_is_move(Borrow *b, const char *name, int depth)
{
    if (depth > 8)
        return 0;
    for (size_t i = 0; i < b->prog->decls.len; i++) {
        Decl *d = b->prog->decls.items[i];
        if (d->kind != DL_TYPE || d->u.type.alias_ty
            || d->u.type.tparams.len > 0 || d->u.type.name != name)
            continue;
        if (d->u.type.is_resource)
            return 1;
        for (size_t j = 0; j < d->u.type.fields.len; j++)
            if (ty_is_move(b, d->u.type.fields.items[j].ty, depth + 1))
                return 1;
        return 0;
    }
    return 0;
}

static int ty_is_move(Borrow *b, Type *t, int depth)
{
    if (!t)
        return 0;
    switch (t->kind) {
    case TY_MUTREF:
        return 1;
    case TY_STRUCT:
        return t->name ? struct_is_move(b, t->name, depth) : 0;
    case TY_UNION:
        for (size_t i = 0; i < t->members.len; i++)
            if (ty_is_move(b, t->members.items[i], depth + 1))
                return 1;
        return 0;
    default:
        return 0;
    }
}

static int is_ref_binding(Borrow *b, const char *name)
{
    BVar *v = var_find(b, name);
    return v && (v->ty->kind == TY_REF || v->ty->kind == TY_MUTREF);
}

/* Root variable of &expr / *expr / field chains; NULL means untracked
 * (through parameters, derefs of references, imported values). */
static const char *resolve_root(Borrow *b, Expr *e)
{
    while (e) {
        switch (e->kind) {
        case EX_FIELD:
            e = e->u.field.base;
            continue;
        case EX_DEREF:
            e = e->u.deref;
            continue;
        case EX_INDEX:
            e = e->u.index.base;
            continue;
        case EX_IDENT:
            if (is_ref_binding(b, e->u.ident))
                return NULL;
            return var_find(b, e->u.ident) ? e->u.ident : NULL;
        default:
            return NULL;
        }
    }
    return NULL;
}

/* ---- last-use pre-scan (NLL release points) ---- */

static void note_use(Borrow *b, Expr *e);
static void note_use_range(Borrow *b, Stmt *s);

static void note_use_stmt(Borrow *b, Stmt *s)
{
    b->pos++;
    switch (s->kind) {
    case ST_LET: note_use(b, s->u.let.init); return;
    case ST_DEFER: note_use_stmt(b, s->u.defer); return;
    case ST_IF:
        note_use(b, s->u.iff.cond);
        for (size_t i = 0; i < s->u.iff.then_blk->stmts.len; i++)
            note_use_stmt(b, s->u.iff.then_blk->stmts.items[i]);
        if (s->u.iff.else_stmt) {
            if (s->u.iff.else_stmt->kind == ST_IF)
                note_use_stmt(b, s->u.iff.else_stmt);
            else
                for (size_t i = 0; i < s->u.iff.else_stmt->u.block->stmts.len; i++)
                    note_use_stmt(b, s->u.iff.else_stmt->u.block->stmts.items[i]);
        }
        return;
    case ST_WHILE:
        note_use(b, s->u.whilee.cond);
        for (size_t i = 0; i < s->u.whilee.body->stmts.len; i++)
            note_use_stmt(b, s->u.whilee.body->stmts.items[i]);
        return;
    case ST_FOR_RANGE:
        note_use_range(b, s);
        return;
    case ST_FOR_EACH:
        note_use(b, s->u.foreach.iter);
        for (size_t i = 0; i < s->u.foreach.body->stmts.len; i++)
            note_use_stmt(b, s->u.foreach.body->stmts.items[i]);
        return;
    case ST_RETURN: note_use(b, s->u.ret); return;
    case ST_EXPR: note_use(b, s->u.expr); return;
    case ST_BLOCK:
        for (size_t i = 0; i < s->u.block->stmts.len; i++)
            note_use_stmt(b, s->u.block->stmts.items[i]);
        return;
    default: return;
    }
}

static void note_use(Borrow *b, Expr *e)
{
    if (!e)
        return;
    switch (e->kind) {
    case EX_IDENT: {
        for (size_t i = 0; i < b->uses.len; i++)
            if (b->uses.items[i].name == e->u.ident) {
                if (b->pos > b->uses.items[i].pos)
                    b->uses.items[i].pos = b->pos;
                return;
            }
        UseRec u;
        u.name = e->u.ident;
        u.pos = b->pos;
        da_push(&b->uses, u);
        return;
    }
    case EX_UNARY: note_use(b, e->u.unary.e); return;
    case EX_DEREF: note_use(b, e->u.deref); return;
    case EX_FIELD: note_use(b, e->u.field.base); return;
    case EX_BINARY:
        note_use(b, e->u.binary.lhs);
        note_use(b, e->u.binary.rhs);
        return;
    case EX_ASSIGN:
        note_use(b, e->u.assign.lhs);
        note_use(b, e->u.assign.rhs);
        return;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            note_use(b, e->u.call.args.items[i]);
        return;
    case EX_INDEX:
        note_use(b, e->u.index.base);
        note_use(b, e->u.index.index);
        return;
    case EX_SLICE:
        note_use(b, e->u.slice.base);
        note_use(b, e->u.slice.start);
        note_use(b, e->u.slice.end);
        return;
    case EX_ORELSE:
        note_use(b, e->u.orelse.lhs);
        note_use(b, e->u.orelse.val);
        if (e->u.orelse.jump && e->u.orelse.jump->kind == ST_RETURN)
            note_use(b, e->u.orelse.jump->u.ret);
        return;
    case EX_ARR_LIT:
        for (size_t i = 0; i < e->u.elems.len; i++)
            note_use(b, e->u.elems.items[i]);
        return;
    case EX_CAST: note_use(b, e->u.cast.e); return;
    case EX_STRUCT_LIT:
        for (size_t i = 0; i < e->u.stlit.fields.len; i++)
            note_use(b, e->u.stlit.fields.items[i].val);
        return;
    case EX_MATCH:
        note_use(b, e->u.match.scrutinee);
        for (size_t i = 0; i < e->u.match.arms.len; i++) {
            MatchArm *a = &e->u.match.arms.items[i];
            if (a->jump) {
                if (a->jump->kind == ST_BLOCK) {
                    for (size_t j = 0; j < a->jump->u.block->stmts.len; j++)
                        note_use_stmt(b, a->jump->u.block->stmts.items[j]);
                }
            } else {
                note_use(b, a->val);
            }
        }
        return;
    case EX_TRY: note_use(b, e->u.tryy); return;
    case EX_UNWRAP: note_use(b, e->u.unwrap); return;
    case EX_COERCE: note_use(b, e->u.coerce.e); return;
    default: return;
    }
}

static int recorded_last_use(Borrow *b, const char *name)
{
    for (size_t i = 0; i < b->uses.len; i++)
        if (b->uses.items[i].name == name)
            return b->uses.items[i].pos;
    return b->pos;
}

/* ---- expression walk ---- */

typedef struct TempLoan {
    const char *root;
    int exclusive;
    Span span;
} TempLoan;

static void bx_expr(Borrow *b, Expr *e);
static void check_root_read(Borrow *b, Expr *e, const char *root);

/* Walk a borrow's operand: the root is lent, not read; nested index
 * expressions still move/consume their own operands. */
static void bx_borrow_operand(Borrow *b, Expr *e)
{
    while (e) {
        if (e->kind == EX_FIELD) {
            e = e->u.field.base;
            continue;
        }
        if (e->kind == EX_DEREF) {
            e = e->u.deref;
            continue;
        }
        break;
    }
    if (e && e->kind == EX_IDENT) {
        BVar *bv = var_find(b, e->u.ident);
        if (bv && bv->ty->kind != TY_REF && bv->ty->kind != TY_MUTREF
            && ty_is_move(b, bv->ty, 0)) {
            if (bv->moved)
                bfatal(e->span, "cannot borrow '%s': value was moved",
                       bv->name);
            return; /* lending is not a move */
        }
        check_root_read(b, e, e->u.ident);
        return;
    }
    /* non-ident bases (calls, literals, indexing) evaluate normally */
    bx_expr(b, e);
}

static void check_borrow(Borrow *b, Expr *e, int borrow_mut)
{
    const char *root = resolve_root(b, e->u.unary.e);
    if (!root)
        return;
    BVar *bv = var_find(b, root);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "cannot borrow '%s': value was moved", root);
    Loan *lk = root_locked(b, root);
    if (borrow_mut) {
        if (!bv->mut)
            bfatal(e->span,
                   "'&mut %s' requires a mutable binding (let mut %s)",
                   root, root);
        if (lk)
            bfatal(e->span, "'%s' is already mutably borrowed by '%s'",
                   root, lk->holder);
        if (root_shared(b, root) > 0)
            bfatal(e->span,
                   "cannot mutably borrow '%s': shared borrow is active",
                   root);
    } else {
        if (lk)
            bfatal(e->span,
                   "cannot borrow '%s': active mutable borrow in '%s'",
                   root, lk->holder);
    }
}

static void check_root_write(Borrow *b, Expr *e, const char *root)
{
    BVar *bv = var_find(b, root);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "use of moved value '%s'", root);
    if (!bv->mut)
        bfatal(e->span, "cannot mutate immutable binding '%s'", root);
    if (root_locked(b, root))
        bfatal(e->span, "cannot mutate '%s': mutably borrowed by '%s'",
               root, root_locked(b, root)->holder);
    if (root_shared(b, root) > 0)
        bfatal(e->span, "cannot mutate '%s': shared borrow is active",
               root);
}

static void check_root_read(Borrow *b, Expr *e, const char *root)
{
    BVar *bv = var_find(b, root);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "use of moved value '%s'", root);
    Loan *lk = root_locked(b, root);
    if (lk)
        bfatal(e->span, "cannot read '%s' while mutably borrowed by '%s'",
               root, lk->holder);
}

static int is_borrowed_view(Type *t);
static Expr *peel_try_unwrap(Expr *e);
static void transfer_call_loans(Borrow *b, const char *holder, Expr *rhs);

static void bx_assign(Borrow *b, Expr *e)
{    bx_expr(b, e->u.assign.rhs);
    Expr *lhs = e->u.assign.lhs;
    if (lhs->kind == EX_IDENT) {
        BVar *bv = var_find(b, lhs->u.ident);
        if (!bv)
            return;
        if (root_locked(b, lhs->u.ident))
            bfatal(lhs->span,
                   "cannot assign to '%s': mutably borrowed by '%s'",
                   lhs->u.ident, root_locked(b, lhs->u.ident)->holder);
        if (root_shared(b, lhs->u.ident) > 0)
            bfatal(lhs->span,
                   "cannot assign to '%s': shared borrow is active",
                   lhs->u.ident);
        bv->moved = 0; /* fresh value: MOVED -> OWNED reinitialization */
        if (is_borrowed_view(bv->ty)) {
            Expr *rhs = peel_try_unwrap(e->u.assign.rhs);
            if (rhs && (rhs->kind == EX_CALL || rhs->kind == EX_SLICE)) {
                if (rhs->kind == EX_SLICE) {
                    /* re-slice: provenance follows the new base */
                    for (size_t i = 0; i < b->loans.len; i++)
                        if (b->loans.items[i].holder == lhs->u.ident)
                            b->loans.items[i].active = 0;
                    const char *root =
                        resolve_root(b, rhs->u.slice.base);
                    if (root && !root_locked(b, root)) {
                        Loan l;
                        memset(&l, 0, sizeof(l));
                        l.holder = lhs->u.ident;
                        l.root = root;
                        l.exclusive = 0;
                        l.active = 1;
                        l.last_use = recorded_last_use(b, lhs->u.ident);
                        l.origin = rhs->span;
                        da_push(&b->loans, l);
                    }
                } else {
                    transfer_call_loans(b, lhs->u.ident, rhs);
                }
            }
        }
        return;
    }
    if (lhs->kind == EX_DEREF) {
        Expr *inner = lhs->u.deref;
        if (inner->kind == EX_IDENT && is_ref_binding(b, inner->u.ident))
            return; /* write through the exclusive holder is fine */
        const char *root = resolve_root(b, lhs);
        if (root)
            check_root_write(b, lhs, root);
        return;
    }
    if (lhs->kind == EX_FIELD || lhs->kind == EX_INDEX) {
        const char *root = resolve_root(b, lhs);
        if (root) {
            BVar *bv = var_find(b, root);
            /* writing through a raw pointer mutates the pointee, not
             * the binding: no mutability requirement on the pointer */
            if (bv && bv->ty->kind == TY_PTR) {
                if (bv->moved)
                    bfatal(lhs->span, "use of moved value '%s'", root);
                return;
            }
            check_root_write(b, lhs, root);
        }
        return;
    }
}

static void bx_call(Borrow *b, Expr *e)
{
    /* print/println args are printable (hence Copy) per sema, so they
     * can never move; inline consuming calls inside args still move */
    DA(TempLoan) temps = {0};
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        Expr *arg = e->u.call.args.items[i];
        if (arg->kind == EX_UNARY && arg->u.unary.op == TK_AMP) {
            /* call-site temporary loans live exactly for this call */
            check_borrow(b, arg, arg->borrow_mut);
            TempLoan tl;
            tl.root = resolve_root(b, arg->u.unary.e);
            tl.exclusive = arg->borrow_mut;
            tl.span = arg->span;
            if (tl.root)
                da_push(&temps, tl);
            bx_borrow_operand(b, arg->u.unary.e);
            continue;
        }
        bx_expr(b, arg);
    }
    for (size_t i = 0; i < temps.len; i++)
        for (size_t j = i + 1; j < temps.len; j++)
            if (temps.items[i].root == temps.items[j].root
                && (temps.items[i].exclusive || temps.items[j].exclusive))
                bfatal(temps.items[j].span,
                       "conflicting borrows of '%s' in call arguments",
                       temps.items[i].root);
    free(temps.items);
}

/* ---- return provenance: views returned by calls borrow their args ---- */

typedef struct CallRoot {
    const char *root;
    int exclusive;
} CallRoot;

typedef DA(CallRoot) CallRootList;

/* expr? / expr! wrappers keep the underlying call for provenance */
static Expr *peel_try_unwrap(Expr *e)
{
    while (e && (e->kind == EX_TRY || e->kind == EX_UNWRAP))
        e = e->kind == EX_TRY ? e->u.tryy : e->u.unwrap;
    return e;
}

static void push_call_root(CallRootList *roots, const char *root,
                          int exclusive)
{
    if (!root)
        return;
    for (size_t i = 0; i < roots->len; i++)
        if (roots->items[i].root == root) {
            if (exclusive)
                roots->items[i].exclusive = 1;
            return;
        }
    CallRoot cr;
    cr.root = root;
    cr.exclusive = exclusive;
    da_push(roots, cr);
}

/* Roots a callee may have captured into its returned view: borrowed
 * receiver (&v / &mut v), slice arguments, and reference/slice locals
 * chained through their own holder loans. */
static void extract_call_roots(Borrow *b, Expr *call, CallRootList *roots)
{
    for (size_t i = 0; i < call->u.call.args.len; i++) {
        Expr *arg = call->u.call.args.items[i];
        if (arg->kind == EX_UNARY && arg->u.unary.op == TK_AMP) {
            push_call_root(roots, resolve_root(b, arg->u.unary.e),
                           arg->borrow_mut);
            continue;
        }
        if (arg->kind == EX_SLICE) {
            push_call_root(roots, resolve_root(b, arg->u.slice.base), 0);
            continue;
        }
        if (arg->kind == EX_IDENT) {
            BVar *bv = var_find(b, arg->u.ident);
            if (!bv || (bv->ty->kind != TY_REF
                        && bv->ty->kind != TY_MUTREF
                        && bv->ty->kind != TY_SLICE))
                continue;
            Loan *l = holder_loan(b, arg->u.ident);
            if (l)
                push_call_root(roots, l->root, l->exclusive);
        }
    }
}

static int is_borrowed_view(Type *t)
{
    /* raw pointers are owned; str stays exempt for ergonomics */
    return t && (t->kind == TY_REF || t->kind == TY_MUTREF
                 || t->kind == TY_SLICE);
}

/* Rebind a borrowed-view binding's loans onto roots borrowed by the
 * call it is assigned from; earlier holder loans die with the rebind. */
static void transfer_call_loans(Borrow *b, const char *holder,
                                Expr *rhs)
{
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].holder == holder)
            b->loans.items[i].active = 0;
    Expr *call = peel_try_unwrap(rhs);
    if (!call || call->kind != EX_CALL)
        return;
    CallRootList roots = {0};
    extract_call_roots(b, call, &roots);
    for (size_t i = 0; i < roots.len; i++) {
        if (root_locked(b, roots.items[i].root))
            continue;
        Loan l;
        memset(&l, 0, sizeof(l));
        l.holder = holder;
        l.root = roots.items[i].root;
        l.exclusive = roots.items[i].exclusive;
        l.active = 1;
        l.last_use = recorded_last_use(b, holder);
        l.origin = call->span;
        da_push(&b->loans, l);
    }
    free(roots.items);
}

static void bx_expr(Borrow *b, Expr *e)
{
    if (!e)
        return;
    switch (e->kind) {
    case EX_IDENT: {
        BVar *bv = var_find(b, e->u.ident);
        if (!bv)
            return;
        if (bv->ty->kind == TY_REF || bv->ty->kind == TY_MUTREF)
            return; /* using a reference binding is never a move */
        if (ty_is_move(b, bv->ty, 0)) {
            if (bv->moved)
                bfatal(e->span, "use of moved value '%s'", bv->name);
            if (bv->defer_used)
                bfatal(e->span,
                       "cannot move '%s': a deferred cleanup still uses it",
                       bv->name);
            Loan *rl = root_loan(b, bv->name);
            if (rl && rl->origin.file) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot move '%s': it is still borrowed by '%s'",
                         bv->name, rl->holder);
                Diagnostic d;
                memset(&d, 0, sizeof(d));
                d.severity = DIAG_ERR;
                d.code = "E0502";
                d.message = msg;
                d.primary.file = e->span.file;
                d.primary.line = (int)e->span.line;
                d.primary.col = (int)e->span.col;
                d.primary.len = (int)(e->span.end > e->span.start
                                         ? e->span.end - e->span.start : 1);
                d.primary.src_ptr = e->span.start;
                d.primary.label = "move occurs here";
                d.secondary.file = rl->origin.file;
                d.secondary.line = (int)rl->origin.line;
                d.secondary.col = (int)rl->origin.col;
                d.secondary.len = (int)(rl->origin.end > rl->origin.start
                                          ? rl->origin.end - rl->origin.start
                                          : 1);
                d.secondary.src_ptr = rl->origin.start;
                d.secondary.label = "borrow of this value is still active";
                diag_emit(&d);
                exit(1);
            }
            if (rl)
                bfatal(e->span,
                       "cannot move '%s': it is still borrowed by '%s'",
                       bv->name, rl->holder);
            bv->moved = 1;
            return;
        }
        check_root_read(b, e, e->u.ident);
        return;
    }
    case EX_UNARY:
        if (e->u.unary.op == TK_AMP) {
            check_borrow(b, e, e->borrow_mut);
            bx_borrow_operand(b, e->u.unary.e);
            return;
        }
        bx_expr(b, e->u.unary.e);
        return;
    case EX_DEREF: {
        Expr *inner = e->u.deref;
        if (inner->kind == EX_IDENT && is_ref_binding(b, inner->u.ident))
            return;
        bx_expr(b, inner);
        const char *root = resolve_root(b, e);
        if (root)
            check_root_read(b, e, root);
        return;
    }
    case EX_FIELD: {
        const char *root = resolve_root(b, e);
        if (root) {
            /* reading a field is a use of the root, never a move of it */
            check_root_read(b, e, root);
            Expr *base = e->u.field.base;
            while (base && base->kind == EX_FIELD)
                base = base->u.field.base;
            if (base && base->kind == EX_DEREF)
                base = base->u.deref;
            if (base && base->kind != EX_IDENT)
                bx_expr(b, base);
            return;
        }
        bx_expr(b, e->u.field.base);
        return;
    }
    case EX_ASSIGN:
        bx_assign(b, e);
        return;
    case EX_CALL:
        bx_call(b, e);
        return;
    case EX_BINARY:
        bx_expr(b, e->u.binary.lhs);
        bx_expr(b, e->u.binary.rhs);
        return;
    case EX_INDEX:
        bx_expr(b, e->u.index.base);
        bx_expr(b, e->u.index.index);
        return;
    case EX_ARR_LIT:
        for (size_t i = 0; i < e->u.elems.len; i++)
            bx_expr(b, e->u.elems.items[i]);
        return;
    case EX_ORELSE:
        bx_expr(b, e->u.orelse.lhs);
        bx_expr(b, e->u.orelse.val);
        if (e->u.orelse.jump && e->u.orelse.jump->kind == ST_RETURN)
            bx_expr(b, e->u.orelse.jump->u.ret);
        return;
    case EX_SLICE: {
        /* slicing takes a shared read of the root for sub's lifetime */
        const char *root = resolve_root(b, e->u.slice.base);
        if (root)
            check_root_read(b, e, root);
        bx_expr(b, e->u.slice.base);
        bx_expr(b, e->u.slice.start);
        bx_expr(b, e->u.slice.end);
        return;
    }
    case EX_CAST:
        bx_expr(b, e->u.cast.e);
        return;
    case EX_STRUCT_LIT:
        for (size_t i = 0; i < e->u.stlit.fields.len; i++)
            bx_expr(b, e->u.stlit.fields.items[i].val);
        return;
    case EX_MATCH:
        bx_expr(b, e->u.match.scrutinee);
        return;
    case EX_TRY:
        bx_expr(b, e->u.tryy);
        return;
    case EX_UNWRAP:
        bx_expr(b, e->u.unwrap);
        return;
    case EX_COERCE:
        bx_expr(b, e->u.coerce.e);
        return;
    default:
        return;
    }
}

/* ---- statements ---- */

static int b_block(Borrow *b, Block *blk);

static void scan_defer_idents(Borrow *b, Expr *e)
{
    if (!e)
        return;
    switch (e->kind) {
    case EX_IDENT: {
        BVar *bv = var_find(b, e->u.ident);
        if (bv)
            bv->defer_used = 1;
        return;
    }
    case EX_UNARY: scan_defer_idents(b, e->u.unary.e); return;
    case EX_DEREF: scan_defer_idents(b, e->u.deref); return;
    case EX_FIELD: scan_defer_idents(b, e->u.field.base); return;
    case EX_BINARY:
        scan_defer_idents(b, e->u.binary.lhs);
        scan_defer_idents(b, e->u.binary.rhs);
        return;
    case EX_ASSIGN:
        scan_defer_idents(b, e->u.assign.lhs);
        scan_defer_idents(b, e->u.assign.rhs);
        return;
    case EX_CALL:
        for (size_t i = 0; i < e->u.call.args.len; i++)
            scan_defer_idents(b, e->u.call.args.items[i]);
        return;
    case EX_INDEX:
        scan_defer_idents(b, e->u.index.base);
        scan_defer_idents(b, e->u.index.index);
        return;
    default: return;
    }
}

static void scan_defer_stmt(Borrow *b, Stmt *s);

static void scan_defer_block(Borrow *b, Block *blk)
{
    for (size_t i = 0; i < blk->stmts.len; i++)
        scan_defer_stmt(b, blk->stmts.items[i]);
}

static void scan_defer_stmt(Borrow *b, Stmt *s)
{
    switch (s->kind) {
    case ST_EXPR: scan_defer_idents(b, s->u.expr); break;
    case ST_DEFER: scan_defer_stmt(b, s->u.defer); break;
    case ST_BLOCK: scan_defer_block(b, s->u.block); break;
    case ST_LET: scan_defer_idents(b, s->u.let.init); break;
    case ST_IF:
        scan_defer_idents(b, s->u.iff.cond);
        scan_defer_block(b, s->u.iff.then_blk);
        if (s->u.iff.else_stmt) {
            if (s->u.iff.else_stmt->kind == ST_IF)
                scan_defer_stmt(b, s->u.iff.else_stmt);
            else
                scan_defer_block(b, s->u.iff.else_stmt->u.block);
        }
        break;
    case ST_WHILE:
        scan_defer_idents(b, s->u.whilee.cond);
        scan_defer_block(b, s->u.whilee.body);
        break;
    case ST_RETURN: scan_defer_idents(b, s->u.ret); break;
    default: break;
    }
}

/* Branch joins: walk with a copied state, then OR the branch's MOVED /
 * defer_used marks back into the running state. Branch-local bindings
 * and loans are dropped. A diverged branch contributes no marks. */
static void merge_branch_marks(Borrow *b, BVar *base, size_t nbase,
                               int diverged)
{
    for (size_t i = 0; i < nbase && i < b->vars.len; i++) {
        if (b->vars.items[i].moved && !diverged)
            base[i].moved = 1;
        if (b->vars.items[i].defer_used)
            base[i].defer_used = 1;
    }
}

static int walk_branch(Borrow *b, Block *body)
{
    size_t nvars = b->vars.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    int diverged = b_block(b, body);
    merge_branch_marks(b, base, nvars, diverged);
    b->vars.len = nvars;
    for (size_t i = 0; i < nvars; i++)
        b->vars.items[i] = base[i];
    free(base);
    return diverged;
}

static int walk_arm(Borrow *b, MatchArm *a)
{
    size_t nvars = b->vars.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    if (a->name && a->ty)
        var_add(b, a->name, a->ty, 0);
    int diverged = 0;
    if (a->jump) {
        if (a->jump->kind == ST_BLOCK)
            diverged = b_block(b, a->jump->u.block);
        else
            diverged = 1; /* return/break/continue leave the construct */
    } else {
        bx_expr(b, a->val);
    }
    merge_branch_marks(b, base, nvars, diverged);
    b->vars.len = nvars;
    for (size_t i = 0; i < nvars; i++)
        b->vars.items[i] = base[i];
    free(base);
    return diverged;
}

static void walk_match(Borrow *b, Expr *m)
{
    bx_expr(b, m->u.match.scrutinee);
    int all_diverge = 1;
    for (size_t i = 0; i < m->u.match.arms.len; i++) {
        int d = walk_arm(b, &m->u.match.arms.items[i]);
        all_diverge = all_diverge && d;
    }
    (void)all_diverge;
}

/* NLL: loans whose holder's last lexical use precedes the current
 * statement position are dead. */
static void tick(Borrow *b)
{
    b->pos++;
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].active
            && b->loans.items[i].last_use < b->pos)
            b->loans.items[i].active = 0;
}

static void note_use_range(Borrow *b, Stmt *s);

static int b_stmt(Borrow *b, Stmt *s);
static void note_use_stmt(Borrow *b, Stmt *s);
static void note_use_range(Borrow *b, Stmt *s)
{
    note_use(b, s->u.forr.start);
    note_use(b, s->u.forr.end);
    for (size_t i = 0; i < s->u.forr.body->stmts.len; i++)
        note_use_stmt(b, s->u.forr.body->stmts.items[i]);
}

static int b_foreach(Borrow *b, Stmt *s)
{
    tick(b);
    bx_expr(b, s->u.foreach.iter);
    /* the container is shared-borrowed for the whole loop lifetime */
    const char *root = resolve_root(b, s->u.foreach.iter);
    if (root && !root_locked(b, root)) {
        Loan l;
        memset(&l, 0, sizeof(l));
        l.holder = s->u.foreach.name;
        l.root = root;
        l.exclusive = 0;
        l.active = 1;
        l.last_use = 1 << 28; /* outlives every statement position */
        l.origin = s->u.foreach.iter->span;
        da_push(&b->loans, l);
    }
    size_t nvars = b->vars.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    var_add(b, s->u.foreach.name, s->u.foreach.ty, 0);
    b_block(b, s->u.foreach.body);
    for (size_t i = 0; i < nvars && i < b->vars.len; i++)
        if (!base[i].moved && b->vars.items[i].moved)
            bfatal(s->span,
                   "'%s' moved inside loop body without reinitialization",
                   base[i].name);
    b->vars.len = nvars;
    for (size_t i = 0; i < nvars; i++)
        b->vars.items[i] = base[i];
    if (root)
        for (size_t i = 0; i < b->loans.len; i++)
            if (b->loans.items[i].holder == s->u.foreach.name)
                b->loans.items[i].active = 0;
    free(base);
    return 0;
}

static int b_range(Borrow *b, Stmt *s)
{
    tick(b);
    bx_expr(b, s->u.forr.start);
    bx_expr(b, s->u.forr.end);
    size_t nvars = b->vars.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    var_add(b, s->u.forr.name, s->u.forr.ty, 0);
    b_block(b, s->u.forr.body);
    for (size_t i = 0; i < nvars && i < b->vars.len; i++)
        if (!base[i].moved && b->vars.items[i].moved)
            bfatal(s->span,
                   "'%s' moved inside loop body without reinitialization",
                   base[i].name);
    b->vars.len = nvars;
    for (size_t i = 0; i < nvars; i++)
        b->vars.items[i] = base[i];
    free(base);
    return 0;
}

static int b_stmt(Borrow *b, Stmt *s)
{
    tick(b);
    switch (s->kind) {
    case ST_LET: {
        Expr *init = s->u.let.init;
        if (!init) {
            var_add(b, s->u.let.name, s->u.let.ty, s->u.let.mut);
            return 0;
        }
        if (init->kind == EX_UNARY && init->u.unary.op == TK_AMP) {
            /* named reference binding: loan held until its last use */
            check_borrow(b, init, init->borrow_mut);
            bx_borrow_operand(b, init->u.unary.e);
            var_add(b, s->u.let.name, s->u.let.ty, s->u.let.mut);
            const char *root = resolve_root(b, init->u.unary.e);
            if (root && s->u.let.ty
                && (s->u.let.ty->kind == TY_REF
                    || s->u.let.ty->kind == TY_MUTREF)) {
                Loan l;
                memset(&l, 0, sizeof(l));
                l.holder = s->u.let.name;
                l.root = root;
                l.exclusive = init->borrow_mut;
                l.active = 1;
                l.last_use = recorded_last_use(b, s->u.let.name);
                da_push(&b->loans, l);
            }
            return 0;
        }
        bx_expr(b, init);
        if (init->kind == EX_MATCH)
            walk_match(b, init);
        BVar *bv0 = var_add(b, s->u.let.name, s->u.let.ty, s->u.let.mut);
        (void)bv0;
        if (init->kind == EX_SLICE) {
            /* provenance: sub holds a shared loan on the base root until
             * its last lexical use */
            const char *root = resolve_root(b, init->u.slice.base);
            if (root && !root_locked(b, root)) {
                Loan l;
                memset(&l, 0, sizeof(l));
                l.holder = s->u.let.name;
                l.root = root;
                l.exclusive = 0;
                l.active = 1;
                l.last_use = recorded_last_use(b, s->u.let.name);
                da_push(&b->loans, l);
            }
        }
        if (is_borrowed_view(s->u.let.ty)) {
            /* v.as_slice() and friends: the returned view borrows every
             * root the callee received, held until this binding's last
             * lexical use */
            Expr *call = peel_try_unwrap(init);
            if (call && call->kind == EX_CALL)
                transfer_call_loans(b, s->u.let.name, call);
        }
        return 0;
    }
    case ST_DEFER:
        /* deferred cleanups run at scope exit; their uses must not block
         * earlier borrows, but they pin the resource against moves */
        scan_defer_stmt(b, s->u.defer);
        return 0;
    case ST_IF: {
        bx_expr(b, s->u.iff.cond);
        int d1 = walk_branch(b, s->u.iff.then_blk);
        int d2 = 0;
        if (s->u.iff.else_stmt) {
            if (s->u.iff.else_stmt->kind == ST_IF)
                d2 = b_stmt(b, s->u.iff.else_stmt);
            else
                d2 = walk_branch(b, s->u.iff.else_stmt->u.block);
        }
        return d1 && d2;
    }
    case ST_WHILE: {
        bx_expr(b, s->u.whilee.cond);
        size_t nvars = b->vars.len;
        BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
        for (size_t i = 0; i < nvars; i++)
            base[i] = b->vars.items[i];
        b_block(b, s->u.whilee.body);
        /* moving inside a loop without reinitialization poisons the
         * next iteration */
        for (size_t i = 0; i < nvars && i < b->vars.len; i++)
            if (!base[i].moved && b->vars.items[i].moved)
                bfatal(s->span,
                       "'%s' moved inside loop body without "
                       "reinitialization", base[i].name);
        b->vars.len = nvars;
        for (size_t i = 0; i < nvars; i++)
            b->vars.items[i] = base[i];
        free(base);
        return 0;
    }
    case ST_RETURN: {
        if (s->u.ret && s->u.ret->kind == EX_UNARY
            && s->u.ret->u.unary.op == TK_AMP) {
            const char *root = resolve_root(b, s->u.ret->u.unary.e);
            if (root) {
                BVar *bv = var_find(b, root);
                if (bv && !bv->is_param)
                    bfatal(s->span,
                           "cannot return reference to local variable '%s'",
                           root);
            }
        }
        bx_expr(b, s->u.ret);
        return 1;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        return 1;
    case ST_FOR_RANGE:
        return b_range(b, s);
    case ST_FOR_EACH:
        return b_foreach(b, s);
    case ST_EXPR:
        if (s->u.expr->kind == EX_MATCH)
            walk_match(b, s->u.expr);
        else
            bx_expr(b, s->u.expr);
        return 0;
    case ST_BLOCK:
        return b_block(b, s->u.block);
    }
    return 0;
}

static int b_block(Borrow *b, Block *blk)
{
    int diverged = 0;
    for (size_t i = 0; i < blk->stmts.len; i++) {
        if (diverged)
            break; /* unreachable after a jump */
        diverged = b_stmt(b, blk->stmts.items[i]);
    }
    return diverged;
}

static void borrow_fn(Borrow *b, Decl *d)
{
    b->vars.len = 0;
    b->loans.len = 0;
    b->pos = 0;
    b->uses.len = 0;
    /* NLL pre-scan: last lexical use of every name precedes the checks */
    for (size_t i = 0; i < d->u.fn.body->stmts.len; i++)
        note_use_stmt(b, d->u.fn.body->stmts.items[i]);
    b->pos = 0;
    b->vars.len = 0;
    for (size_t i = 0; i < d->u.fn.params.len; i++) {
        Param *pm = &d->u.fn.params.items[i];
        BVar *bv = var_add(b, pm->name, pm->ty, 0);
        bv->is_param = 1;
    }
    b_block(b, d->u.fn.body);
}

static void borrow_test(Borrow *b, Decl *d)
{
    b->vars.len = 0;
    b->loans.len = 0;
    b->pos = 0;
    b->uses.len = 0;
    for (size_t i = 0; i < d->u.test.body->stmts.len; i++)
        note_use_stmt(b, d->u.test.body->stmts.items[i]);
    b->pos = 0;
    b->vars.len = 0;
    b_block(b, d->u.test.body);
}

void borrow_check(Program *prog)
{
    Borrow *b = zalloc(sizeof(Borrow));
    b->prog = prog;
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_FN && d->u.fn.tparams.len == 0)
            borrow_fn(b, d);
        if (d->kind == DL_TEST)
            borrow_test(b, d);
    }
}
