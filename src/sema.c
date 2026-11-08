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
#include "modules.h"
#include "diag.h"

#include <ctype.h>

typedef enum SymKind { SYM_VAR, SYM_FN, SYM_NS, SYM_TYPE } SymKind;

typedef struct Sym {
    const char *name;
    SymKind kind;
    Span span;
    Type *type;      /* variable type, alias type, or struct type */
    int mut;
    int read_count;  /* reads of a SYM_VAR; 0 at declaration */
    DA(Param) params; /* function signature */
    Type *ret;
    CHeader *hdr;     /* imported C namespace members */
    int is_zov;       /* namespace of a loaded .zov module */
    const char *modname; /* owning module for a private C alias */
    StrList zsyms;    /* exported symbol names of the module */
    StrList ztargets; /* registered spellings for ns call rewrites */
    const char *alias_to; /* module-scope alias: the registered name */
    int is_template;  /* generic decl, checked only via instantiation */
    Decl *decl;       /* owning decl for templates */
} Sym;

typedef struct Scope {
    struct Scope *parent;
    DA(Sym*) syms;
} Scope;

typedef struct Sema {
    Scope *scope;
    Type *fn_ret;
    int loop_depth;
    int has_main;
    int inst_depth; /* nested generic instantiations; guards recursive types */
    Program *prog;
    DA(struct Meth) methods;
    DA(Sym) modc;      /* module-private C header aliases */
    DA(Sym) modtypes;  /* module-private Zov type declarations */
    const char *curmod; /* module whose body/signature is being checked */
} Sema;

typedef struct Meth {
    const char *sname; /* struct template name: Vec, Point */
    const char *mname; /* method name as written: push */
    Sym sym; /* stored by value: the scope array reallocs */
} Meth;

static Sym *find_method(Sema *se, const char *sname, const char *mname);
static Sym *instantiate_method(Sema *se, Sym *m, const char *key,
                               const char *tmpl_sname, TypeList *inst_args);

static void push_scope(Sema *se)
{
    Scope *s = zalloc(sizeof(Scope));
    s->parent = se->scope;
    se->scope = s;
}

static void pop_scope(Sema *se)
{
    for (size_t i = 0; i < se->scope->syms.len; i++) {
        Sym *s = se->scope->syms.items[i];
        if (s->kind != SYM_VAR || s->read_count > 0 || !s->name)
            continue;
        if (s->name[0] == '_')
            continue;
        char help[256];
        snprintf(help, sizeof(help), "prefix with an underscore to "
                "silence: '_%s'", s->name);
        Diagnostic d;
        memset(&d, 0, sizeof(d));
        d.severity = DIAG_WARN;
        d.code = "W0001";
        char msg[256];
        snprintf(msg, sizeof(msg), "unused variable '%s'", s->name);
        d.message = msg;
        d.primary.file = s->span.file;
        d.primary.line = (int)s->span.line;
        d.primary.col = (int)s->span.col;
        d.primary.len = 1;
        d.primary.src_ptr = s->span.start;
        d.help = help;
        diag_emit(&d);
    }
    se->scope = se->scope->parent;
}

static Sym *scope_find(Scope *sc, const char *name)
{
    for (Scope *s = sc; s; s = s->parent)
        for (size_t i = s->syms.len; i > 0; i--)
            if (s->syms.items[i - 1]->name == name)
                return s->syms.items[i - 1];
    return NULL;
}

static void scope_add(Sema *se, Sym sym)
{
    for (size_t i = se->scope->syms.len; i > 0; i--)
        if (se->scope->syms.items[i - 1]->name == sym.name)
            fatal_at(E0409, sym.span, "duplicate definition of '%s'", sym.name);
    Sym *s = zalloc(sizeof(Sym));
    *s = sym;
    da_push(&se->scope->syms, s);
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
        if (d->kind == DL_TYPE && zstr_eq(d->u.type.name, name)
            && !d->u.type.alias_ty && d->u.type.tparams.len == 0)
            return d;
    }
    return NULL;
}

/* std::mem::drop is an explicit destruction point.  Its generic
 * declaration exists so module resolution registers it, but codegen
 * lowers each call against the concrete argument type. */
static int is_mem_drop_target(Sema *se, const char *target)
{
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind != DL_FN || !d->regname || d->regname != target)
            continue;
        return d->modname && zstr_eq(d->modname, "mem")
               && zstr_eq(d->u.fn.name, "drop") && !d->u.fn.recv;
    }
    return 0;
}

/* mem::drop consumes a place: the value stays in storage but the
 * resources it owns are released. */
static int drop_place_kind(Expr *e)
{
    return e->kind == EX_IDENT || e->kind == EX_FIELD
           || e->kind == EX_INDEX || e->kind == EX_DEREF;
}

/* Dropping through &T/&mut T cannot update the borrower's bookkeeping:
 * the owner would still appear initialized and could free twice.  Raw
 * pointer dereferences stay in the unchecked domain. */
static int drop_through_reference(Expr *e)
{
    while (e) {
        if (e->kind == EX_FIELD) {
            e = e->u.field.base;
            continue;
        }
        if (e->kind == EX_INDEX) {
            e = e->u.index.base;
            continue;
        }
        break;
    }
    if (!e)
        return 0;
    if (e->kind == EX_DEREF)
        return e->u.deref->type
               && (e->u.deref->type->kind == TY_REF
                   || e->u.deref->type->kind == TY_MUTREF);
    if (e->kind == EX_IDENT && e->type)
        return e->type->kind == TY_REF || e->type->kind == TY_MUTREF;
    return 0;
}

/* A type that contains itself by value has infinite size.  Pointers,
 * slices, references, and owned String (which stores a pointer) break
 * the cycle; arrays, unions, and aliases do not. */
typedef struct CycCtx {
    Sema *se;
    const char *path[64];
    int alias_path[64]; /* path[i] was entered through a type alias */
    int npath;
} CycCtx;

static Decl *find_type_decl_any(Sema *se, const char *name)
{
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind == DL_TYPE && d->u.type.tparams.len == 0
            && !d->u.type.is_enum && zstr_eq(d->u.type.name, name))
            return d;
    }
    return NULL;
}

/* Renders the active path from `from` plus the repeated name. */
static const char *cycle_chain(CycCtx *c, int from, const char *again)
{
    static char buf[512];
    size_t off = 0;
    buf[0] = '\0';
    for (int i = from; i < c->npath && off + 4 < sizeof(buf); i++)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s -> ",
                                c->path[i]);
    snprintf(buf + off, sizeof(buf) - off, "%s", again);
    return buf;
}

static void cyc_walk_type(CycCtx *c, Type *t, Span span);

static void cyc_walk_decl(CycCtx *c, Decl *d, Span span)
{
    for (int i = 0; i < c->npath; i++) {
        if (!zstr_eq(c->path[i], d->u.type.name))
            continue;
        int all_alias = d->u.type.alias_ty != NULL;
        for (int j = i; j < c->npath; j++)
            if (!c->alias_path[j])
                all_alias = 0;
        if (all_alias)
            fatal_at(E0416, span, "type alias cycle: %s",
                     cycle_chain(c, i, d->u.type.name));
        fatal_at(E0410, span,
                 "recursive type '%s' has infinite size: %s; "
                 "use a pointer or slice", d->u.type.name,
                 cycle_chain(c, i, d->u.type.name));
    }
    if (c->npath >= 60)
        fatal_at(E0805, d->u.type.name ? span : span,
                 "type nesting is too deep");
    c->alias_path[c->npath] = d->u.type.alias_ty != NULL;
    c->path[c->npath++] = d->u.type.name;
    if (d->u.type.alias_ty) {
        cyc_walk_type(c, d->u.type.alias_ty, span);
    } else {
        for (size_t i = 0; i < d->u.type.fields.len; i++) {
            Field *f = &d->u.type.fields.items[i];
            cyc_walk_type(c, f->ty, f->span);
        }
    }
    c->npath--;
}

static void cyc_walk_type(CycCtx *c, Type *t, Span span)
{
    if (!t)
        return;
    switch (t->kind) {
    case TY_ARRAY:
        cyc_walk_type(c, t->base, span);
        break;
    case TY_UNION:
        /* name the union in the chain: S -> (S | !void) -> S */
        for (size_t k = 0; k < t->members.len; k++) {
            Type *m = t->members.items[k];
            while (m && m->kind == TY_ARRAY)
                m = m->base;
            if (!m || m->kind != TY_STRUCT || !m->name
                || zstr_eq(m->name, "String"))
                continue;
            for (int i = 0; i < c->npath; i++) {
                if (!zstr_eq(c->path[i], m->name))
                    continue;
                char chain[512];
                size_t off = 0;
                for (int j = i; j < c->npath && off + 4 < sizeof(chain); j++)
                    off += (size_t)snprintf(chain + off, sizeof(chain) - off,
                                           "%s -> ", c->path[j]);
                snprintf(chain + off, sizeof(chain) - off, "%s -> %s",
                         type_spelling(t), m->name);
                fatal_at(E0410, span,
                         "recursive type '%s' has infinite size: %s; "
                         "use a pointer or slice", m->name, chain);
            }
        }
        for (size_t k = 0; k < t->members.len; k++)
            cyc_walk_type(c, t->members.items[k], span);
        break;
    case TY_STRUCT: {
        /* owned String stores a heap pointer, so it never nests */
        if (!t->name || zstr_eq(t->name, "String"))
            return;
        Decl *d = find_type_decl_any(c->se, t->name);
        if (d)
            cyc_walk_decl(c, d, span);
        break;
    }
    default:
        break; /* pointers, slices, references, scalars stop the cycle */
    }
}

static void check_all_type_cycles(Sema *se)
{
    CycCtx c;
    memset(&c, 0, sizeof(c));
    c.se = se;
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind != DL_TYPE || d->u.type.is_enum
            || d->u.type.tparams.len > 0)
            continue;
        c.npath = 0;
        cyc_walk_decl(&c, d, d->span);
    }
}

static Type *unknown(void)
{
    return type_prim(TY_UNKNOWN);
}

static int is_unknown(Type *t)
{
    return t && t->kind == TY_UNKNOWN;
}

/* Owned string: a compiler-known resource struct.  The declaration is
 * injected once per program so `String` is available without an import. */
static int is_string_type(Type *t)
{
    return t && t->kind == TY_STRUCT && t->name
        && zstr_eq(t->name, "String");
}

static int is_stringish(Type *t)
{
    return t && (t->kind == TY_STR || is_string_type(t));
}

/* True when copying this value bitwise aliases owned storage.  Used to
 * stop Vec.get/Map.get from handing out a second owner of a resource. */
static int type_owns_resource(Sema *se, Type *t, int depth)
{
    if (!t || depth > 8)
        return 0;
    switch (t->kind) {
    case TY_MUTREF:
        return 1;
    case TY_STRUCT: {
        if (is_string_type(t))
            return 1;
        Decl *d = find_struct_decl(se, t->name);
        if (!d)
            return 0;
        if (d->u.type.is_resource)
            return 1;
        for (size_t i = 0; i < d->u.type.fields.len; i++)
            if (type_owns_resource(se, d->u.type.fields.items[i].ty,
                                   depth + 1))
                return 1;
        return 0;
    }
    case TY_UNION:
        for (size_t i = 0; i < t->members.len; i++)
            if (type_owns_resource(se, t->members.items[i], depth + 1))
                return 1;
        return 0;
    default:
        return 0;
    }
}

/* Indexed places on arrays and slices are shared storage: moving an owned
 * element out would leave the container responsible for a value it no longer
 * owns, which is a double-free waiting to happen.  Borrowing and overwriting
 * stay available because they preserve exactly one owner. */
static int indexed_owned_place(Sema *se, Expr *e)
{
    while (e && e->kind == EX_FIELD)
        e = e->u.field.base;
    if (!e || e->kind != EX_INDEX || !e->type
        || !type_owns_resource(se, e->type, 0))
        return 0;
    Type *bt = e->u.index.base->type;
    return bt && (bt->kind == TY_ARRAY || bt->kind == TY_SLICE);
}

static void reject_indexed_move(Sema *se, Expr *e)
{
    if (!indexed_owned_place(se, e))
        return;
    fatal_at(E0518, e->span,
             "cannot move an owned element out of an indexed place; "
             "borrow it with '&arr[i]' or transfer it with a container API");
}

