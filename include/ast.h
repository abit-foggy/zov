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
} TypeKind;

typedef struct Type {
    TypeKind kind;
    const char *name; /* struct spelling, interned */
    struct Type *base;
    size_t count;     /* fixed array length */
    int nullable;     /* pointer nullability */
} Type;

Type *type_prim(TypeKind k);
Type *type_ptr(Type *base, int nullable);
Type *type_ref(Type *base, int is_mut);
Type *type_slice(Type *base);
Type *type_array(Type *base, size_t n);
Type *type_struct(const char *name);
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
} ExprKind;

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
        struct { struct Expr *base; Type *arg; } generic;
    } u;
} Expr;

Expr *expr_new(ExprKind kind, Span span);

typedef struct Param {
    const char *name;
    Type *ty;
    Span span;
} Param;

typedef struct Field {
    const char *name;
    Type *ty;
    Span span;
} Field;

typedef enum StmtKind {
    ST_LET, ST_DEFER, ST_IF, ST_WHILE, ST_RETURN, ST_BREAK, ST_CONTINUE,
    ST_EXPR, ST_BLOCK,
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
        Expr *ret;
        Expr *expr;
        Block *block;
    } u;
} Stmt;

Stmt *stmt_new(StmtKind kind, Span span);
Block *block_new(void);

typedef enum DeclKind { DL_USE, DL_TYPE, DL_FN } DeclKind;

typedef struct Decl {
    DeclKind kind;
    Span span;
    int exported;
    union {
        struct { int is_c_header; const char *path; const char *alias; } use;
        struct { const char *name; DA(struct Field) fields; } type;
        struct { const char *name; DA(struct Param) params; Type *ret;
                  Block *body; } fn;
    } u;
} Decl;

Decl *decl_new(DeclKind kind, Span span);

typedef struct Program {
    DA(struct Decl *) decls;
} Program;

#endif /* ZOV_AST_H */
