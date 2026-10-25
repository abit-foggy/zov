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

#ifndef ZOV_AST_H
#define ZOV_AST_H

#include "common.h"
#include "token.h"

void *zalloc(size_t size); /* bump allocation from the global AST arena */

typedef enum TypeKind {
    TY_NONE = 0,
    TY_UNKNOWN, /* result of untyped extern C calls; passthrough only */
    TY_VOID,
    TY_I8, TY_I16, TY_I32, TY_I64, TY_ISIZE,
    TY_U8, TY_U16, TY_U32, TY_U64, TY_USIZE,
    TY_F32, TY_F64,
    TY_BOOL, TY_CHAR, TY_STR,
    TY_PTR,      /* nullable flag distinguishes nullable *T from *T */
    TY_REF,      /* shared borrow: &T */
    TY_MUTREF,   /* mutable borrow: &mut T */
    TY_SLICE,
    TY_ARRAY,
    TY_STRUCT,
    TY_UNION,   /* tagged union: deterministic tag = member index */
    TY_ERR,     /* !T error type; name gives variant identity */
    TY_INST,    /* generic instantiation Type[T,U]; members = type args */
    TY_ENUM,    /* named scalar enum: variants live on the decl */
} TypeKind;

typedef DA(struct Type *) TypeList;
typedef DA(const char *) StrList;
typedef DA(struct Expr *) ExprList;

typedef struct Type {
    TypeKind kind;
    const char *name; /* struct spelling or error variant name, interned */
    struct Type *base;
    size_t count;     /* fixed array length */
    int nullable;     /* pointer nullability */
    int is_error;     /* TY_ERR marker; error-wrapping unions set this too */
    int is_move;      /* linear types: resource structs and &mut T */
    const char *c_name; /* verbatim C spelling for imported types */
    const char *tmpl;    /* originating generic template name, if any */
    TypeList inst_args; /* concrete type arguments for generic instances */
    TypeList members; /* TY_UNION payload members, tag == index;
                         TY_INST stores concrete type arguments */
} Type;

Type *type_prim(TypeKind k);
Type *type_ptr(Type *base, int nullable);
Type *type_ref(Type *base, int is_mut);
Type *type_slice(Type *base);
Type *type_array(Type *base, size_t n);

Type *type_struct(const char *name);
Type *type_union(TypeList *members);
Type *type_err(const char *name, Type *payload);
Type *type_null(void);
Type *type_default_int(void);   /* i32 */
Type *type_default_float(void); /* f64 */
int type_eq(const Type *a, const Type *b);
const char *type_spelling(Type *t);
Type *type_from_suffix(const char *suffix); /* NULL when not a type suffix */

int ty_is_int(TypeKind k);
int ty_is_float(TypeKind k);
int ty_is_num(TypeKind k);
int ty_is_bool_like(TypeKind k);

typedef enum ExprKind {
    EX_INT, EX_FLOAT, EX_STR, EX_CHAR, EX_BOOL, EX_NULL,
    EX_IDENT, EX_NS_IDENT,
    EX_ASSIGN, EX_BINARY, EX_UNARY,
    EX_CALL, EX_INDEX, EX_CAST, EX_GENERIC,
    EX_MATCH, EX_TRY, EX_UNWRAP, EX_COERCE,
    EX_DEREF, EX_FIELD, EX_STRUCT_LIT, EX_SLICE, EX_ARR_LIT,
    EX_ORELSE,
} ExprKind;

typedef enum PatKind {
    PAT_NULL,      /* case null => */
    PAT_WILDCARD,  /* case _ => */
    PAT_TYPE_BIND, /* case let x: T => */
    PAT_TYPE_VAL, /* case TypeName or EnumName::Variant => */
    PAT_LIT,       /* case 42 | 'a' | "s" | true => */
} PatKind;

typedef struct FieldInit {
    const char *name;
    Span span;
    struct Expr *val;
} FieldInit;

typedef struct MatchArm {
    PatKind kind;
    Span span;
    const char *name; /* binder or referenced type name */
    Type *ty;         /* PAT_TYPE_BIND target type */
    int vidx;         /* enum match: resolved variant index */
    const char *variant_c; /* enum arm: C variant constant */
    struct Expr *pat;  /* PAT_LIT pattern expression */
    struct Expr *val;  /* expression body; NULL when jump is set */
    struct Stmt *jump; /* return/break/continue arm; NULL when val is set */
} MatchArm;