static void install_builtin_string(Program *prog)
{
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (d->kind == DL_TYPE && zstr_eq(d->u.type.name, "String"))
            return;
    }
    Span sp;
    memset(&sp, 0, sizeof(sp));
    Decl *d = decl_new(DL_TYPE, sp);
    d->u.type.name = str_intern_c("String");
    d->u.type.is_resource = 1;
    Field data;
    memset(&data, 0, sizeof(data));
    data.name = str_intern_c("data");
    data.ty = type_ptr(type_prim(TY_U8), 1);
    da_push(&d->u.type.fields, data);
    Field len;
    memset(&len, 0, sizeof(len));
    len.name = str_intern_c("len");
    len.ty = type_prim(TY_USIZE);
    da_push(&d->u.type.fields, len);
    Field cap;
    memset(&cap, 0, sizeof(cap));
    cap.name = str_intern_c("cap");
    cap.ty = type_prim(TY_USIZE);
    da_push(&d->u.type.fields, cap);
    da_push(&prog->decls, d);
}

static int is_ptrish(Type *t)
{
    return t && (t->kind == TY_PTR || t->kind == TY_REF
                 || t->kind == TY_MUTREF);
}

/* *T widens to nullable *T: always safe, never loses nullability */
static int ptr_widens(Type *from, Type *to)
{
    return from && to && from->kind == TY_PTR && to->kind == TY_PTR
        && !from->nullable && to->nullable
        && type_eq(from->base, to->base);
}

/* The null literal may initialize any nullable pointer target. */
static int null_fits(Expr *e, Type *to)
{
    return e && e->kind == EX_NULL && to && to->kind == TY_PTR
        && to->nullable;
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
    case TY_PTR: case TY_ENUM:
        return 1;
    default:
        return is_string_type(t);
    }
}

/* Integer literal typing and range checking. */
static Type *int_literal_type(Expr *e, Type *hint)
{
    Type *suf = type_from_suffix(e->u.ilit.suffix);
    if (suf)
        return suf;
    if (hint && ty_is_int(hint->kind))
        return hint;
    return type_default_int();
}

static int int_bounds(Type *t, uint64_t *max, int *is_signed)
{
    *is_signed = 0;
    switch (t->kind) {
    case TY_I8: *max = 127u; *is_signed = 1; return 1;
    case TY_I16: *max = 32767u; *is_signed = 1; return 1;
    case TY_I32: *max = 2147483647u; *is_signed = 1; return 1;
    case TY_I64:
    case TY_ISIZE: *max = UINT64_C(9223372036854775807); *is_signed = 1; return 1;
    case TY_U8: *max = 255u; return 1;
    case TY_U16: *max = 65535u; return 1;
    case TY_U32: *max = 4294967295u; return 1;
    case TY_U64:
    case TY_USIZE: *max = UINT64_MAX; return 1;
    default: return 0;
    }
}

static void check_int_literal(Expr *e, Type *ty)
{
    uint64_t max;
    int is_signed;
    if (!int_bounds(ty, &max, &is_signed))
        return;
    if (e->u.ilit.v > max)
        fatal_at(E0701, e->span, "integer literal %llu does not fit in %s",
                 (unsigned long long)e->u.ilit.v, type_spelling(ty));
}

static Decl *find_enum_decl(Sema *se, const char *name)
{
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind == DL_TYPE && d->u.type.is_enum
            && d->u.type.name == name)
            return d;
    }
    return NULL;
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
static void push_module_scope(Sema *se, const char *modname);
static const char *fn_display_name(Sym *fn);

// walks the scope chain collecting candidate names for typo suggestions
static const char *scope_suggest(Sema *se, const char *name)
{
    const char *cands[256];
    size_t n = 0;
    for (Scope *s = se->scope; s && n < 256; s = s->parent)
        for (size_t i = s->syms.len; i > 0 && n < 256; i--)
            cands[n++] = s->syms.items[i - 1]->name;
    return diag_suggest_symbol(name, cands, n);
}

/* module fn access resolves to the mangled registered spelling */
static const char *zov_ns_target(Sema *se, const char *ns, const char *name){
    Sym *s = scope_find(se->scope, ns);
    if (!s || s->kind != SYM_NS || !s->is_zov)
        return NULL;
    for (size_t i = 0; i < s->zsyms.len; i++)
        if (s->zsyms.items[i] == name)
            return s->ztargets.items[i];
    return NULL;
}

/* Module-private C aliases live outside the global scope.  A module body
 * sees its own bindings; only that module may call through them. */
static Sym *ns_lookup(Sema *se, const char *name)
{
    if (se->curmod)
        for (size_t i = 0; i < se->modc.len; i++)
            if (se->modc.items[i].modname == se->curmod
                && se->modc.items[i].name == name)
                return &se->modc.items[i];
    Sym *s = scope_find(se->scope, name);
    if (s)
        return s;
    for (size_t i = 0; i < se->modc.len; i++)
        if (se->modc.items[i].name == name)
            return &se->modc.items[i];
    return NULL;
}

static int c_ns_private(Sema *se, Sym *ns)
{
    return ns && ns->modname && ns->modname != se->curmod;
}

static const char *mod_display_name(const char *modname)
{
    static const char *const stds[] = {
        "io", "fs", "env", "proc", "time", "math", "mem", "str", "vec", "map", "arena", "strsplit"
    };
    if (!modname)
        return "";
    if (strncmp(modname, "std::", 5) == 0)
        return modname;
    for (size_t i = 0; i < sizeof(stds) / sizeof(stds[0]); i++) {
        if (strcmp(modname, stds[i]) == 0) {
            static char buf[64];
            snprintf(buf, sizeof(buf), "std::%s", modname);
            return buf;
        }
    }
    return modname;
}

/* module ns call diagnostics: a missing member or an unimported std
 * module gets an actionable message instead of the C-ns fallback */
static void ns_call_diag(Sema *se, Expr *callee, Span span)
{
    const char *ns = callee->u.nident.ns;
    Sym *nsym = scope_find(se->scope, ns);
    if (nsym && nsym->kind == SYM_NS && nsym->is_zov)
        fatal_at(E0403, span, "module '%s' has no function '%s'", ns,
                 callee->u.nident.name);
    const char *stdpath = NULL;
    if (strstr(ns, "::")) {
        if (zov_module_exists(ns))
            stdpath = ns;
    } else {
        char probe[256];
        snprintf(probe, sizeof(probe), "std::%s", ns);
        if (zov_module_exists(probe))
            stdpath = str_intern_c(probe);
    }
    if (stdpath)
        fatal_at(E0404, span, "module '%s' is not imported; add `use %s;` "
                 "at the top of the file", ns, stdpath);
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
    Sym *ns = ns_lookup(se, str_intern_c(nbuf));
    if (!ns || ns->kind != SYM_NS || c_ns_private(se, ns))
        return NULL;
    if (ns->is_zov) {
        /* loaded .zov module: member resolves to the global symbol */
        Sym *member = scope_find(se->scope, str_intern_c(sep + 2));
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
                diag_fatal_global(E0408, "unknown imported type '%s'", t->name);
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

/* Wrap e in EX_COERCE when target is a union and e holds a member type
 * (directly, or as an error variant's payload). */
static Expr *maybe_coerce(Expr *e, Type *target)
{
    if (!target || target->kind != TY_UNION || !e->type)
        return e;
    if (e->type->kind == TY_UNION || e->type->kind == TY_UNKNOWN)
        return e;
    int tag = union_member_index(target, e->type);
    if (tag < 0) {
        /* error-payload wrap: return 1 builds FsError from its i32 */
        for (size_t i = 0; i < target->members.len; i++) {
            Type *m = target->members.items[i];
            if (m->is_error && type_eq(m->base, e->type)) {
                tag = (int)i;
                break;
            }
        }
    }
    if (tag < 0)
        return e;
    Expr *c = expr_new(EX_COERCE, e->span);
    c->u.coerce.e = e;
    c->u.coerce.target = target;
    c->coerce_tag = tag;
    c->type = target;
    return c;
}

/* ---- generic monomorphization ---- */

static void register_decl(Sema *se, Decl *d);
static void check_fn(Sema *se, Decl *d);

static Sym *find_method(Sema *se, const char *sname, const char *mname)
{
    for (size_t i = 0; i < se->methods.len; i++) {
        if ((zstr_eq(se->methods.items[i].sname, sname) || se->methods.items[i].sname == sname)
            && (zstr_eq(se->methods.items[i].mname, mname) || se->methods.items[i].mname == mname))
            return &se->methods.items[i].sym;
    }
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
        /* Concrete builtin and module-private types in a receiver are not
         * method type params: Map[String, V] binds only V. */
        if (t->name && zstr_eq(t->name, "String"))
            return;
        if (t->name)
            for (size_t i = 0; i < se->modtypes.len; i++)
                if (se->modtypes.items[i].modname == se->curmod
                    && se->modtypes.items[i].name == t->name)
                    return;
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
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->regname && d->regname == name)
            return d;
        if (d->kind == DL_TYPE ? d->u.type.name == name
                               : d->u.fn.name == name)
            return d;
    }
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
                sym = scope_find(se->scope, str_intern_c(sep + 2));
        }
    }
    if (!sym || sym->kind != SYM_TYPE || !sym->is_template || !sym->decl)
        diag_fatal_global(E0801, "'%s' is not a generic type", tname);
    if (se->inst_depth >= 64)
        /* Box[T] containing Box[Box[T]] never reaches a finite layout */
        fatal_at(E0410, sym->decl->span,
                 "recursive generic type '%s' expands without bound; "
                 "break the cycle with a concrete type",
                 sym->decl->u.type.name);
    se->inst_depth++;
    /* canonicalize qualified names: the key must match receiver
     * instantiations built inside the defining module */
    tname = sym->decl->u.type.name;
    StrList *tps = &sym->decl->u.type.tparams;
    if (tps->len != args->len)
        diag_fatal_global(E0802, "'%s' expects %lu type argument(s), got %lu", tname,
              (unsigned long)tps->len, (unsigned long)args->len);
    char *key = inst_key(tname, args);
    Decl *existing = find_decl_by_name(se, str_intern_c(key));
    if (existing) {
        se->inst_depth--;
        return type_struct(existing->u.type.name);
    }
    const char *tmpl_name = sym->decl->u.type.name;
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
    it->tmpl = tmpl_name;
    for (size_t i = 0; i < args->len; i++)
        da_push(&it->inst_args, args->items[i]);
    if (!se->prog->freestanding) {
        Sym *free_sym = find_method(se, tmpl_name, "free");
        if (free_sym && free_sym->is_template) {
            char mkey[512];
            snprintf(mkey, sizeof(mkey), "%s__free", d->u.type.name);
            instantiate_method(se, free_sym, mkey, tmpl_name, args);
        }
    }
    se->inst_depth--;
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
    case ST_FOR_RANGE:
        n->u.forr.name = s->u.forr.name;
        n->u.forr.ty = s->u.forr.ty
                           ? subst_type(se, s->u.forr.ty, tps, args) : NULL;
        n->u.forr.start = clone_expr(se, s->u.forr.start, tps, args);
        n->u.forr.end = clone_expr(se, s->u.forr.end, tps, args);
        n->u.forr.body = clone_block(se, s->u.forr.body, tps, args);
        break;
    case ST_FOR_EACH:
        n->u.foreach.name = s->u.foreach.name;
        n->u.foreach.ty = s->u.foreach.ty
                              ? subst_type(se, s->u.foreach.ty, tps, args)
                              : NULL;
        n->u.foreach.iter = clone_expr(se, s->u.foreach.iter, tps, args);
        n->u.foreach.body = clone_block(se, s->u.foreach.body, tps, args);
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
    /* parser-set state the sema re-check will not re-derive */
    n->comp_op = e->comp_op;
    n->method_call = e->method_call;
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
    case EX_SLICE:
        n->u.slice.base = clone_expr(se, e->u.slice.base, tps, args);
        n->u.slice.start = clone_expr(se, e->u.slice.start, tps, args);
        n->u.slice.end = clone_expr(se, e->u.slice.end, tps, args);
        break;
    case EX_ARR_LIT:
        for (size_t i = 0; i < e->u.elems.len; i++)
            da_push(&n->u.elems,
                    clone_expr(se, e->u.elems.items[i], tps, args));
        break;
    case EX_ORELSE:
        n->u.orelse.lhs = clone_expr(se, e->u.orelse.lhs, tps, args);
        n->u.orelse.val = clone_expr(se, e->u.orelse.val, tps, args);
        n->u.orelse.jump =
            e->u.orelse.jump ? clone_stmt(se, e->u.orelse.jump, tps, args)
                             : NULL;
        n->coerce_tag = e->coerce_tag; /* payload index rides along */
        break;
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
    d->modname = sym->decl->modname;
    d->u.fn.name = keyi;
    d->u.fn.srcname = sym->decl->u.fn.srcname ? sym->decl->u.fn.srcname
                                              : sym->decl->u.fn.name;
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
    const char *saved_mod = se->curmod;
    se->curmod = d->modname;
    register_decl(se, d);
    /* the clone resolves names from the template's own world: its
     * module's aliases plus globals, never the caller's live locals
     * (a caller binding named 'c' must not shadow a cimport alias) */
    Scope *saved_scope = se->scope;
    while (se->scope->parent)
        se->scope = se->scope->parent;
    push_module_scope(se, d->modname);
    check_fn(se, d);
    se->curmod = saved_mod;
    pop_scope(se);
    se->scope = saved_scope;
    return scope_find(se->scope, keyi);
}

