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
    int freed;      /* String.free() already released this owner */
    int is_param;
    int defer_used;
    int defer_free; /* a deferred cleanup calls free() on this binding */
    /* Released owned fields: h.name.free() must poison later h.name uses
     * without poisoning unrelated fields. */
    const char *freed_fields[8];
    size_t nfreed_fields;
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
    int read_only; /* String operands of concat/print are borrowed, not moved */
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

static int field_freed(BVar *v, const char *field)
{
    if (!v || !field)
        return 0;
    for (size_t i = 0; i < v->nfreed_fields; i++)
        if (v->freed_fields[i] == field)
            return 1;
    return 0;
}

static void mark_field_freed(BVar *v, const char *field)
{
    if (!v || !field || field_freed(v, field))
        return;
    if (v->nfreed_fields < sizeof(v->freed_fields) / sizeof(*v->freed_fields))
        v->freed_fields[v->nfreed_fields++] = field;
}

static void clear_freed_fields(BVar *v)
{
    if (v)
        v->nfreed_fields = 0;
}

/* Outermost field of a place rooted at a binding: h.a.b -> "a". */
static const char *place_field_name(Expr *e)
{
    const char *field = NULL;
    while (e && (e->kind == EX_FIELD || e->kind == EX_INDEX)) {
        if (e->kind == EX_FIELD && e->u.field.name)
            field = e->u.field.name;
        e = e->kind == EX_FIELD ? e->u.field.base : e->u.index.base;
    }
    return field;
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
        if (t->name && zstr_eq(t->name, "String"))
            return 1; /* owned string: moving copies the handle, not the bytes */
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

/* Reference binding at the root of a place (fields/index/deref).  NULL
 * means the place is owned or rooted in raw pointer state. */
static const char *place_ref_root(Borrow *b, Expr *e)
{
    while (e) {
        switch (e->kind) {
        case EX_FIELD: e = e->u.field.base; continue;
        case EX_INDEX: e = e->u.index.base; continue;
        case EX_DEREF: e = e->u.deref; continue;
        case EX_IDENT:
            return is_ref_binding(b, e->u.ident) ? e->u.ident : NULL;
        default: return NULL;
        }
    }
    return NULL;
}

/* Reference binding reborrowed by &*r, &mut *r, or &mut r.field. */
static const char *reborrow_root(Borrow *b, Expr *e)
{
    while (e && (e->kind == EX_FIELD || e->kind == EX_INDEX))
        e = e->kind == EX_FIELD ? e->u.field.base : e->u.index.base;
    if (e && e->kind == EX_DEREF) {
        e = e->u.deref;
        while (e && (e->kind == EX_FIELD || e->kind == EX_INDEX))
            e = e->kind == EX_FIELD ? e->u.field.base : e->u.index.base;
    }
    if (e && e->kind == EX_IDENT && is_ref_binding(b, e->u.ident))
        return e->u.ident;
    return NULL;
}

static void check_ref_place_write(Borrow *b, Expr *e, const char *ref)
{
    BVar *bv = var_find(b, ref);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "use of moved value '%s'", ref);
    if (bv->ty->kind == TY_REF)
        bfatal(e->span, "cannot write through shared reference '%s'", ref);
    Loan *lk = root_locked(b, ref);
    if (lk)
        bfatal(e->span, "cannot write through '%s': mutably borrowed by '%s'",
               ref, lk->holder);
    if (root_shared(b, ref) > 0)
        bfatal(e->span,
               "cannot write through '%s': shared borrow is active", ref);
}

static void check_ref_place_read(Borrow *b, Expr *e, const char *ref)
{
    BVar *bv = var_find(b, ref);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "use of moved value '%s'", ref);
    Loan *lk = root_locked(b, ref);
    if (lk)
        bfatal(e->span, "cannot read '%s' while mutably borrowed by '%s'",
               ref, lk->holder);
}