typedef struct Expr {
    ExprKind kind;
    Span span;
    Type *type;
    union {
        struct { uint64_t v; const char *suffix; } ilit;
        struct { double v; const char *suffix; } flit;
        struct { const char *v; size_t len; } slit;
        struct { uint32_t v; } clit;
        struct { int v; } blit;
        const char *ident;
        struct { const char *ns, *name; } nident;
        struct { struct Expr *lhs, *rhs; } assign;
        struct { struct Expr *lhs, *rhs; TokenKind op; } binary;
        struct { struct Expr *e; TokenKind op; } unary;
        struct { struct Expr *callee; DA(struct Expr *) args; } call;
        struct { struct Expr *base, *index; } index;
        struct { struct Expr *e; Type *t; } cast;
        struct { struct Expr *base; DA(Type *) targs; } generic;
        struct { struct Expr *scrutinee; DA(MatchArm) arms; } match;
        struct Expr *tryy;    /* EX_TRY operand */
        struct Expr *unwrap;  /* EX_UNWRAP operand */
        struct { struct Expr *e; Type *target; } coerce; /* EX_COERCE */
        struct Expr *deref;   /* EX_DEREF operand */
        struct { struct Expr *base; const char *name; } field;
        struct { const char *name; DA(FieldInit) fields;
                 DA(Type *) targs; } stlit;
        struct { struct Expr *base, *start, *end; } slice;
        DA(struct Expr *) elems; /* EX_ARR_LIT */
        struct { struct Expr *lhs, *val; struct Stmt *jump; } orelse;
    } u;
    int borrow_mut; /* &mut in EX_UNARY AMP */
    int via_ref;    /* EX_FIELD reached through a reference: emit '->' */
    int builtin_io; /* EX_CALL to built-in print/println */
    Type *szarg;    /* EX_CALL to sizeof[T](): the type argument */
    int coerce_tag; /* EX_COERCE: deterministic member index in target */
    int builtin_assert; /* EX_CALL to built-in assert(bool) */
    const char *variant_c; /* EX_NS_IDENT enum variant: C spelling */
    TokenKind comp_op;     /* EX_ASSIGN: compound operator, 0 when plain = */
    Type *hash_ty;         /* EX_CALL to hash_val[T](): the key type */
    int builtin_env_kind;  /* 1 args_count, 2 arg_at, 3 env_has, 4 env_get */
} Expr;

Expr *expr_new(ExprKind kind, Span span);

typedef struct Param {
    const char *name;
    Type *ty;
    Span span;
} Param;

typedef struct EnumVariant {
    const char *name;
    long long value;
    Span span;
} EnumVariant;

typedef struct Field {
    const char *name;
    Type *ty;
    Span span;
} Field;

typedef enum StmtKind {
    ST_LET, ST_DEFER, ST_IF, ST_WHILE, ST_RETURN, ST_BREAK, ST_CONTINUE,
    ST_EXPR, ST_BLOCK, ST_FOR_RANGE, ST_FOR_EACH,
} StmtKind;

typedef struct Block {
    DA(struct Stmt *) stmts;
} Block;

typedef struct Stmt {
    StmtKind kind;
    Span span;
    union {
        struct { const char *name; int mut; Type *ty; Expr *init; } let;
        struct Stmt *defer;
        struct { Expr *cond; Block *then_blk; struct Stmt *else_stmt; } iff;
        struct { Expr *cond; Block *body; } whilee;
        struct { const char *name; Type *ty; Expr *start, *end;
                 Block *body; } forr;
        struct { const char *name; Type *ty; Expr *iter;
                 Block *body; } foreach;
        Expr *ret;
        Expr *expr;
        Block *block;
    } u;
} Stmt;

Stmt *stmt_new(StmtKind kind, Span span);
Block *block_new(void);

typedef enum DeclKind { DL_USE, DL_TYPE, DL_FN, DL_TEST } DeclKind;

typedef struct Decl {
    DeclKind kind;
    Span span;
    int exported;
    const char *modname; /* originating module: enables fn mangling */
    const char *regname; /* name registered in sema (may be mangled) */
    int registered;      /* idempotence: instantiations append mid-loop */
    union {
        struct { int is_c_header; const char *path; const char *alias;
                 StrList zsyms;    /* exported plain symbol names */
                 StrList ztargets; /* registered (mangled) fn targets */
                 } use;
        struct { const char *name; DA(struct Field) fields;
                  Type *alias_ty; int is_resource; int is_enum;
                  DA(struct EnumVariant) variants;
                  StrList tparams; } type;
        struct { const char *name; DA(struct Param) params; Type *ret;
                  Block *body; StrList tparams; struct Param *recv;
                  const char *sname; const char *mname; } fn;
        struct { const char *desc; Block *body; } test;
    } u;
} Decl;

Decl *decl_new(DeclKind kind, Span span);

typedef struct Program {
    DA(struct Decl *) decls;
} Program;

#endif /* ZOV_AST_H */