static Sym *instantiate_fn(Sema *se, const char *fname, TypeList *args,
                           Span span)
{
    Sym *sym = scope_find(se->scope, fname);
    if (!sym || sym->kind != SYM_FN || !sym->is_template || !sym->decl)
        fatal_at(E0801, span, "'%s' is not a generic function",
                 fn_display_name(sym));
    StrList *tps = &sym->decl->u.fn.tparams;
    if (tps->len != args->len)
        fatal_at(E0802, span, "'%s' expects %lu type argument(s), got %lu",
                 fn_display_name(sym),
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
        diag_fatal_global(E0408, "unknown generic struct '%s'", tmpl_sname);
    if (td->u.type.tparams.len != inst_args->len)
        diag_fatal_global(E0802, "'%s' instantiation arity mismatch", tmpl_sname);
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
            diag_fatal_global(E0804, "method type parameter '%s' is not bound by the receiver",
                  want);
    }
    StrList *tps = &m->decl->u.fn.tparams;
    return instantiate_fn_from(se, m, tps, &margs, key);
}

static void require_bool(Expr *cond)
{
    if (cond->type && cond->type->kind == TY_BOOL)
        return;
    fatal_at(E0604, cond->span, "condition must be bool, got %s",
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
    fatal_at(E0601, a->span, "type mismatch for '%s': %s vs %s (use an 'as' cast)",
             op, type_spelling(a->type), type_spelling(b->type));
}

// user-facing name: the source spelling, never the mangled registration
static const char *fn_display_name(Sym *fn)
{
    if (fn && fn->decl && fn->decl->kind == DL_FN)
        return fn->decl->u.fn.srcname ? fn->decl->u.fn.srcname
                                      : fn->decl->u.fn.name;
    return fn ? fn->name : "?";
}

// binds a type parameter by unifying a parameter shape against an
// argument; pointers, borrows, slices, and arrays look through
static Type *infer_tparam(Type *pt, Type *at, const char *tp)
{
    if (!pt || !at)
        return NULL;
    if (pt->kind == TY_STRUCT && zstr_eq(pt->name, tp))
        return at;
    switch (pt->kind) {
    case TY_PTR:
    case TY_SLICE:
    case TY_ARRAY:
        if (at->kind != pt->kind)
            return NULL;
        return infer_tparam(pt->base, at->base, tp);
    case TY_REF:
    case TY_MUTREF:
        if (at->kind == TY_REF || at->kind == TY_MUTREF
            || at->kind == TY_PTR)
            return infer_tparam(pt->base, at->base, tp);
        // auto-borrow: a value argument binds T to the value type
        return infer_tparam(pt->base, at, tp);
    default:
        return NULL;
    }
}

/* Modules that cannot be checked in a freestanding build: their bodies
 * use hosted facilities, so only call sites are diagnosed. */
static int hosted_only_module(const char *mod)
{
    return mod && (zstr_eq(mod, "mem") || zstr_eq(mod, "vec")
                   || zstr_eq(mod, "map") || zstr_eq(mod, "arena")
                   || zstr_eq(mod, "fs") || zstr_eq(mod, "env")
                   || zstr_eq(mod, "time") || zstr_eq(mod, "io")
                   || zstr_eq(mod, "proc") || zstr_eq(mod, "strsplit"));
}

/* std::math mixes portable helpers with libm-backed functions.  Only the
 * libm and rand() callers are unavailable without a hosted libc. */
static int math_hosted_fn(const char *name)
{
    if (!name)
        return 0;
    return zstr_eq(name, "randint") || zstr_eq(name, "fmin")
           || zstr_eq(name, "fmax") || zstr_eq(name, "fclamp")
           || zstr_eq(name, "fabs") || zstr_eq(name, "pow")
           || zstr_eq(name, "floor") || zstr_eq(name, "ceil")
           || zstr_eq(name, "round") || zstr_eq(name, "sin")
           || zstr_eq(name, "cos") || zstr_eq(name, "tan");
}

static const char *fn_srcname(Decl *d)
{
    if (!d || d->kind != DL_FN)
        return NULL;
    return d->u.fn.srcname ? d->u.fn.srcname : d->u.fn.name;
}

static int decl_is_hosted_only(Decl *d)
{
    if (!d || d->kind != DL_FN || !d->modname)
        return 0;
    if (zstr_eq(d->modname, "math"))
        return math_hosted_fn(fn_srcname(d));
    return zstr_eq(d->modname, "strsplit");
}

static Decl *find_decl_by_regname(Sema *se, const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind == DL_FN && d->regname && d->regname == name)
            return d;
    }
    return NULL;
}

/* Hosted-only standard modules fail here with a platform explanation
 * instead of compiling into an object that cannot link freestanding. */
static void reject_hosted_decl(Sema *se, Decl *decl, Span span)
{
    if (!se->prog->freestanding || !decl || decl->kind != DL_FN
        || !decl->modname)
        return;
    const char *mod = decl->modname;
    if (zstr_eq(mod, "vec") || zstr_eq(mod, "map")
        || zstr_eq(mod, "arena"))
        fatal_at(E0306, span,
                 "std::%s depends on std::mem and is unavailable in "
                 "freestanding builds", mod);
    if (zstr_eq(mod, "mem"))
        fatal_at(E0306, span,
                 "std::mem requires a hosted allocator; freestanding builds "
                 "must provide the allocation hooks in runtime/zov_rt.h");
    if (zstr_eq(mod, "fs"))
        fatal_at(E0306, span,
                 "std::fs requires an operating-system file API and is "
                 "unavailable in freestanding builds");
    if (zstr_eq(mod, "env"))
        fatal_at(E0306, span,
                 "std::env requires a hosted process and is unavailable "
                 "in freestanding builds");
    if (zstr_eq(mod, "time"))
        fatal_at(E0306, span,
                 "std::time requires a platform timer and is unavailable "
                 "in freestanding builds");
    if (zstr_eq(mod, "proc"))
        fatal_at(E0306, span,
                 "std::proc requires a hosted POSIX process API and is "
                 "unavailable in freestanding builds");
    if (zstr_eq(mod, "io"))
        fatal_at(E0306, span,
                 "std::io requires a platform output interface and is "
                 "unavailable in freestanding builds");
    if (zstr_eq(mod, "math") && math_hosted_fn(fn_srcname(decl)))
        fatal_at(E0306, span,
                 "std::math::%s uses the hosted C math library; pure math "
                 "helpers remain available in freestanding builds",
                 fn_srcname(decl));
    if (zstr_eq(mod, "strsplit"))
        fatal_at(E0306, span,
                 "std::strsplit allocates a Vec; use std::str for "
                 "zero-copy trim, prefix, suffix, and byte search");
}

static void reject_hosted_module(Sema *se, Sym *fn, Span span)
{
    reject_hosted_decl(se, fn->decl, span);
}

static Type *check_call_user(Sema *se, Expr *e, Sym *fn)
{
    reject_hosted_module(se, fn, e->span);
    if (e->u.call.args.len != fn->params.len)
        fatal_at(E0602, e->span, "'%s' expects %lu argument(s), got %lu",
                 fn_display_name(fn),
                 (unsigned long)fn->params.len,
                 (unsigned long)e->u.call.args.len);
    for (size_t i = 0; i < fn->params.len; i++) {
        Expr *arg = e->u.call.args.items[i];
        Type *pt = fn->params.items[i].ty;
        check_expr(se, arg, pt);
        reject_indexed_move(se, arg);
        if (pt->kind == TY_UNION) {
            e->u.call.args.items[i] = maybe_coerce(arg, pt);
            continue;
        }
        if (is_unknown(arg->type))
            continue;
        if (null_fits(arg, pt))
            continue;
        if (pt->kind == TY_REF && arg->type && arg->type->kind == TY_MUTREF
            && type_eq(arg->type->base, pt->base))
            continue;
        if (!type_eq(arg->type, pt))
            fatal_at(E0601, arg->span, "argument %lu of '%s': expected %s, got %s",
                     (unsigned long)(i + 1), fn_display_name(fn),
                     type_spelling(pt),
                     type_spelling(arg->type));
    }
    if (fn->decl && fn->decl->kind == DL_FN && fn->decl->modname
        && zstr_eq(fn->decl->modname, "io") && !fn->decl->u.fn.recv) {
        const char *src = fn->decl->u.fn.srcname
                              ? fn->decl->u.fn.srcname
                              : fn->decl->u.fn.name;
        if (zstr_eq(src, "print") || zstr_eq(src, "println")
            || zstr_eq(src, "eprint") || zstr_eq(src, "eprintln")) {
            if (se->prog->freestanding)
                fatal_at(E0306, e->span,
                         "std::io is unavailable in freestanding builds; "
                         "use a platform output hook");
            e->builtin_print = zstr_eq(src, "print") ? 1
                             : zstr_eq(src, "println") ? 2
                             : zstr_eq(src, "eprint") ? 3 : 4;
        }
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
        /* void * is the C FFI wildcard for any object pointer */
        if (at->kind == TY_PTR && type_eq(at->base, type_prim(TY_VOID)))
            return;
        if (at->kind == TY_PTR && !at->nullable && type_eq(at->base, pt->base))
            return;
        if ((at->kind == TY_REF || at->kind == TY_MUTREF)
            && type_eq(at->base, pt->base))
            return;
        if (at->kind == TY_STR
            && (pt->base->kind == TY_I8 || pt->base->kind == TY_U8))
            return;
        if (at->kind == TY_PTR && at->nullable)
            fatal_at(E0607, arg->span,
                     "cannot pass nullable %s to '%s' parameter %lu: "
                     "narrow it via match or unwrap",
                     type_spelling(at), fname, (unsigned long)idx);
        fatal_at(E0601, arg->span, "argument %lu of '%s': expected %s, got %s",
                 (unsigned long)idx, fname, type_spelling(pt),
                 type_spelling(at));
    }
    if (pt->kind == TY_PTR && pt->nullable) {
        if (null_fits(arg, pt))
            return;
        if (at->kind == TY_PTR && type_eq(at->base, type_prim(TY_VOID)))
            return;
        if (at->kind == TY_PTR && type_eq(at->base, pt->base))
            return;
        if ((at->kind == TY_REF || at->kind == TY_MUTREF)
            && type_eq(at->base, pt->base))
            return;
        if (at->kind == TY_STR
            && (pt->base->kind == TY_I8 || pt->base->kind == TY_U8))
            return;
        fatal_at(E0601, arg->span, "argument %lu of '%s': expected %s, got %s",
                 (unsigned long)idx, fname, type_spelling(pt),
                 type_spelling(at));
    }
    if (type_eq(at, pt))
        return;
    fatal_at(E0601, arg->span, "argument %lu of '%s': expected %s, got %s",
             (unsigned long)idx, fname, type_spelling(pt), type_spelling(at));
}

static Type *check_call_ns(Sema *se, Expr *e, Expr *callee)
{
    Sym *ns = ns_lookup(se, callee->u.nident.ns);
    if (!ns)
        fatal_at(E0405, callee->span, "unknown namespace '%s'", callee->u.nident.ns);
    if (ns->kind != SYM_NS || !ns->hdr)
        fatal_at(E0405, callee->span, "'%s' is not a C namespace",
                 callee->u.nident.ns);
    if (c_ns_private(se, ns))
        fatal_at(E0306, callee->span,
                 "C symbol '%s::%s' is private to %s and unavailable "
                 "to importers; use an exported Zov wrapper",
                 callee->u.nident.ns, callee->u.nident.name,
                 mod_display_name(ns->modname));
    CMember *fn = cimport_find(ns->hdr, callee->u.nident.name);
    if (!fn)
        fatal_at(E0402, callee->span, "unknown C symbol '%s::%s'",
                 callee->u.nident.ns, callee->u.nident.name);
    if (fn->kind != CM_FN)
        fatal_at(E0406, callee->span, "'%s::%s' is not a function",
                 callee->u.nident.ns, callee->u.nident.name);
    if (e->u.call.args.len < fn->params.len
        || (!fn->variadic && e->u.call.args.len > fn->params.len))
        fatal_at(E0602, e->span, "'%s' expects %lu argument(s)%s, got %lu",
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
                fatal_at(E0604, arg->span, "cannot pass %s through '...'",
                         type_spelling(at));
        }
    }
    e->type = fn->ret; /* callers read e->type for concat/arg typing */
    return fn->ret;
}