/* Owned values with borrowed elements (str, slices, Vec[str], fields). */
static int type_contains_view(Borrow *b, Type *t, int depth)
{
    if (!t || depth > 8)
        return 0;
    switch (t->kind) {
    case TY_STR:
    case TY_REF:
    case TY_MUTREF:
    case TY_SLICE:
        return 1;
    case TY_PTR:
    case TY_ARRAY:
        return type_contains_view(b, t->base, depth + 1);
    case TY_UNION:
        for (size_t i = 0; i < t->members.len; i++)
            if (type_contains_view(b, t->members.items[i], depth + 1))
                return 1;
        return 0;
    case TY_STRUCT: {
        if (!t->name || zstr_eq(t->name, "String"))
            return 0;
        for (size_t i = 0; i < b->prog->decls.len; i++) {
            Decl *d = b->prog->decls.items[i];
            if (d->kind != DL_TYPE || d->u.type.alias_ty
                || d->u.type.name != t->name)
                continue;
            for (size_t j = 0; j < d->u.type.fields.len; j++)
                if (type_contains_view(b, d->u.type.fields.items[j].ty,
                                       depth + 1))
                    return 1;
            return 0;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int root_is_local(Borrow *b, const char *root)
{
    BVar *v = var_find(b, root);
    return v && !v->is_param;
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
static void walk_match(Borrow *b, Expr *m);
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
    /* reborrowing through a reference binding roots the loan at the
     * reference itself; the original loan on the pointee stays intact */
    const char *ref = reborrow_root(b, e->u.unary.e);
    if (ref) {
        BVar *rv = var_find(b, ref);
        if (!rv)
            return;
        if (rv->moved)
            bfatal(e->span, "use of moved value '%s'", ref);
        if (borrow_mut && rv->ty->kind != TY_MUTREF)
            bfatal(e->span,
                   "cannot mutably borrow through shared reference '%s'",
                   ref);
        Loan *lk = root_locked(b, ref);
        if (lk)
            bfatal(e->span, "'%s' is already mutably borrowed by '%s'",
                   ref, lk->holder);
        if (borrow_mut && root_shared(b, ref) > 0)
            bfatal(e->span,
                   "cannot mutably borrow '%s': shared borrow is active",
                   ref);
        return;
    }
    const char *root = resolve_root(b, e->u.unary.e);
    if (!root)
        return;
    BVar *bv = var_find(b, root);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(e->span, "cannot borrow '%s': value was moved", root);
    if (bv->freed)
        bfatal(e->span, "cannot borrow '%s': value was freed", root);
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
    if (bv->freed)
        bfatal(e->span, "use of freed value '%s'", root);
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
    if (bv->freed)
        bfatal(e->span, "use of freed value '%s'", root);
    Loan *lk = root_locked(b, root);
    if (lk)
        bfatal(e->span, "cannot read '%s' while mutably borrowed by '%s'",
               root, lk->holder);
}

static Expr *peel_try_unwrap(Expr *e);
static void rebind_view_loans(Borrow *b, const char *holder, Expr *rhs);

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
        bv->freed = 0;
        clear_freed_fields(bv);
        if (type_contains_view(b, bv->ty, 0))
            rebind_view_loans(b, lhs->u.ident, e->u.assign.rhs);
        return;
    }
    if (lhs->kind == EX_DEREF) {
        const char *ref = place_ref_root(b, lhs);
        if (ref) {
            check_ref_place_write(b, lhs, ref);
            return;
        }
        const char *root = resolve_root(b, lhs);
        if (root)
            check_root_write(b, lhs, root);
        return;
    }
    if (lhs->kind == EX_FIELD || lhs->kind == EX_INDEX) {
        const char *ref = place_ref_root(b, lhs);
        if (ref) {
            check_ref_place_write(b, lhs, ref);
            return;
        }
        const char *root = resolve_root(b, lhs);
        if (root) {
            BVar *bv = var_find(b, root);
            if (bv) {
                const char *field = place_field_name(lhs);
                if (field)
                    for (size_t i = 0; i < bv->nfreed_fields; i++)
                        if (bv->freed_fields[i] == field) {
                            bv->freed_fields[i] =
                                bv->freed_fields[bv->nfreed_fields - 1];
                            bv->nfreed_fields--;
                            break;
                        }
            }
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

/* String.free() mutates the backing allocation, so it must reject both
 * moved receivers and any live borrow (a str view from .view()). */
static void check_free_receiver(Borrow *b, Expr *recv)
{
    const char *root = resolve_root(b, recv);
    if (!root) {
        bx_expr(b, recv);
        return;
    }
    BVar *bv = var_find(b, root);
    if (!bv)
        return;
    if (bv->moved)
        bfatal(recv->span, "use of moved value '%s'", root);
    if (bv->freed)
        bfatal(recv->span, "value was already freed: '%s'", root);
    if (bv->defer_free)
        bfatal(recv->span,
               "cannot free '%s': a deferred cleanup already frees it",
               root);
    const char *field = place_field_name(recv);
    if (field && field_freed(bv, field))
        bfatal(recv->span, "value was already freed: '%s.%s'", root,
               field);

    Loan *l = root_loan(b, root);
    if (l)
        bfatal(recv->span,
               "cannot free '%s': it is still borrowed by '%s'", root,
               l->holder);
    /* track the direct owner and released fields separately, so freeing
     * h.name does not poison h.id but does catch a second h.name.free() */
    if (recv->kind == EX_IDENT) {
        bv->freed = 1;
        clear_freed_fields(bv);
    } else if (field) {
        mark_field_freed(bv, field);
    }
}

static Decl *find_fn_decl(Borrow *b, const char *name);

static void bx_call(Borrow *b, Expr *e)
{
    Expr *callee = e->u.call.callee;
    /* builtin String methods read the receiver: using a moved String
     * through .view()/.free() must still be rejected, but the receiver
     * itself is not consumed by the call. */
    if (e->str_recv) {
        if (e->builtin_str_method == 1)
            check_free_receiver(b, e->str_recv);
        else {
            int prev = b->read_only;
            b->read_only = 1;
            bx_expr(b, e->str_recv);
            b->read_only = prev;
        }
    }
    /* print/println args are printable (hence Copy) per sema, so they
     * can never move; inline consuming calls inside args still move */
    Decl *callee_decl = callee->kind == EX_IDENT
                            ? find_fn_decl(b, callee->u.ident) : NULL;
    DA(TempLoan) temps = {0};
    for (size_t i = 0; i < e->u.call.args.len; i++) {
        Expr *arg = e->u.call.args.items[i];
        /* passing an existing &mut binding to a &mut parameter is a
         * reborrow, not a move of the reference */
        if (arg->kind == EX_IDENT && callee_decl
            && i < callee_decl->u.fn.params.len) {
            BVar *av = var_find(b, arg->u.ident);
            Type *formal = callee_decl->u.fn.params.items[i].ty;
            if (av && av->ty->kind == TY_MUTREF && formal
                && formal->kind == TY_MUTREF
                && type_eq(av->ty, formal)) {
                if (av->moved)
                    bfatal(arg->span, "use of moved value '%s'", av->name);
                Loan *lk = root_locked(b, av->name);
                if (lk)
                    bfatal(arg->span,
                           "'%s' is already mutably borrowed by '%s'",
                           av->name, lk->holder);
                continue;
            }
        }
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
        int prev = b->read_only;
        if (e->builtin_print)
            b->read_only = 1;
        bx_expr(b, arg);
        b->read_only = prev;
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

/* Find the callee declaration for call-site formal parameter types. */
static Decl *find_fn_decl(Borrow *b, const char *name)
{
    for (size_t i = 0; i < b->prog->decls.len; i++) {
        Decl *d = b->prog->decls.items[i];
        if (d->kind != DL_FN)
            continue;
        if (d->u.fn.name == name || (d->regname && d->regname == name))
            return d;
        if (d->u.fn.srcname && d->u.fn.srcname == name)
            return d;
    }
    return NULL;
}

/* Roots a callee may have captured into its returned view: borrowed
 * receiver (&v / &mut v), slice arguments, and reference/slice locals
 * chained through their own holder loans. */
static void extract_call_roots(Borrow *b, Expr *call, CallRootList *roots)
{
    if (call->str_recv)
        push_call_root(roots, resolve_root(b, call->str_recv), 0);
    int method = call->method_call;
    for (size_t i = 0; i < call->u.call.args.len; i++) {
        Expr *arg = call->u.call.args.items[i];
        if (method && i == 0 && arg->kind == EX_UNARY
            && arg->u.unary.op == TK_AMP
            && call->type && call->type->kind != TY_REF
            && call->type->kind != TY_MUTREF
            && call->type->kind != TY_SLICE) {
            /* method returning an element copy (Vec[str].get): the copy
             * inherits loans held by the receiver, not a borrow of it */
            const char *recv = resolve_root(b, arg->u.unary.e);
            for (size_t k = 0; recv && k < b->loans.len; k++)
                if (b->loans.items[k].active
                    && b->loans.items[k].holder == recv)
                    push_call_root(roots, b->loans.items[k].root,
                                   b->loans.items[k].exclusive);
            continue;
        }
        if (arg->kind == EX_UNARY && arg->u.unary.op == TK_AMP) {
            push_call_root(roots, resolve_root(b, arg->u.unary.e),
                           arg->borrow_mut);
            continue;
        }
        if (arg->kind == EX_SLICE) {
            push_call_root(roots, resolve_root(b, arg->u.slice.base), 0);
            continue;
        }
        if (!method && arg->kind == EX_IDENT && arg->type
            && arg->type->kind == TY_STR) {
            /* free functions returning str may return a view of this arg;
             * method receivers are excluded because Vec.get returns a
             * value that does not borrow the vector itself. */
            Loan *l = holder_loan(b, arg->u.ident);
            if (l)
                push_call_root(roots, l->root, l->exclusive);
            continue;
        }
        if (!method && arg->kind == EX_CALL && arg->builtin_str_method == 2) {
            /* trim(s.view()) borrows the String behind the temporary view */
            push_call_root(roots, resolve_root(b, arg->str_recv), 0);
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

/* Raw-pointer places belong to the unsafe allocator domain, not to a
 * tracked owner binding. */
static int place_through_raw_pointer(Expr *e)
{
    while (e) {
        if (e->kind == EX_FIELD) {
            e = e->u.field.base;
            continue;
        }
        if (e->kind == EX_INDEX) {
            if (e->u.index.base->type
                && e->u.index.base->type->kind == TY_PTR)
                return 1;
            e = e->u.index.base;
            continue;
        }
        if (e->kind == EX_DEREF) {
            if (e->u.deref->type && e->u.deref->type->kind == TY_PTR)
                return 1;
            e = e->u.deref;
            continue;
        }
        return 0;
    }
    return 0;
}

/* Roots whose storage may be embedded in a view-bearing value. */
static void collect_view_roots(Borrow *b, Expr *e, CallRootList *roots)
{
    if (!e)
        return;
    switch (e->kind) {
    case EX_IDENT: {
        Loan *l = holder_loan(b, e->u.ident);
        if (l)
            push_call_root(roots, l->root, l->exclusive);
        return;
    }
    case EX_FIELD:
    case EX_INDEX: {
        if (place_through_raw_pointer(e))
            return;
        const char *root = resolve_root(b, e);
        if (root) {
            push_call_root(roots, root, 0);
            return;
        }
        const char *ref = place_ref_root(b, e);
        if (ref) {
            Loan *l = holder_loan(b, ref);
            if (l)
                push_call_root(roots, l->root, l->exclusive);
            return;
        }
        Expr *base = e->kind == EX_FIELD ? e->u.field.base
                                         : e->u.index.base;
        collect_view_roots(b, base, roots);
        return;
    }
    case EX_DEREF: {
        if (e->u.deref->type && e->u.deref->type->kind == TY_PTR)
            return;
        const char *ref = place_ref_root(b, e);
        if (ref) {
            Loan *l = holder_loan(b, ref);
            if (l)
                push_call_root(roots, l->root, l->exclusive);
            return;
        }
        collect_view_roots(b, e->u.deref, roots);
        return;
    }
    case EX_SLICE:
        if (e->u.slice.base->type
            && e->u.slice.base->type->kind == TY_PTR)
            return; /* raw-pointer slices have no tracked owner */
        {
            const char *root = resolve_root(b, e->u.slice.base);
            if (root) {
                push_call_root(roots, root, 0);
                return;
            }
            collect_view_roots(b, e->u.slice.base, roots);
        }
        return;
    case EX_CALL:
        extract_call_roots(b, e, roots);
        return;
    case EX_UNARY:
        if (e->u.unary.op == TK_AMP) {
            push_call_root(roots, resolve_root(b, e->u.unary.e),
                           e->borrow_mut);
            return;
        }
        collect_view_roots(b, e->u.unary.e, roots);
        return;
    case EX_MATCH:
        for (size_t i = 0; i < e->u.match.arms.len; i++) {
            MatchArm *a = &e->u.match.arms.items[i];
            if (!a->jump)
                collect_view_roots(b, a->val, roots);
        }
        return;
    case EX_ORELSE:
        collect_view_roots(b, e->u.orelse.lhs, roots);
        collect_view_roots(b, e->u.orelse.val, roots);
        return;
    case EX_TRY:
        collect_view_roots(b, e->u.tryy, roots);
        return;
    case EX_UNWRAP:
        collect_view_roots(b, e->u.unwrap, roots);
        return;
    case EX_COERCE:
        collect_view_roots(b, e->u.coerce.e, roots);
        return;
    case EX_CAST:
        collect_view_roots(b, e->u.cast.e, roots);
        return;
    case EX_STRUCT_LIT:
        for (size_t i = 0; i < e->u.stlit.fields.len; i++)
            collect_view_roots(b, e->u.stlit.fields.items[i].val, roots);
        return;
    case EX_ARR_LIT:
        for (size_t i = 0; i < e->u.elems.len; i++)
            collect_view_roots(b, e->u.elems.items[i], roots);
        return;
    default:
        return;
    }
}

/* Rebinding a view-bearing destination replaces its old loans with the
 * roots of the new value, including match arms and aggregate literals. */
static void rebind_view_loans(Borrow *b, const char *holder, Expr *rhs)
{
    CallRootList roots = {0};
    collect_view_roots(b, rhs, &roots);
    for (size_t i = 0; i < b->loans.len; i++)
        if (b->loans.items[i].holder == holder)
            b->loans.items[i].active = 0;
    Expr *src = peel_try_unwrap(rhs);
    if (src && src->kind == EX_IDENT) {
        BVar *sv = var_find(b, src->u.ident);
        if (sv && sv->ty && ty_is_move(b, sv->ty, 0))
            for (size_t i = 0; i < b->loans.len; i++)
                if (b->loans.items[i].holder == src->u.ident)
                    b->loans.items[i].active = 0;
    }
    for (size_t i = 0; i < roots.len; i++) {
        if (!roots.items[i].root || root_locked(b, roots.items[i].root))
            continue;
        Loan l;
        memset(&l, 0, sizeof(l));
        l.holder = holder;
        l.root = roots.items[i].root;
        l.exclusive = roots.items[i].exclusive;
        l.active = 1;
        l.last_use = recorded_last_use(b, holder);
        l.origin = rhs->span;
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
        if (bv->freed)
            bfatal(e->span, "use of freed value '%s'", bv->name);
        if (bv->ty->kind == TY_REF)
            return; /* shared references are Copy */
        if (bv->ty->kind == TY_MUTREF) {
            /* &mut T is linear: using it as a value moves the invitation */
            if (bv->moved)
                bfatal(e->span, "use of moved value '%s'", bv->name);
            if (bv->defer_used)
                bfatal(e->span,
                       "cannot move '%s': a deferred cleanup still uses it",
                       bv->name);
            Loan *lk = root_locked(b, bv->name);
            if (lk)
                bfatal(e->span,
                       "cannot move '%s': mutably borrowed by '%s'",
                       bv->name, lk->holder);
            if (root_shared(b, bv->name) > 0)
                bfatal(e->span,
                       "cannot move '%s': shared borrow is active",
                       bv->name);
            bv->moved = 1;
            return;
        }
        if (b->read_only && bv->ty->kind == TY_STRUCT && bv->ty->name
            && zstr_eq(bv->ty->name, "String")) {
            check_root_read(b, e, e->u.ident);
            return;
        }
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
        const char *ref = place_ref_root(b, e);
        if (ref) {
            check_ref_place_read(b, e, ref);
            return;
        }
        bx_expr(b, e->u.deref);
        const char *root = resolve_root(b, e);
        if (root)
            check_root_read(b, e, root);
        return;
    }
    case EX_FIELD: {
        const char *ref = place_ref_root(b, e);
        if (ref) {
            check_ref_place_read(b, e, ref);
            return;
        }
        if (!b->read_only && ty_is_move(b, e->type, 0))
            bfatal(e->span,
                   "cannot move field '%s' out of a resource container; "
                   "move the whole value instead", e->u.field.name);
        const char *root = resolve_root(b, e);
        if (root) {
            /* reading a field is a use of the root, never a move of it */
            check_root_read(b, e, root);
            BVar *bv = var_find(b, root);
            const char *field = place_field_name(e);
            if (field && field_freed(bv, field))
                bfatal(e->span, "use of freed value '%s.%s'", root, field);
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
    case EX_BINARY: {
        int stringish = e->type && e->type->kind == TY_STRUCT
                        && e->type->name && zstr_eq(e->type->name, "String");
        if (!stringish && (e->u.binary.op == TK_EQ
                           || e->u.binary.op == TK_NEQ
                           || e->u.binary.op == TK_LT
                           || e->u.binary.op == TK_LE
                           || e->u.binary.op == TK_GT
                           || e->u.binary.op == TK_GE)) {
            Type *lt = e->u.binary.lhs->type;
            Type *rt = e->u.binary.rhs->type;
            stringish = (lt && (lt->kind == TY_STR
                                || (lt->kind == TY_STRUCT && lt->name
                                    && zstr_eq(lt->name, "String"))))
                     || (rt && (rt->kind == TY_STR
                                || (rt->kind == TY_STRUCT && rt->name
                                    && zstr_eq(rt->name, "String"))));
        }
        int prev = b->read_only;
        if (stringish)
            b->read_only = 1;
        bx_expr(b, e->u.binary.lhs);
        bx_expr(b, e->u.binary.rhs);
        b->read_only = prev;
        return;
    }
    case EX_INDEX: {
        const char *ref = place_ref_root(b, e);
        if (ref) {
            check_ref_place_read(b, e, ref);
            return;
        }
        bx_expr(b, e->u.index.base);
        bx_expr(b, e->u.index.index);
        return;
    }
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
        walk_match(b, e);
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

/* Reject returning borrowed data rooted in a local binding.  Parameters
 * are allowed: the caller tracks provenance through transfer_call_loans. */
static void check_return_escape(Borrow *b, Expr *raw)
{
    Expr *e = peel_try_unwrap(raw);
    if (!e)
        return;
    if (e->kind == EX_UNARY && e->u.unary.op == TK_AMP) {
        const char *root = resolve_root(b, e->u.unary.e);
        if (root && root_is_local(b, root))
            bfatal(e->span,
                   "cannot return reference to local variable '%s'", root);
        return;
    }
    if (!e->type || !type_contains_view(b, e->type, 0))
        return;
    CallRootList roots = {0};
    collect_view_roots(b, e, &roots);
    for (size_t i = 0; i < roots.len; i++)
        if (roots.items[i].root && root_is_local(b, roots.items[i].root))
            bfatal(e->span, "cannot return borrowed data from local '%s'",
                   roots.items[i].root);
    free(roots.items);
}

/* ---- statements ---- */

static int b_block(Borrow *b, Block *blk);
static int b_stmt(Borrow *b, Stmt *s);

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
        if (e->builtin_str_method == 1 && e->str_recv
            && e->str_recv->kind == EX_IDENT) {
            BVar *bv = var_find(b, e->str_recv->u.ident);
            if (bv) {
                if (bv->freed)
                    bfatal(e->span,
                           "cannot defer free of already-freed '%s'",
                           bv->name);
                bv->defer_free = 1;
            }
        }
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
        if (b->vars.items[i].freed && !diverged)
            base[i].freed = 1;
        if (b->vars.items[i].defer_used)
            base[i].defer_used = 1;
        if (b->vars.items[i].defer_free)
            base[i].defer_free = 1;
    }
}

static int walk_branch(Borrow *b, Block *body)
{
    size_t nvars = b->vars.len;
    size_t nloans = b->loans.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    int diverged = b_block(b, body);
    merge_branch_marks(b, base, nvars, diverged);
    b->vars.len = nvars;
    b->loans.len = nloans;
    for (size_t i = 0; i < nvars; i++)
        b->vars.items[i] = base[i];
    free(base);
    return diverged;
}

static int walk_arm(Borrow *b, MatchArm *a)
{
    size_t nvars = b->vars.len;
    size_t nloans = b->loans.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    if (a->name && a->ty)
        var_add(b, a->name, a->ty, 0);
    int diverged = 0;
    if (a->jump) {
        diverged = b_stmt(b, a->jump);
    } else {
        bx_expr(b, a->val);
    }
    merge_branch_marks(b, base, nvars, diverged);
    b->vars.len = nvars;
    b->loans.len = nloans;
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
    size_t nloans = b->loans.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    var_add(b, s->u.foreach.name, s->u.foreach.ty, 0);
    b_block(b, s->u.foreach.body);
    b->loans.len = nloans;
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
    size_t nloans = b->loans.len;
    BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
    for (size_t i = 0; i < nvars; i++)
        base[i] = b->vars.items[i];
    var_add(b, s->u.forr.name, s->u.forr.ty, 0);
    b_block(b, s->u.forr.body);
    b->loans.len = nloans;
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
            const char *root = reborrow_root(b, init->u.unary.e);
            if (!root)
                root = resolve_root(b, init->u.unary.e);
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
        if (init->kind == EX_IDENT) {
            BVar *src = var_find(b, init->u.ident);
            if (src && (src->ty->kind == TY_REF
                        || src->ty->kind == TY_MUTREF)) {
                Type *lt = s->u.let.ty ? s->u.let.ty : init->type;
                TypeKind sk = src->ty->kind;
                if (sk == TY_MUTREF) {
                    /* &mut T moves; its loan follows the new owner */
                    if (src->moved)
                        bfatal(init->span, "use of moved value '%s'",
                               src->name);
                    Loan *l = holder_loan(b, src->name);
                    if (l) {
                        l->holder = s->u.let.name;
                        l->last_use = recorded_last_use(b, s->u.let.name);
                    }
                    src->moved = 1;
                }
                var_add(b, s->u.let.name, lt, s->u.let.mut);
                if (sk == TY_REF) {
                    /* shared references are Copy: copy the loan too */
                    Loan *l = holder_loan(b, src->name);
                    if (l) {
                        Loan copy = *l;
                        copy.holder = s->u.let.name;
                        copy.last_use = recorded_last_use(b, s->u.let.name);
                        da_push(&b->loans, copy);
                    }
                }
                return 0;
            }
        }
        bx_expr(b, init);
        Type *lt = s->u.let.ty ? s->u.let.ty : init->type;
        var_add(b, s->u.let.name, s->u.let.ty, s->u.let.mut);
        if (lt && type_contains_view(b, lt, 0))
            rebind_view_loans(b, s->u.let.name, init);
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
        size_t nloans = b->loans.len;
        BVar *base = nvars ? malloc(nvars * sizeof(BVar)) : NULL;
        for (size_t i = 0; i < nvars; i++)
            base[i] = b->vars.items[i];
        b_block(b, s->u.whilee.body);
        b->loans.len = nloans;
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
        check_return_escape(b, s->u.ret);
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
        bx_expr(b, s->u.expr);
        return 0;
    case ST_BLOCK: {
        size_t nvars = b->vars.len;
        size_t nloans = b->loans.len;
        int diverged = b_block(b, s->u.block);
        b->vars.len = nvars;
        b->loans.len = nloans;
        return diverged;
    }
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