static Type *check_expr(Sema *se, Expr *e, Type *hint)
{
    switch (e->kind) {
    case EX_INT: {
        e->type = int_literal_type(e, hint);
        check_int_literal(e, e->type);
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
        if (!sym) {
            const char *hint = scope_suggest(se, e->u.ident);
            char help[256];
            if (hint)
                snprintf(help, sizeof(help), "did you mean '%s'?", hint);
            diag_fatal_at("E0402", e->span, NULL, NULL, NULL,
                          hint ? help : NULL,
                          "undefined symbol '%s'", e->u.ident);
        }
        if (sym->kind != SYM_VAR)
            fatal_at(E0406, e->span, "'%s' is not a variable", e->u.ident);
        sym->read_count++;
        e->type = sym->type;
        return e->type;
    }
    case EX_NS_IDENT: {
        Sym *ns = ns_lookup(se, e->u.nident.ns);
        if (!ns)
            fatal_at(E0405, e->span, "unknown namespace '%s'", e->u.nident.ns);
        /* enum variant: Color::Red resolves through the type registry */
        if (ns->kind == SYM_TYPE && ns->type && ns->type->kind == TY_ENUM) {
            Decl *ed = find_enum_decl(se, e->u.nident.ns);
            for (size_t k = 0; ed && k < ed->u.type.variants.len; k++)
                if (ed->u.type.variants.items[k].name == e->u.nident.name) {
                    e->type = ns->type;
                    char cbuf[256];
                    snprintf(cbuf, sizeof(cbuf), "Zov_%s_%s",
                             e->u.nident.ns, e->u.nident.name);
                    e->variant_c = str_intern_c(cbuf);
                    return e->type;
                }
            fatal_at(E0407, e->span, "enum '%s' has no variant '%s'",
                     e->u.nident.ns, e->u.nident.name);
        }
        if (ns->kind != SYM_NS || !ns->hdr)
            fatal_at(E0405, e->span, "'%s' is not a namespace", e->u.nident.ns);
        if (c_ns_private(se, ns))
            fatal_at(E0306, e->span,
                     "C symbol '%s::%s' is private to %s and "
                     "unavailable to importers; use an exported Zov wrapper",
                     e->u.nident.ns, e->u.nident.name,
                     mod_display_name(ns->modname));
        CMember *m = cimport_find(ns->hdr, e->u.nident.name);
        if (!m)
            fatal_at(E0402, e->span, "unknown symbol '%s::%s'", e->u.nident.ns,
                     e->u.nident.name);
        if (m->kind == CM_CONST) {
            e->type = m->ret;
            return e->type;
        }
        fatal_at(E0406, e->span, "'%s::%s' is not a value", e->u.nident.ns,
                 e->u.nident.name);
        return NULL;
    }
    case EX_ASSIGN: {
        Expr *lhs = e->u.assign.lhs;
        Type *lt;
        if (lhs->kind == EX_IDENT) {
            Sym *sym = scope_find(se->scope, lhs->u.ident);
            if (!sym)
                fatal_at(E0402, lhs->span, "undefined symbol '%s'", lhs->u.ident);
            if (sym->kind != SYM_VAR)
                fatal_at(E0406, lhs->span, "'%s' is not a variable", lhs->u.ident);
            if (!sym->mut)
                fatal_at(E0516, lhs->span,
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
            fatal_at(E0707, lhs->span, "invalid assignment target");
            return NULL;
        }
        Type *rt = check_expr(se, e->u.assign.rhs, lt);
        reject_indexed_move(se, e->u.assign.rhs);
        if (!e->comp_op && lt->kind == TY_UNION) {
            e->u.assign.rhs = maybe_coerce(e->u.assign.rhs, lt);
            rt = e->u.assign.rhs->type;
        }
        if (e->comp_op) {
            /* compound forms: the operation must type-check on lt */
            int bit = (e->comp_op == TK_AMP_EQ || e->comp_op == TK_PIPE_EQ
                       || e->comp_op == TK_CARET_EQ
                       || e->comp_op == TK_SHL_EQ
                       || e->comp_op == TK_SHR_EQ);
            if (!is_unknown(lt) && !ty_is_int(lt->kind))
                fatal_at(E0709, e->span, "compound assignment requires an "
                         "integer lvalue, got %s", type_spelling(lt));
            if (!bit && !is_unknown(lt) && !ty_is_num(lt->kind))
                fatal_at(E0709, e->span, "compound assignment requires a "
                         "numeric lvalue, got %s", type_spelling(lt));
            (void)rt;
            e->type = lt;
            return e->type;
        }
        if (!is_unknown(rt) && !is_unknown(lt) && !type_eq(lt, rt)
            && !ptr_widens(rt, lt)
            && !null_fits(e->u.assign.rhs, lt))
            fatal_at(E0601, e->span, "cannot assign %s to %s", type_spelling(rt),
                     type_spelling(lt));
        if (lt->kind == TY_ARRAY)
            fatal_at(E0612, e->span,
                     "cannot assign array type %s; assign elements "
                     "individually", type_spelling(lt));
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
        int bitwise = (op == TK_AMP || op == TK_PIPE || op == TK_CARET
                       || op == TK_SHL || op == TK_SHR);
        Type *lt = check_expr(se, e->u.binary.lhs,
                             (comparison || bitwise) ? NULL : hint);
        Type *rt = check_expr(se, e->u.binary.rhs, lt);
        if (bitwise) {
            if (!is_unknown(lt) && !ty_is_int(lt->kind))
                fatal_at(E0604, e->span, "'%s' requires integer operands, got %s",
                         op == TK_AMP ? "&" : op == TK_PIPE ? "|"
                         : op == TK_CARET ? "^" : op == TK_SHL ? "<<"
                                                             : ">>",
                         type_spelling(lt));
            if (!is_unknown(rt) && !ty_is_int(rt->kind))
                fatal_at(E0604, e->u.binary.rhs->span,
                         "shift/bit operands must be integers, got %s",
                         type_spelling(rt));
            require_same(e->u.binary.lhs, e->u.binary.rhs, "<bitwise>");
            e->type = is_unknown(lt) ? unknown() : lt;
            return e->type;
        }
        if (comparison) {
            /* null compares against any nullable pointer */
            int null_cmp = 0;
            if (e->u.binary.lhs->kind == EX_NULL && rt->kind == TY_PTR
                && rt->nullable)
                null_cmp = 1;
            if (e->u.binary.rhs->kind == EX_NULL && lt->kind == TY_PTR
                && lt->nullable)
                null_cmp = 1;
            if (null_cmp) {
                e->type = type_prim(TY_BOOL);
                return e->type;
            }
        }
        if (op == TK_PLUS && (is_stringish(lt)
                              || is_stringish(e->u.binary.rhs->type))) {
            /* str/String + T scripting chain: any printable T.  The
             * result owns a fresh heap buffer and must be released. */
            if (!is_printable(lt) || !is_printable(e->u.binary.rhs->type))
                fatal_at(E0604, e->span, "cannot concatenate %s with %s",
                         type_spelling(lt),
                         type_spelling(e->u.binary.rhs->type));
            if (se->prog->freestanding)
                fatal_at(E0306, e->span,
                         "String concatenation needs an allocator; "
                         "freestanding builds must provide one explicitly");
            e->type = type_struct("String");
            return e->type;
        }
        if (comparison && (is_stringish(lt)
                           || is_stringish(e->u.binary.rhs->type))) {
            if (!is_stringish(lt) || !is_stringish(e->u.binary.rhs->type))
                fatal_at(E0604, e->span,
                         "cannot compare %s with %s", type_spelling(lt),
                         type_spelling(e->u.binary.rhs->type));
            e->type = type_prim(TY_BOOL);
            return e->type;
        }
        require_same(e->u.binary.lhs, e->u.binary.rhs, "<binary>");
        if (comparison) {
            Type *t = e->u.binary.lhs->type;
            /* ==/!= generalize to str (memcmp) and enums; <,<=,>,>=
             * also order strings lexically via zov_str_cmp */
            int eq_ne = (op == TK_EQ || op == TK_NEQ);
            int ok = ty_is_num(t->kind) || t->kind == TY_CHAR
                     || is_ptrish(t)
                     || (eq_ne && (t->kind == TY_STR || t->kind == TY_ENUM
                                   || t->kind == TY_BOOL))
                     || (!eq_ne && t->kind == TY_STR);
            if (!is_unknown(t) && !ok)
                fatal_at(E0604, e->span,
                         "relational operators require numeric or str "
                         "operands");
            e->type = type_prim(TY_BOOL);
        } else {
            Type *t = e->u.binary.lhs->type;
            if (!is_unknown(t) && !ty_is_num(t->kind))
                fatal_at(E0604, e->span, "arithmetic requires numeric operands");
            e->type = is_unknown(t) ? unknown() : t;
        }
        return e->type;
    }
    case EX_UNARY: {
        /* fold the sign into a literal so INT_MIN does not overflow its
         * own positive magnitude check */
        if (e->u.unary.op == TK_MINUS && e->u.unary.e->kind == EX_INT) {
            Expr *lit = e->u.unary.e;
            Type *lt = int_literal_type(lit, hint);
            uint64_t max;
            int is_signed;
            lit->type = lt;
            e->type = lt;
            if (int_bounds(lt, &max, &is_signed) && lit->u.ilit.v > max) {
                if (!(is_signed && max != UINT64_MAX && lit->u.ilit.v == max + 1))
                    fatal_at(E0701, lit->span,
                             "integer literal %llu does not fit in %s",
                             (unsigned long long)lit->u.ilit.v,
                             type_spelling(lt));
            }
            return e->type;
        }
        Type *t = check_expr(se, e->u.unary.e, hint);
        switch (e->u.unary.op) {
        case TK_MINUS:
            if (!is_unknown(t) && !ty_is_num(t->kind))
                fatal_at(E0604, e->span, "unary '-' requires a numeric operand");
            e->type = t;
            break;
        case TK_TILDE:
            if (!is_unknown(t) && !ty_is_int(t->kind))
                fatal_at(E0604, e->span,
                         "unary '~' requires an integer operand, got %s",
                         type_spelling(t));
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
            fatal_at(E0902, e->span, "unsupported unary operator");
        }
        return e->type;
    }
    case EX_CALL: {
        Expr *callee = e->u.call.callee;
        /* std::mem::drop: explicit destruction, lowered per argument type.
         * Accept both drop(v) and drop[T](v); the type argument is
         * redundant because the place's type is already known. */
        const char *drop_target = NULL;
        if (callee->kind == EX_NS_IDENT)
            drop_target = zov_ns_target(se, callee->u.nident.ns,
                                        callee->u.nident.name);
        else if (callee->kind == EX_GENERIC
                 && callee->u.generic.base->kind == EX_NS_IDENT)
            drop_target = zov_ns_target(
                se, callee->u.generic.base->u.nident.ns,
                callee->u.generic.base->u.nident.name);
        if (drop_target && is_mem_drop_target(se, drop_target)) {
            if (e->u.call.args.len != 1)
                fatal_at(E0602, e->span,
                         "mem::drop takes exactly one argument");
            Expr *arg = e->u.call.args.items[0];
            check_expr(se, arg, NULL);
            reject_indexed_move(se, arg);
            if (!drop_place_kind(arg))
                fatal_at(E0605, arg->span,
                         "mem::drop requires a place expression "
                         "(variable, field, index, or dereference)");
            if (drop_through_reference(arg))
                fatal_at(E0605, arg->span,
                         "mem::drop cannot destroy through a reference; "
                         "drop the owning binding directly");
            /* A type with a by-value free() method releases its own
             * storage; instantiate the generic method now so codegen can
             * call it from the drop helper. */
            Type *at = arg->type;
            if (at && at->kind == TY_STRUCT && at->tmpl
                && at->inst_args.len > 0) {
                Sym *m = find_method(se, at->tmpl, "free");
                if (m && m->is_template) {
                    char key[512];
                    snprintf(key, sizeof(key), "%s__free", at->name);
                    instantiate_method(se, m, key, at->tmpl,
                                       &at->inst_args);
                }
            }
            e->builtin_drop = 1;
            e->type = type_prim(TY_VOID);
            return e->type;
        }
        if (callee->kind == EX_NS_IDENT
            && zstr_eq(callee->u.nident.ns, "String")) {
            if (zstr_eq(callee->u.nident.name, "new")) {
                if (e->u.call.args.len != 0)
                    fatal_at(E0602, e->span, "String::new takes no arguments");
                e->builtin_str_ctor = 2;
                e->type = type_struct("String");
                return e->type;
            }
            if (zstr_eq(callee->u.nident.name, "fromowned")
                || zstr_eq(callee->u.nident.name, "from_owned")) {
                if (e->u.call.args.len != 3)
                    fatal_at(E0602, e->span,
                             "String::fromowned takes data, len, and cap");
                Type *dt = check_expr(se, e->u.call.args.items[0], NULL);
                if (!is_unknown(dt)
                    && !(dt->kind == TY_PTR && dt->base
                         && dt->base->kind == TY_U8))
                    fatal_at(E0601, e->u.call.args.items[0]->span,
                             "String::fromowned expects *u8 data, got %s",
                             type_spelling(dt));
                for (size_t ai = 1; ai < 3; ai++) {
                    Type *at = check_expr(se, e->u.call.args.items[ai],
                                          type_prim(TY_USIZE));
                    if (!is_unknown(at) && at->kind != TY_USIZE)
                        fatal_at(E0601, e->u.call.args.items[ai]->span,
                                 "String::fromowned expects usize length "
                                 "and capacity, got %s", type_spelling(at));
                }
                e->builtin_str_ctor = 3;
                e->type = type_struct("String");
                return e->type;
            }
            if (zstr_eq(callee->u.nident.name, "from")) {
                if (e->u.call.args.len != 1)
                    fatal_at(E0602, e->span, "String::from takes one argument");
                Type *at = check_expr(se, e->u.call.args.items[0],
                                      type_prim(TY_STR));
                if (!is_unknown(at) && !is_stringish(at))
                    fatal_at(E0601, e->u.call.args.items[0]->span,
                             "String::from expects str or String, got %s",
                             type_spelling(at));
                if (se->prog->freestanding)
                    fatal_at(E0306, e->span,
                             "String allocation is unavailable in "
                             "freestanding mode; provide an allocator");
                e->builtin_str_ctor = 1;
                e->type = type_struct("String");
                return e->type;
            }
            fatal_at(E0407, callee->span, "String has no associated function '%s'",
                     callee->u.nident.name);
        }
        if (callee->kind == EX_NS_IDENT) {
            const char *tgt = zov_ns_target(se, callee->u.nident.ns,
                                            callee->u.nident.name);
            if (tgt) {
                /* module fn: rewrite to the mangled registered symbol */
                Expr *id = expr_new(EX_IDENT, callee->span);
                id->u.ident = tgt;
                e->u.call.callee = id;
                callee = id;
            } else {
                ns_call_diag(se, callee, callee->span);
            }
        }
        if (callee->kind == EX_GENERIC
            && callee->u.generic.base->kind == EX_NS_IDENT) {
            const char *tgt = zov_ns_target(
                se, callee->u.generic.base->u.nident.ns,
                callee->u.generic.base->u.nident.name);
            if (tgt) {
                /* reject hosted-only modules before instantiating their
                 * bodies, so the diagnostic names the user's call site */
                reject_hosted_decl(se, find_decl_by_regname(se, tgt),
                                   callee->span);
                Expr *id = expr_new(EX_IDENT, callee->u.generic.base->span);
                id->u.ident = tgt;
                callee->u.generic.base = id;
            } else {
                ns_call_diag(se, callee->u.generic.base, callee->span);
            }
        }
        if (callee->kind == EX_FIELD) {
            /* expr.method(...): receiver resolution with auto-borrowing */
            Type *bt = check_expr(se, callee->u.field.base, NULL);
            if (!bt || (bt->kind != TY_STRUCT && bt->kind != TY_REF
                        && bt->kind != TY_MUTREF))
                fatal_at(E0414, callee->span,
                         "method calls require a struct receiver");
            Type *st = bt;
            while (st && (st->kind == TY_REF || st->kind == TY_MUTREF))
                st = st->base;
            if (!st || st->kind != TY_STRUCT || !st->name)
                fatal_at(E0414, callee->span, "methods require a struct receiver");
            if (is_string_type(st)) {
                if (e->u.call.args.len != 0)
                    fatal_at(E0602, e->span, "String.%s takes no arguments",
                             callee->u.field.name);
                Expr *recv = callee->u.field.base;
                if (zstr_eq(callee->u.field.name, "free"))
                    reject_indexed_move(se, recv);
                if (bt->kind == TY_REF || bt->kind == TY_MUTREF) {
                    /* String methods are value intrinsics: expose the
                     * pointee so codegen never passes a pointer where a
                     * String value is expected. */
                    Expr *dr = expr_new(EX_DEREF, recv->span);
                    dr->u.deref = recv;
                    dr->type = st;
                    recv = dr;
                }
                e->str_recv = recv;
                if (zstr_eq(callee->u.field.name, "free")) {
                    e->builtin_str_method = 1;
                    e->type = type_prim(TY_VOID);
                    return e->type;
                }
                if (zstr_eq(callee->u.field.name, "view")) {
                    e->builtin_str_method = 2;
                    e->type = type_prim(TY_STR);
                    return e->type;
                }
                if (zstr_eq(callee->u.field.name, "len")) {
                    e->builtin_str_method = 3;
                    e->type = type_prim(TY_USIZE);
                    return e->type;
                }

                fatal_at(E0407, callee->span, "String has no method '%s'",
                         callee->u.field.name);
            }
            const char *sname = st->tmpl ? st->tmpl : st->name;
            Sym *m = find_method(se, sname, callee->u.field.name);
            if (!m)
                fatal_at(E0407, callee->span, "struct '%s' has no method '%s'",
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
                    if (bt->kind == TY_MUTREF && rp->ty->kind == TY_REF
                        && type_eq(bt->base, rp->ty->base)) {
                        /* allowed: &mut T passed to &T method receiver */
                    } else if (!type_eq(bt, rp->ty))
                        fatal_at(E0414, recv->span,
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
            e->method_call = 1;
            ExprList nargs = {0};
            da_push(&nargs, recv);
            for (size_t i = 0; i < e->u.call.args.len; i++)
                da_push(&nargs, e->u.call.args.items[i]);
            e->u.call.args.items = nargs.items;
            e->u.call.args.len = nargs.len;
            e->u.call.args.cap = nargs.cap;
            Expr *id = expr_new(EX_IDENT, callee->span);
            id->u.ident = (m->decl && m->decl->regname)
                              ? m->decl->regname : m->name;
            e->u.call.callee = id;
            e->type = check_call_user(se, e, m);
            if (zstr_eq(callee->u.field.name, "get") && m->decl
                && m->decl->modname
                && (zstr_eq(m->decl->modname, "vec")
                    || zstr_eq(m->decl->modname, "map"))
                && type_owns_resource(se, e->type, 0)) {
                Type *shown = e->type;
                if (shown->kind == TY_UNION)
                    for (size_t mi = 0; mi < shown->members.len; mi++)
                        if (type_owns_resource(se, shown->members.items[mi],
                                               0)) {
                            shown = shown->members.items[mi];
                            break;
                        }
                fatal_at(E0611, e->span,
                         "std::%s::get would shallow-copy %s; use "
                         "getref() for borrowed access or an "
                         "ownership-transferring API",
                         m->decl->modname, type_spelling(shown));
            }
            return e->type;
        }
        if (callee->kind == EX_GENERIC
            && callee->u.generic.base->kind == EX_IDENT
            && (zstr_eq(callee->u.generic.base->u.ident, "hashval")
                || zstr_eq(callee->u.generic.base->u.ident, "hash_val"))
            && !scope_find(se->scope, "hashval")
            && !scope_find(se->scope, "hash_val")) {
            if (callee->u.generic.targs.len != 1)
                fatal_at(E0806, callee->span,
                         "hashval[T] takes one type argument");
            Type *kt = resolve_type(se, callee->u.generic.targs.items[0]);
            if (e->u.call.args.len != 1)
                fatal_at(E0806, e->span, "hashval[T] hashes exactly one value");
            Type *at = check_expr(se, e->u.call.args.items[0], kt);
            if (kt->kind != TY_STR && kt->kind != TY_ENUM
                && !ty_is_int(kt->kind) && !is_string_type(kt))
                fatal_at(E0806, callee->span,
                         "hashval supports str, String, integers, and "
                         "enums, not %s", type_spelling(kt));
            if (!is_unknown(at) && !type_eq(at, kt))
                fatal_at(E0806, e->u.call.args.items[0]->span,
                         "hashval[%s] argument must be %s, got %s",
                         type_spelling(kt), type_spelling(kt),
                         type_spelling(at));
            e->hash_ty = kt;
            e->type = type_prim(TY_U64);
            return e->type;
        }
        if (callee->kind == EX_GENERIC
            && callee->u.generic.base->kind == EX_IDENT
            && zstr_eq(callee->u.generic.base->u.ident, "sizeof")
            && !scope_find(se->scope, "sizeof")) {
            if (callee->u.generic.targs.len != 1)
                fatal_at(E0806, callee->span, "sizeof[T]() takes one type argument");
            e->szarg = resolve_type(se, callee->u.generic.targs.items[0]);
            e->type = type_prim(TY_USIZE);
            return e->type;
        }
        if (callee->kind == EX_GENERIC) {
            /* swap in the specialized ident, then re-enter as a plain
             * user-fn call against the instantiated signature */
            if (callee->u.generic.base->kind != EX_IDENT)
                fatal_at(E0804, callee->span,
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
                if (zstr_eq(callee->u.ident, "zov_str_from_raw")) {
                    if (e->u.call.args.len != 2)
                        fatal_at(E0806, e->span,
                                 "zov_str_from_raw takes a pointer and "
                                 "a length");
                    Type *pt = check_expr(se, e->u.call.args.items[0],
                                          NULL);
                    Type *nt = check_expr(se, e->u.call.args.items[1],
                                         type_prim(TY_USIZE));
                    if (!is_unknown(pt) && !is_ptrish(pt))
                        fatal_at(E0806, e->u.call.args.items[0]->span,
                                 "zov_str_from_raw needs a pointer");
                    if (!is_unknown(nt) && !ty_is_int(nt->kind))
                        fatal_at(E0604, e->u.call.args.items[1]->span,
                                 "length must be an integer");
                    e->builtin_env_kind = 0;
                    e->type = type_prim(TY_STR);
                    e->str_raw_builtin = 1;
                    return e->type;
                }
                /* runtime-env builtins backing std::env */
                if (zstr_eq(callee->u.ident, "zov_env_args_count")) {
                    if (e->u.call.args.len != 0)
                        fatal_at(E0806, e->span, "zov_env_args_count takes no "
                                          "arguments");
                    e->builtin_env_kind = 1;
                    e->type = type_prim(TY_USIZE);
                    return e->type;
                }
                if (zstr_eq(callee->u.ident, "zov_env_arg_at")) {
                    if (e->u.call.args.len != 1)
                        fatal_at(E0806, e->span, "zov_env_arg_at takes one index");
                    Type *t = check_expr(se, e->u.call.args.items[0],
                                         type_prim(TY_USIZE));
                    if (!is_unknown(t) && !ty_is_int(t->kind))
                        fatal_at(E0604, e->u.call.args.items[0]->span,
                                 "argument index must be an integer");
                    e->builtin_env_kind = 2;
                    e->type = type_prim(TY_STR);
                    return e->type;
                }
                if (zstr_eq(callee->u.ident, "zov_env_has")) {
                    if (e->u.call.args.len != 1)
                        fatal_at(E0806, e->span, "zov_env_has takes one name");
                    Type *t = check_expr(se, e->u.call.args.items[0],
                                         type_prim(TY_STR));
                    if (!is_unknown(t) && t->kind != TY_STR)
                        fatal_at(E0806, e->u.call.args.items[0]->span,
                                 "environment names must be str");
                    e->builtin_env_kind = 3;
                    e->type = type_prim(TY_BOOL);
                    return e->type;
                }
                if (zstr_eq(callee->u.ident, "zov_env_get_str")) {
                    if (e->u.call.args.len != 1)
                        fatal_at(E0806, e->span, "zov_env_get_str takes one name");
                    Type *t = check_expr(se, e->u.call.args.items[0],
                                         type_prim(TY_STR));
                    if (!is_unknown(t) && t->kind != TY_STR)
                        fatal_at(E0806, e->u.call.args.items[0]->span,
                                 "environment names must be str");
                    e->builtin_env_kind = 4;
                    e->type = type_prim(TY_STR);
                    return e->type;
                }
                if (zstr_eq(callee->u.ident, "assert")
                    && !scope_find(se->scope, "assert")) {
                    if (e->u.call.args.len != 1)
                        fatal_at(E0602, e->span, "assert(cond) takes one "
                                          "argument");
                    Type *t = check_expr(se, e->u.call.args.items[0], NULL);
                    if (!is_unknown(t) && t->kind != TY_BOOL)
                        fatal_at(E0604, e->u.call.args.items[0]->span,
                                 "assert requires bool, got %s",
                                 type_spelling(t));
                    e->builtin_assert = 1;
                    e->type = type_prim(TY_VOID);
                    return e->type;
                }
                {
                    const char *hint = scope_suggest(se, callee->u.ident);
                    char help[256];
                    if (hint)
                        snprintf(help, sizeof(help), "did you mean '%s'?",
                                 hint);
                    diag_fatal_at("E0401", callee->span, NULL, NULL, NULL,
                                  hint ? help : NULL,
                                  "undefined function '%s'",
                                  callee->u.ident);
                }
            }
            if (fn->kind != SYM_FN)
                fatal_at(E0413, callee->span, "'%s' is not callable",
                         callee->u.ident);
            if (fn->alias_to) {
                /* module alias: rewrite to the registered spelling */
                callee->u.ident = fn->alias_to;
                fn = scope_find(se->scope, fn->alias_to);
                if (!fn)
                    fatal_at(E0412, callee->span, "unresolved module symbol '%s'",
                             callee->u.ident);
            }
            if (fn->is_template) {
                /* bare generic call: infer type args from the arguments */
                StrList *tps = &fn->decl->u.fn.tparams;
                if (e->u.call.args.len < fn->decl->u.fn.params.len)
                    fatal_at(E0602, e->span, "'%s' expects %lu argument(s), "
                             "got %lu", fn_display_name(fn),
                             (unsigned long)fn->decl->u.fn.params.len,
                             (unsigned long)e->u.call.args.len);
                TypeList argtys = {0};
                for (size_t ai = 0; ai < e->u.call.args.len; ai++)
                    da_push(&argtys,
                            check_expr(se, e->u.call.args.items[ai], NULL));
                TypeList inferred = {0};
                int complete = 1;
                size_t width = fn->decl->u.fn.params.len < argtys.len
                    ? fn->decl->u.fn.params.len : argtys.len;
                for (size_t ti = 0; ti < tps->len && complete; ti++) {
                    Type *pick = NULL;
                    for (size_t ai = 0; ai < width; ai++) {
                        Type *at = argtys.items[ai];
                        if (!at || is_unknown(at))
                            continue;
                        pick = infer_tparam(
                            fn->decl->u.fn.params.items[ai].ty, at,
                            tps->items[ti]);
                        if (pick)
                            break;
                    }
                    if (!pick)
                        complete = 0;
                    else
                        da_push(&inferred, pick);
                }
                free(argtys.items);
                if (!complete)
                    fatal_at(E0803, callee->span,
                             "generic function '%s' requires explicit type "
                             "arguments, e.g. %s[T](...)",
                             fn_display_name(fn), fn_display_name(fn));
                Sym *inst = instantiate_fn(se, callee->u.ident, &inferred,
                                           callee->span);
                Expr *id = expr_new(EX_IDENT, callee->span);
                id->u.ident = inst->name;
                e->u.call.callee = id;
                e->type = check_call_user(se, e, inst);
                return e->type;
            }
            e->type = check_call_user(se, e, fn);
            return e->type;
        }
        if (callee->kind == EX_NS_IDENT)
            return check_call_ns(se, e, callee);
        fatal_at(E0413, callee->span, "expression is not callable");
        return NULL;
    }
    case EX_ARR_LIT: {
        Type *elem = NULL;
        if (hint && hint->kind == TY_ARRAY)
            elem = hint->base; /* annotation drives element coercion */
        for (size_t i = 0; i < e->u.elems.len; i++) {
            Type *t = check_expr(se, e->u.elems.items[i], elem);
            reject_indexed_move(se, e->u.elems.items[i]);
            if (!elem)
                elem = t;
            else if (!is_unknown(t) && !type_eq(elem, t))
                fatal_at(E0601, e->u.elems.items[i]->span,
                         "array element %lu: expected %s, got %s",
                         (unsigned long)i, type_spelling(elem),
                         type_spelling(t));
        }
        if (!elem)
            elem = unknown();
        e->type = type_array(elem, e->u.elems.len);
        return e->type;
    }
    case EX_ORELSE: {
        Type *lt = check_expr(se, e->u.orelse.lhs, NULL);
        reject_indexed_move(se, e->u.orelse.lhs);
        Type *result = NULL;
        int payload_idx = -1;
        if (lt->kind == TY_PTR && lt->nullable) {
            result = type_ptr(lt->base, 0);
        } else if (lt->kind == TY_UNION) {
            Type *payload = union_payload(lt);
            if (!payload)
                fatal_at(E0605, e->span,
                         "orelse requires exactly one non-error variant");
            result = payload;
            for (size_t i = 0; i < lt->members.len; i++)
                if (lt->members.items[i] == payload)
                    payload_idx = (int)i;
        } else {
            fatal_at(E0605, e->span,
                     "orelse requires a nullable pointer or a tagged "
                     "union, got %s", type_spelling(lt));
        }
        e->coerce_tag = payload_idx; /* reused: payload member index */
        if (e->u.orelse.jump) {
            /* diverging fallback: defers unroll before the jump */
            check_stmt(se, e->u.orelse.jump);
        } else {
            Type *rt = check_expr(se, e->u.orelse.val, result);
            if (!is_unknown(rt) && !type_eq(result, rt)
                && !ptr_widens(result, rt))
                fatal_at(E0601, e->span, "orelse fallback is %s, expected %s",
                         type_spelling(rt), type_spelling(result));
        }
        e->type = result;
        return e->type;
    }
    case EX_SLICE: {
        Type *bt = check_expr(se, e->u.slice.base, NULL);
        Type *st = e->u.slice.start
                       ? check_expr(se, e->u.slice.start, type_prim(TY_USIZE))
                       : NULL;
        Type *et = e->u.slice.end
                       ? check_expr(se, e->u.slice.end, type_prim(TY_USIZE))
                       : NULL;
        if (st && !is_unknown(st) && !ty_is_int(st->kind))
            fatal_at(E0604, e->u.slice.start->span, "slice start must be an "
                     "integer, got %s", type_spelling(st));
        if (et && !is_unknown(et) && !ty_is_int(et->kind))
            fatal_at(E0604, e->u.slice.end->span, "slice end must be an integer, "
                     "got %s", type_spelling(et));
        if (bt->kind == TY_ARRAY || bt->kind == TY_SLICE) {
            e->type = type_slice(bt->base);
            return e->type;
        }
        if (is_stringish(bt)) {
            e->type = type_prim(TY_STR);
            return e->type;
        }
        if (bt->kind == TY_PTR && !bt->nullable) {
            /* raw pointer slicing: bounds are caller-supplied */
            e->type = type_slice(bt->base);
            return e->type;
        }
        fatal_at(E0605, e->span, "cannot slice %s", type_spelling(bt));
        return NULL;
    }
    case EX_INDEX: {
        Type *bt = check_expr(se, e->u.index.base, NULL);
        Type *it = check_expr(se, e->u.index.index, type_prim(TY_USIZE));
        if (!is_unknown(it) && !ty_is_int(it->kind))
            fatal_at(E0604, e->u.index.index->span,
                     "index must be an integer, got %s", type_spelling(it));
        if (is_unknown(bt))
            return unknown();
        if (is_stringish(bt)) {
            e->type = type_prim(TY_U8);
            return e->type;
        }
        if (bt->kind == TY_SLICE || bt->kind == TY_ARRAY) {
            e->type = bt->base;
            return e->type;
        }
        if (bt->kind == TY_PTR) {
            if (bt->nullable)
                fatal_at(E0605, e->span, "cannot index nullable %s; narrow it "
                                  "via match or unwrap",
                         type_spelling(bt));
            // raw pointer indexing: unchecked, for low-level containers
            e->type = bt->base;
            return e->type;
        }
        fatal_at(E0605, e->span, "cannot index %s", type_spelling(bt));
        return NULL;
    }
    case EX_CAST: {
        Type *t = check_expr(se, e->u.cast.e, e->u.cast.t);
        Type *dst = e->u.cast.t;
        if (!is_unknown(t) && !ty_is_num(t->kind) && t->kind != TY_CHAR
            && t->kind != TY_BOOL && !is_ptrish(t))
            fatal_at(E0603, e->span, "cannot cast %s", type_spelling(t));
        if (!ty_is_num(dst->kind) && dst->kind != TY_CHAR
            && dst->kind != TY_BOOL && !is_ptrish(dst))
            fatal_at(E0603, e->span, "cannot cast to %s", type_spelling(dst));
        if (t->kind == TY_PTR && t->nullable && dst->kind == TY_PTR
            && !dst->nullable)
            fatal_at(E0603, e->span,
                     "cannot cast %s to %s: narrow it via match "
                     "or '!' instead",
                     type_spelling(t), type_spelling(dst));
        e->type = dst;
        return e->type;
    }
    case EX_GENERIC:
        /* call/literal sites handle their own instantiation */
        fatal_at(E0804, e->span, "generic instantiation requires a call or struct "
                          "literal");
        return NULL;
    case EX_DEREF: {
        Type *t = check_expr(se, e->u.deref, NULL);
        if (t->kind == TY_PTR && t->nullable)
            fatal_at(E0605, e->span,
                     "cannot dereference nullable pointer; narrow it first");
        if (t->kind == TY_PTR || t->kind == TY_REF || t->kind == TY_MUTREF) {
            e->type = t->base;
            return e->type;
        }
        fatal_at(E0605, e->span, "cannot dereference %s", type_spelling(t));
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
            if (zstr_eq(e->u.field.name, "ptr")) {
                e->type = type_ptr(type_prim(TY_U8), 0);
                return e->type;
            }
            fatal_at(E0407, e->span, "str has no field '%s'", e->u.field.name);
        }
        if (t->kind == TY_SLICE) {
            if (zstr_eq(e->u.field.name, "len")) {
                e->type = type_prim(TY_USIZE);
                return e->type;
            }
            if (zstr_eq(e->u.field.name, "ptr")) {
                e->type = type_ptr(t->base, 0);
                return e->type;
            }
        }
        if (t->kind == TY_ARRAY && zstr_eq(e->u.field.name, "len")) {
            e->type = type_prim(TY_USIZE);
            return e->type;
        }
        /* references auto-deref for field access */
        while (t && (t->kind == TY_REF || t->kind == TY_MUTREF)) {
            e->via_ref = 1;
            t = t->base;
        }
        if (is_string_type(t))
            fatal_at(E0415, e->span,
                     "String fields are private; use view(), len(), or "
                     "free()");
        if (t->kind != TY_STRUCT || !t->name)
            fatal_at(E0407, e->span, "%s has no fields", type_spelling(t));
        Decl *d = find_struct_decl(se, t->name);
        if (!d)
            fatal_at(E0408, e->span, "unknown struct type '%s'", t->name);
        for (size_t i = 0; i < d->u.type.fields.len; i++)
            if (d->u.type.fields.items[i].name == e->u.field.name) {
                e->type = d->u.type.fields.items[i].ty;
                return e->type;
            }
        fatal_at(E0407, e->span, "struct '%s' has no field '%s'", t->name,
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
            fatal_at(E0408, e->span, "'%s' is not a struct type", e->u.stlit.name);
        if (is_string_type(st))
            fatal_at(E0605, e->span,
                     "String values must be built with String::new, "
                     "String::from, or String::fromowned");
        Decl *d = find_struct_decl(se, st->name);
        if (!d)
            fatal_at(E0408, e->span, "unknown struct type '%s'", st->name);
        for (size_t i = 0; i < e->u.stlit.fields.len; i++) {
            FieldInit *fi = &e->u.stlit.fields.items[i];
            Type *ft = NULL;
            for (size_t j = 0; j < d->u.type.fields.len; j++)
                if (d->u.type.fields.items[j].name == fi->name)
                    ft = d->u.type.fields.items[j].ty;
            if (!ft)
                fatal_at(E0407, fi->span, "struct '%s' has no field '%s'",
                         st->name, fi->name);
            Type *it = check_expr(se, fi->val, ft);
            reject_indexed_move(se, fi->val);
            if (ft->kind == TY_UNION) {
                fi->val = maybe_coerce(fi->val, ft);
                it = fi->val->type;
            }
            if (fi->val->kind == EX_NULL && ft->kind == TY_PTR && ft->nullable)
                continue; /* null widens to any nullable pointer */
            if (ptr_widens(it, ft))
                continue; /* *T widens to nullable *T */
            if (!is_unknown(it) && !type_eq(it, ft))
                fatal_at(E0601, fi->span, "field '%s' expects %s, got %s", fi->name,
                         type_spelling(ft), type_spelling(it));
        }
        /* resource literals stay explicit; plain structs rely on
         * C99 6.7.8/19 zero-filling unlisted members */
        if (d->u.type.is_resource) {
            for (size_t j = 0; j < d->u.type.fields.len; j++) {
                const char *fn = d->u.type.fields.items[j].name;
                int found = 0;
                for (size_t i = 0; i < e->u.stlit.fields.len; i++)
                    if (e->u.stlit.fields.items[i].name == fn)
                        found = 1;
                if (!found)
                    fatal_at(E0606, e->span,
                             "missing field '%s' in resource '%s' literal",
                             fn, st->name);
            }
        }
        e->type = st;
        return e->type;
    }
    case EX_MATCH: {
        Type *st = check_expr(se, e->u.match.scrutinee, NULL);
        reject_indexed_move(se, e->u.match.scrutinee);
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
                    fatal_at(E0610, a->span,
                             "'null' pattern requires a nullable pointer");
                else if (a->kind == PAT_TYPE_VAL) {
                    /* qualified error names (mod::NotFound) split first */
                    const char *vname = a->name;
                    const char *qsep = strstr(a->name, "::");
                    if (qsep)
                        vname = qsep + 2;
                    Sym *sym = scope_find(se->scope, vname);
                    if (sym && sym->kind == SYM_TYPE && sym->type)
                        idx = union_member_index(st, sym->type);
                    else
                        for (size_t k = 0; k < st->members.len; k++)
                            if (st->members.items[k]->kind == TY_ERR
                                && zstr_eq(st->members.items[k]->name,
                                           vname))
                                idx = (int)k;
                    if (idx < 0)
                        fatal_at(E0407, a->span, "'%s' is not a variant of %s",
                                 a->name, type_spelling(st));
                } else {
                    a->ty = resolve_type(se, a->ty);
                    idx = union_member_index(st, a->ty);
                    if (idx < 0)
                        fatal_at(E0407, a->span, "%s has no variant %s",
                                 type_spelling(st), type_spelling(a->ty));
                    if (st->members.items[idx]->is_error)
                        fatal_at(E0610, a->span,
                                 "cannot bind error variant %s; match it "
                                 "by name",
                                 type_spelling(st->members.items[idx]));
                }
                if (idx >= 0) {
                    if (cover & ((size_t)1 << idx))
                        fatal_at(E0609, a->span, "duplicate match arm for %s",
                                 type_spelling(st->members.items[idx]));
                    cover |= (size_t)1 << idx;
                }
            }
            if (!wild && cover != ((size_t)1 << st->members.len) - 1)
                for (size_t k = 0; k < st->members.len; k++)
                    if (!(cover & ((size_t)1 << k)))
                        fatal_at(E0608, e->span,
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
                        fatal_at(E0601, a->span, "match arm type %s differs from %s",
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
                        fatal_at(E0610, a->span, "bind pattern must be %s, got %s",
                                 type_spelling(nonnull),
                                 type_spelling(a->ty));
                    has_val = 1;
                } else {
                    fatal_at(E0610, a->span, "pointer match has no named variants");
                }
            }
            if (!wild && (!has_null || !has_val))
                fatal_at(E0608, e->span,
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
                        fatal_at(E0601, a->span, "match arm type %s differs from %s",
                                 type_spelling(at), type_spelling(mty));
                }
                pop_scope(se);
            }
            e->type = mty ? mty : type_prim(TY_VOID);
            return e->type;
        }
        if (st->kind == TY_ENUM) {
            Decl *ed = find_enum_decl(se, st->name);
            if (!ed)
                fatal_at(E0408, e->span, "unknown enum '%s'", st->name);
            /* exhaustiveness bitmask over variant indices */
            size_t cover = 0;
            int wild = 0;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                a->vidx = -1;
                if (a->kind == PAT_WILDCARD) {
                    wild = 1;
                    continue;
                }
                if (a->kind != PAT_TYPE_VAL || !a->name)
                    fatal_at(E0610, a->span,
                             "enum match arms must be Enum::Variant or '_'");
                const char *sep = strstr(a->name, "::");
                size_t enlen = strlen(st->name);
                if (!sep || (size_t)(sep - a->name) != enlen
                    || strncmp(a->name, st->name, enlen) != 0)
                    fatal_at(E0407, a->span, "'%s' is not a variant of %s",
                             a->name, st->name);
                int idx = -1;
                for (size_t k = 0; k < ed->u.type.variants.len; k++)
                    if (zstr_eq(ed->u.type.variants.items[k].name, sep + 2))
                        idx = (int)k;
                if (idx < 0)
                    fatal_at(E0407, a->span, "enum '%s' has no variant '%s'",
                             st->name, sep + 2);
                if (cover & ((size_t)1 << idx))
                    fatal_at(E0609, a->span, "duplicate match arm for %s::%s",
                             st->name, sep + 2);
                cover |= (size_t)1 << idx;
                a->vidx = idx;
            }
            if (!wild && cover != ((size_t)1 << ed->u.type.variants.len) - 1)
                for (size_t k = 0; k < ed->u.type.variants.len; k++)
                    if (!(cover & ((size_t)1 << k)))
                        fatal_at(E0608, e->span,
                                 "non-exhaustive match: missing variant "
                                 "%s::%s",
                                 st->name, ed->u.type.variants.items[k].name);
            e->type = type_prim(TY_VOID);
            Type *mty = NULL;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                push_scope(se);
                if (a->jump) {
                    check_stmt(se, a->jump);
                } else {
                    Type *at = check_expr(se, a->val, mty ? mty : hint);
                    if (!mty)
                        mty = at;
                    else if (!is_unknown(at) && !type_eq(mty, at))
                        fatal_at(E0601, a->span, "match arm type %s differs from %s",
                                 type_spelling(at), type_spelling(mty));
                }
                pop_scope(se);
            }
            e->type = mty ? mty : type_prim(TY_VOID);
            return e->type;
        }
        if (st->kind == TY_BOOL || ty_is_int(st->kind) || st->kind == TY_CHAR
            || st->kind == TY_STR) {
            int wild = 0;
            int has_t = 0, has_f = 0;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                if (a->kind == PAT_WILDCARD) {
                    wild = 1;
                    continue;
                }
                if (a->kind != PAT_LIT || !a->pat)
                    fatal_at(E0610, a->span,
                             "value match arms must be literals or '_'");
                Type *pt = check_expr(se, a->pat, st);
                if (!is_unknown(pt) && !type_eq(pt, st))
                    fatal_at(E0610, a->span, "pattern %s does not match %s",
                             type_spelling(pt), type_spelling(st));
                if (st->kind == TY_BOOL && a->pat->kind == EX_BOOL) {
                    if (a->pat->u.blit.v)
                        has_t = 1;
                    else
                        has_f = 1;
                }
            }
            if (st->kind == TY_BOOL) {
                if (!wild && (!has_t || !has_f))
                    fatal_at(E0608, e->span, "non-exhaustive bool match: cover "
                                      "'true' and 'false' (or use '_')");
            } else if (!wild) {
                /* integers, char, and str have open domains: demand '_' */
                fatal_at(E0608, e->span, "value match on %s requires a case _ => "
                                  "arm for exhaustiveness",
                         type_spelling(st));
            }
            e->type = type_prim(TY_VOID);
            Type *mty = NULL;
            for (size_t i = 0; i < e->u.match.arms.len; i++) {
                MatchArm *a = &e->u.match.arms.items[i];
                push_scope(se);
                if (a->jump) {
                    check_stmt(se, a->jump);
                } else {
                    Type *at = check_expr(se, a->val, mty ? mty : hint);
                    if (!mty)
                        mty = at;
                    else if (!is_unknown(at) && !type_eq(mty, at))
                        fatal_at(E0601, a->span, "match arm type %s differs from %s",
                                 type_spelling(at), type_spelling(mty));
                }
                pop_scope(se);
            }
            e->type = mty ? mty : type_prim(TY_VOID);
            return e->type;
        }
        fatal_at(E0605, e->span, "match requires a tagged union or nullable "
                          "pointer, got %s", type_spelling(st));
        return NULL;
    }
    case EX_TRY: {
        Type *t = check_expr(se, e->u.tryy, NULL);
        reject_indexed_move(se, e->u.tryy);
        Type *rt = se->fn_ret;
        if (t->kind == TY_UNION) {
            if (union_has_error(t) && (!rt || rt->kind != TY_UNION))
                fatal_at(E0605, e->span,
                         "'?' cannot propagate: enclosing function returns %s",
                         rt ? type_spelling(rt) : "void");
            for (size_t i = 0; i < t->members.len; i++) {
                Type *m = t->members.items[i];
                if (m->is_error && rt && union_member_index(rt, m) < 0)
                    fatal_at(E0605, e->span,
                             "'?' error variant %s is not in the enclosing "
                             "return type %s", type_spelling(m),
                             type_spelling(rt));
            }
            Type *payload = union_payload(t);
            if (!payload)
                fatal_at(E0605, e->span,
                         "'?' requires exactly one non-error variant");
            e->type = payload;
            return e->type;
        }
        if (t->kind == TY_PTR && t->nullable) {
            if (!rt || rt->kind != TY_PTR || !rt->nullable
                || !type_eq(rt->base, t->base))
                fatal_at(E0605, e->span,
                         "'?' on a nullable requires the enclosing function "
                         "to return %s", type_spelling(type_ptr(t->base, 1)));
            e->type = type_ptr(t->base, 0);
            return e->type;
        }
        fatal_at(E0605, e->span, "'?' requires a tagged union or nullable pointer");
        return NULL;
    }
    case EX_UNWRAP: {
        Type *t = check_expr(se, e->u.unwrap, NULL);
        reject_indexed_move(se, e->u.unwrap);
        if (t->kind == TY_UNION) {
            Type *payload = union_payload(t);
            if (!payload)
                fatal_at(E0605, e->span,
                         "'!' requires exactly one non-error variant");
            e->type = payload;
            return e->type;
        }
        if (t->kind == TY_PTR && t->nullable) {
            e->type = type_ptr(t->base, 0);
            return e->type;
        }
        fatal_at(E0605, e->span, "'!' requires a tagged union or nullable pointer");
        return NULL;
    }
    case EX_COERCE:
        if (e->u.coerce.e)
            check_expr(se, e->u.coerce.e, NULL);
        e->type = e->u.coerce.target;
        return e->type;
    }
    fatal_at(E0902, e->span, "invalid expression");
    return NULL;
}

static void check_let(Sema *se, Stmt *s)
{
    if (s->u.let.ty)
        s->u.let.ty = resolve_type(se, s->u.let.ty);
    if (s->u.let.init == NULL) {
        /* init-less lets: arrays and structs zero-initialize */
        if (!s->u.let.ty || (s->u.let.ty->kind != TY_ARRAY
                             && s->u.let.ty->kind != TY_STRUCT))
            fatal_at(E0708, s->span,
                     "let without an initializer requires an array or "
                     "struct type");
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = s->u.let.name;
        sym.kind = SYM_VAR;
        sym.span = s->span;
        sym.type = s->u.let.ty;
        sym.mut = s->u.let.mut;
        scope_add(se, sym);
        return;
    }
    Type *it = check_expr(se, s->u.let.init, s->u.let.ty);
    reject_indexed_move(se, s->u.let.init);
    if (s->u.let.ty && s->u.let.ty->kind == TY_UNION) {
        s->u.let.init = maybe_coerce(s->u.let.init, s->u.let.ty);
        it = s->u.let.init->type;
    }
    Type *declared = s->u.let.ty;
    if (!declared) {
        if (is_unknown(it))
            fatal_at(E0605, s->span,
                     "cannot infer type from extern call; "
                     "add an explicit type annotation");
        declared = it;
    } else {
        if (!is_unknown(it) && !type_eq(declared, it)
            && !(declared->kind == TY_REF && it->kind == TY_MUTREF
                 && type_eq(declared->base, it->base))
            && !ptr_widens(it, declared)
            && !null_fits(s->u.let.init, declared))
            fatal_at(E0601, s->span, "cannot bind %s to %s", type_spelling(it),
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

/* Conservative return-path analysis: a statement terminates when it always
 * leaves the function. Loops are not treated as terminating because they may
 * break or run zero times. */
static int block_terminates(Block *b);
static int block_terminates_stmt(Stmt *s);

static int all_match_arms_terminate(Expr *e)
{
    if (!e || e->kind != EX_MATCH || e->u.match.arms.len == 0)
        return 0;
    for (size_t i = 0; i < e->u.match.arms.len; i++) {
        MatchArm *a = &e->u.match.arms.items[i];
        if (!a->jump || !block_terminates_stmt(a->jump))
            return 0;
    }
    return 1;
}

static int block_terminates_stmt(Stmt *s)
{
    if (!s)
        return 0;
    switch (s->kind) {
    case ST_RETURN:
        return 1;
    case ST_BLOCK:
        return block_terminates(s->u.block);
    case ST_IF:
        if (!s->u.iff.else_stmt)
            return 0;
        return block_terminates(s->u.iff.then_blk)
            && block_terminates_stmt(s->u.iff.else_stmt);
    case ST_EXPR:
        return all_match_arms_terminate(s->u.expr);
    default:
        return 0;
    }
}

static int block_terminates(Block *b)
{
    if (!b || b->stmts.len == 0)
        return 0;
    /* reachable last statement decides; earlier dead code is irrelevant */
    return block_terminates_stmt(b->stmts.items[b->stmts.len - 1]);
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
        Type *et = check_expr(se, s->u.forr.end, NULL);
        Type *st = check_expr(se, s->u.forr.start, et);
        if (!is_unknown(st) && !ty_is_int(st->kind))
            fatal_at(E0604, s->u.forr.start->span,
                     "range start must be an integer, got %s",
                     type_spelling(st));
        if (is_unknown(st) || is_unknown(et))
            fatal_at(E0605, s->span, "range bounds must have concrete types");
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
    case ST_FOR_EACH: {
        Type *it = check_expr(se, s->u.foreach.iter, NULL);
        Type *elem = NULL;
        if (it->kind == TY_SLICE || it->kind == TY_ARRAY)
            elem = it->base;
        if (is_stringish(it))
            elem = type_prim(TY_U8);
        if (!elem)
            fatal_at(E0605, s->span, "cannot iterate %s", type_spelling(it));
        if (type_owns_resource(se, elem, 0))
            fatal_at(E0518, s->span,
                     "cannot iterate owned elements by value; iterate "
                     "indices and borrow each element instead");
        s->u.foreach.ty = elem;
        push_scope(se);
        Sym iv;
        memset(&iv, 0, sizeof(iv));
        iv.name = s->u.foreach.name;
        iv.kind = SYM_VAR;
        iv.span = s->span;
        iv.type = elem;
        iv.mut = 0;
        scope_add(se, iv);
        se->loop_depth++;
        check_block(se, s->u.foreach.body);
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
            reject_indexed_move(se, s->u.ret);
            if (ret->kind == TY_VOID)
                fatal_at(E0706, s->span, "void function cannot return a value");
            if (ret->kind == TY_UNION)
                s->u.ret = maybe_coerce(s->u.ret, ret);
            t = s->u.ret->type;
            if (!is_unknown(t) && !type_eq(ret, t)
                && !null_fits(s->u.ret, ret))
                fatal_at(E0601, s->span, "returning %s from a %s function",
                         type_spelling(t), type_spelling(ret));
        } else if (ret->kind != TY_VOID) {
            /* bare return may build a void-payload union member */
            if (ret->kind == TY_UNION) {
                int idx = -1;
                for (size_t i = 0; i < ret->members.len; i++)
                    if (!ret->members.items[i]->is_error
                        && ret->members.items[i]->kind == TY_VOID)
                        idx = (int)i;
                if (idx >= 0) {
                    Expr *c = expr_new(EX_COERCE, s->span);
                    c->u.coerce.e = NULL;
                    c->u.coerce.target = ret;
                    c->coerce_tag = idx;
                    s->u.ret = c;
                }
            }
            if (!s->u.ret)
                fatal_at(E0702, s->span, "missing return value");
        }
        break;
    }
    case ST_BREAK:
    case ST_CONTINUE:
        if (se->loop_depth == 0)
            fatal_at(E0705, s->span, "%s outside of a loop",
                     s->kind == ST_BREAK ? "break" : "continue");
        break;
    case ST_EXPR: {
        Type *t = check_expr(se, s->u.expr, NULL);
        // #[must_use]: dropping an error-carrying union is a hard error.
        // Assignment keeps the value alive in its destination, so it is not
        // a drop even when the destination is an error-carrying union.
        if (t && s->u.expr->kind != EX_ASSIGN
            && (t->kind == TY_ERR
                || (t->kind == TY_UNION && union_has_error(t))))
            fatal_at(E0704, s->span,
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
    if (d->registered)
        return; /* pass-1 iterates a list instantiations append to */
    d->registered = 1;
    /* instantiations register while a body check holds a local scope;
     * all decls must land in the global scope */
    Scope *saved = se->scope;
    while (se->scope->parent)
        se->scope = se->scope->parent;
    switch (d->kind) {
    case DL_TEST:
        break; /* test bodies carry no global symbols */
    case DL_USE: {
        if (d->u.use.zsyms.len > 0) {
            /* loaded .zov module: members resolve to global symbols */
            Sym *prior = scope_find(se->scope, d->u.use.alias);
            if (prior && prior->kind == SYM_NS)
                break; /* diamond import: namespace already registered */
            Sym sym;
            memset(&sym, 0, sizeof(sym));
            sym.name = d->u.use.alias;
            sym.kind = SYM_NS;
            sym.span = d->span;
            sym.is_zov = 1;
            sym.zsyms = d->u.use.zsyms;
            sym.ztargets = d->u.use.ztargets;
            scope_add(se, sym);
            /* the full path (std::io) resolves to the same members, so
             * std::io::println works without the alias */
            const char *full = str_intern_c(d->u.use.path);
            if (full != sym.name && !scope_find(se->scope, full)) {
                Sym full_sym = sym;
                full_sym.name = full;
                scope_add(se, full_sym);
            }
            break;
        }
        if (!d->u.use.is_c_header)
            return; /* std module paths carry no symbols */
        Sym *prior = scope_find(se->scope, d->u.use.alias);
        CHeader *h = cimport_header(d->u.use.path, 1);
        if (!h)
            h = cimport_header(d->u.use.path, 0);
        if (!h)
            fatal_at(E0303, d->span, "cannot import C header '%s'", d->u.use.path);
        if (prior && prior->kind == SYM_NS && prior->hdr) {
            /* same alias over different headers: merge member tables */
            for (size_t i = 0; i < h->members.len; i++) {
                CMember *m = &h->members.items[i];
                if (!cimport_find(prior->hdr, m->name)) {
                    CMember copy = *m;
                    da_push(&prior->hdr->members, copy);
                }
            }
            break;
        }
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.use.alias;
        sym.kind = SYM_NS;
        sym.span = d->span;
        sym.hdr = h;
        /* a C alias declared inside a module is private to that module:
         * keep it out of the global scope so importers cannot see it */
        sym.modname = d->modname;
        if (d->modname) {
            int dup = 0;
            for (size_t i = 0; i < se->modc.len; i++)
                if (se->modc.items[i].modname == d->modname
                    && se->modc.items[i].name == sym.name)
                    dup = 1;
            if (!dup)
                da_push(&se->modc, sym);
            break;
        }
        scope_add(se, sym);
        break;
    }
    case DL_TYPE: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        sym.name = d->u.type.name;
        sym.kind = SYM_TYPE;
        sym.span = d->span;
        sym.modname = d->modname;
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
        } else if (d->u.type.is_enum) {
            /* enums are Copy scalars keyed by their declaration name */
            Type *e = zalloc(sizeof(Type));
            e->kind = TY_ENUM;
            e->name = d->u.type.name;
            sym.type = e;
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
                    fatal_at(E0605, d->u.type.fields.items[i].span,
                             "borrowed reference cannot be stored in struct "
                             "field; use an owned value or *T");
            }
            sym.type = type_struct(d->u.type.name);
            if (d->u.type.is_resource)
                sym.type->is_move = 1;
        }
        if (d->modname && !d->exported) {
            da_push(&se->modtypes, sym);
            break;
        }
        scope_add(se, sym);
        break;
    }
    case DL_FN: {
        Sym sym;
        memset(&sym, 0, sizeof(sym));
        /* module fns register mangled; methods keep their plain names */
        if (d->modname && d->regname && !d->u.fn.recv)
            sym.name = d->regname;
        else
            sym.name = d->u.fn.name;
        sym.kind = SYM_FN;
        sym.span = d->span;
        sym.decl = d;
        if (!d->regname)
            d->regname = sym.name;
        int is_method = d->u.fn.recv != NULL;
        if (is_method && !d->u.fn.sname)
            fatal_at(E0414, d->span, "method registration requires a struct name");
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
        if (is_method && reg) {
            Meth m;
            m.sname = d->u.fn.sname;
            m.mname = d->u.fn.mname;
            m.sym = *reg; /* copy: scope storage reallocs */
            da_push(&se->methods, m);
        }
        if (zstr_eq(d->u.fn.name, "main")) {
            se->has_main = 1;
            if (d->u.fn.params.len != 0)
                fatal_at(E0411, d->span, "'main' must take no parameters");
        }
        break;
    }
    }
    se->scope = saved;
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
    if (d->u.fn.ret->kind != TY_VOID
        && !block_terminates(d->u.fn.body))
        fatal_at(E0703, d->span,
                 "function '%s' does not return a value on all paths",
                 d->u.fn.name);
    se->fn_ret = prev_ret;
    se->loop_depth = prev_loop;
    pop_scope(se);
}

static void check_test(Sema *se, Decl *d)
{
    push_scope(se);
    Type *prev_ret = se->fn_ret;
    int prev_loop = se->loop_depth;
    se->loop_depth = 0;
    se->fn_ret = type_prim(TY_VOID);
    check_block(se, d->u.test.body);
    se->fn_ret = prev_ret;
    se->loop_depth = prev_loop;
    pop_scope(se);
}

/* plain-name call sites inside a module resolve to its mangled fns */
static void push_module_scope(Sema *se, const char *modname)
{
    if (!modname) {
        push_scope(se);
        return;
    }
    Scope *saved = se->scope;
    while (se->scope->parent)
        se->scope = se->scope->parent;
    push_scope(se);
    for (size_t i = 0; i < se->prog->decls.len; i++) {
        Decl *d = se->prog->decls.items[i];
        if (d->kind != DL_FN || !d->modname || d->modname != modname
            || d->u.fn.recv)
            continue;
        Sym *real = scope_find(se->scope, d->regname);
        if (!real)
            continue;
        Sym alias = *real;
        alias.name = d->u.fn.name;
        alias.alias_to = d->regname;
        scope_add(se, alias);
    }
    for (size_t i = 0; i < se->modtypes.len; i++) {
        Sym *t = &se->modtypes.items[i];
        if (t->modname != modname)
            continue;
        scope_add(se, *t);
    }
    se->scope->parent = saved;
}

void sema_check(Program *prog, int test_mode)
{
    Sema *se = zalloc(sizeof(Sema));
    se->prog = prog;
    push_scope(se); /* global scope */
    install_builtin_string(prog);
    for (size_t i = 0; i < prog->decls.len; i++) {
        se->curmod = prog->decls.items[i]->modname;
        register_decl(se, prog->decls.items[i]);
    }
    se->curmod = NULL;
    check_all_type_cycles(se);
    /* freestanding kernels may use kmain or no entry at all */
    if (!se->has_main && !test_mode && !prog->freestanding)
        diag_fatal_global(E0411, "no 'fn main' found in program");
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
    const char *curmod = (const char *)1;
    for (size_t i = 0; i < prog->decls.len; i++) {
        Decl *d = prog->decls.items[i];
        if (prog->freestanding && d->kind == DL_FN
            && (hosted_only_module(d->modname)
                || decl_is_hosted_only(d)))
            continue;
        if ((d->kind == DL_FN && d->u.fn.tparams.len == 0)
            || d->kind == DL_TEST) {
            if (d->modname != curmod) {
                if (curmod != (const char *)1)
                    pop_scope(se);
                curmod = d->modname;
                push_module_scope(se, curmod);
            }
            se->curmod = d->modname;
            if (d->kind == DL_FN)
                check_fn(se, d);
            else
                check_test(se, d);
        }
    }
    if (curmod != (const char *)1)
        pop_scope(se);
}
