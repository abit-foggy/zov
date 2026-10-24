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

#include "parser.h"

typedef struct Parser {
    Lexer lx;
} Parser;

static Token *tok(Parser *p)
{
    return &p->lx.tok;
}

static TokenKind kind(Parser *p)
{
    return p->lx.tok.kind;
}

static void advance(Parser *p)
{
    lex_next(&p->lx);
}

static void expect(Parser *p, TokenKind k, const char *what)
{
    if (kind(p) != k)
        fatal_at(tok(p)->span, "expected %s", what);
    advance(p);
}

static const char *tok_desc(Parser *p)
{
    static char buf[64];
    switch (kind(p)) {
    case TK_EOF: return "end of file";
    case TK_IDENT: snprintf(buf, sizeof(buf), "identifier '%s'", tok(p)->name);
        return buf;
    default: snprintf(buf, sizeof(buf), "'%.*s'",
                      (int)(tok(p)->end - tok(p)->start), tok(p)->start);
        return buf;
    }
}

static void eat_semi(Parser *p)
{
    if (kind(p) == TK_SEMI)
        advance(p);
}

static int at(Parser *p, TokenKind k)
{
    return kind(p) == k;
}

/* ---- types ---- */

static Type *parse_type(Parser *p);

static int at_type_start(Parser *p)
{
    switch (kind(p)) {
    case TK_VOID: case TK_I8: case TK_I16: case TK_I32: case TK_I64:
    case TK_ISIZE: case TK_U8: case TK_U16: case TK_U32: case TK_U64:
    case TK_USIZE: case TK_F32: case TK_F64: case TK_BOOL:
    case TK_CHAR_KW: case TK_STR_KW: case TK_NULLABLE:
    case TK_STAR: case TK_AMP: case TK_LBRACKET: case TK_IDENT:
    case TK_BANG: case TK_LPAREN:
        return 1;
    default:
        return 0;
    }
}

static Type *parse_primitive(Parser *p)
{
    static const struct { TokenKind tk; TypeKind ty; } tab[] = {
        {TK_VOID, TY_VOID}, {TK_I8, TY_I8}, {TK_I16, TY_I16},
        {TK_I32, TY_I32}, {TK_I64, TY_I64}, {TK_ISIZE, TY_ISIZE},
        {TK_U8, TY_U8}, {TK_U16, TY_U16}, {TK_U32, TY_U32},
        {TK_U64, TY_U64}, {TK_USIZE, TY_USIZE},
        {TK_F32, TY_F32}, {TK_F64, TY_F64},
        {TK_BOOL, TY_BOOL}, {TK_CHAR_KW, TY_CHAR}, {TK_STR_KW, TY_STR},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i].tk == kind(p)) {
            advance(p);
            return type_prim(tab[i].ty);
        }
    fatal_at(tok(p)->span, "expected type");
    return NULL;
}

static Type *parse_type(Parser *p)
{
    switch (kind(p)) {
    case TK_BANG: {
        // '!T' error type; named at the type declaration site by sema
        advance(p);
        return type_err(NULL, parse_type(p));
    }
    case TK_LPAREN: {
        advance(p);
        Type *first = parse_type(p);
        if (!at(p, TK_PIPE)) {
            expect(p, TK_RPAREN, "')'");
            return first;
        }
        TypeList members = {0};        da_push(&members, first);
        while (at(p, TK_PIPE)) {
            advance(p);
            da_push(&members, parse_type(p));
        }
        expect(p, TK_RPAREN, "')'");
        return type_union(&members);
    }
    case TK_NULLABLE:
        advance(p);
        expect(p, TK_STAR, "'*' after 'nullable'");
        return type_ptr(parse_type(p), 1);
    case TK_STAR:
        advance(p);
        return type_ptr(parse_type(p), 0);
    case TK_AMP:
        advance(p);
        if (at(p, TK_MUT)) {
            advance(p);
            return type_ref(parse_type(p), 1);
        }
        return type_ref(parse_type(p), 0);
    case TK_LBRACKET: {
        /* LL(1) split: '[]' slice vs '[N]' fixed array */
        advance(p);
        if (at(p, TK_RBRACKET)) {
            advance(p);
            return type_slice(parse_type(p));
        }
        if (!at(p, TK_INT))
            fatal_at(tok(p)->span, "expected array length");
        size_t n = (size_t)tok(p)->int_val;
        advance(p);
        expect(p, TK_RBRACKET, "']'");
        return type_array(parse_type(p), n);
    }
    default:
        if (at(p, TK_IDENT)) {
            const char *name = tok(p)->name;
            Span sp = tok(p)->span;
            advance(p);
            if (at(p, TK_COLONCOLON)) {
                // qualified C type: ns::Name resolves in sema via cimport
                advance(p);
                if (!at(p, TK_IDENT))
                    fatal_at(tok(p)->span, "expected type name after '::'");
                char joined[256];
                snprintf(joined, sizeof(joined), "%s::%s", name,
                         tok(p)->name);
                advance(p);
                return type_struct(str_intern_c(joined));
            }
            if (at(p, TK_LBRACKET)) {
                /* Name[T, U]: instantiation; resolved by sema */
                Type *inst = zalloc(sizeof(Type));
                inst->kind = TY_INST;
                inst->name = name;
                advance(p);
                for (;;) {
                    da_push(&inst->members, parse_type(p));
                    if (at(p, TK_COMMA)) {
                        advance(p);
                        continue;
                    }
                    break;
                }
                expect(p, TK_RBRACKET, "']'");
                return inst;
            }
            (void)sp;
            return type_struct(name);
        }
        return parse_primitive(p);
    }
}

/* ---- expressions ---- */

static Expr *parse_expr(Parser *p);

// "a{x}b" desugars to "a" + (x) + "b"; '\{' stays literal via marker 0x01
static Expr *build_string_expr(Parser *p, const char *val, size_t len)
{
    Expr *chain = NULL;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && val[i] != '{')
            i++;
        if (i > start) {
            Expr *seg = expr_new(EX_STR, p->lx.tok.span);
            seg->u.slit.v = val + start;
            seg->u.slit.len = i - start;
            if (chain) {
                Expr *add = expr_new(EX_BINARY, chain->span);
                add->u.binary.op = TK_PLUS;
                add->u.binary.lhs = chain;
                add->u.binary.rhs = seg;
                chain = add;
            } else {
                chain = seg;
            }
        }
        if (i >= len)
            break;
        /* interpolation hole: reparse the inner source as an expression */
        size_t inner_start = ++i;
        while (i < len && val[i] != '}')
            i++;
        if (i >= len)
            fatal_at(p->lx.tok.span, "unterminated '{' interpolation");
        char *src = zalloc(i - inner_start + 1);
        memcpy(src, val + inner_start, i - inner_start);
        src[i - inner_start] = '\0';
        Parser *sub = zalloc(sizeof(Parser));
        lex_init(&sub->lx, src, p->lx.tok.span.file);
        lex_next(&sub->lx);
        Expr *hole = parse_expr(sub);
        if (sub->lx.tok.kind != TK_EOF)
            fatal_at(sub->lx.tok.span, "unexpected token in interpolation");
        if (!chain) {
            chain = hole;
            i++;
            continue;
        }
        Expr *add = expr_new(EX_BINARY, chain->span);
        add->u.binary.op = TK_PLUS;
        add->u.binary.lhs = chain;
        add->u.binary.rhs = hole;
        chain = add;
        i++;
    }
    if (!chain) {
        Expr *seg = expr_new(EX_STR, p->lx.tok.span);
        seg->u.slit.v = val;
        seg->u.slit.len = 0;
        return seg;
    }
    return chain;
}
static Block *parse_block(Parser *p);
static Stmt *parse_stmt(Parser *p);
static Stmt *parse_arm_jump(Parser *p, Span sp)
{
    if (at(p, TK_RETURN)) {
        advance(p);
        Stmt *s = stmt_new(ST_RETURN, sp);
        if (!at(p, TK_COMMA) && !at(p, TK_RBRACE))
            s->u.ret = parse_expr(p);
        return s;
    }
    if (at(p, TK_BREAK)) {
        advance(p);
        return stmt_new(ST_BREAK, sp);
    }
    advance(p);
    return stmt_new(ST_CONTINUE, sp);
}

static Expr *parse_match(Parser *p)
{
    Span sp = tok(p)->span;
    advance(p); /* 'match' */
    expect(p, TK_LPAREN, "'(' after 'match'");
    Expr *e = expr_new(EX_MATCH, sp);
    e->u.match.scrutinee = parse_expr(p);
    expect(p, TK_RPAREN, "')' after match scrutinee");
    expect(p, TK_LBRACE, "'{' in match");
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        expect(p, TK_CASE, "'case'");
        MatchArm arm;
        memset(&arm, 0, sizeof(arm));
        arm.span = tok(p)->span;
        if (at(p, TK_NULL)) {
            arm.kind = PAT_NULL;
            advance(p);
        } else if (at(p, TK_IDENT) && zstr_eq(tok(p)->name, "_")) {
            arm.kind = PAT_WILDCARD;
            advance(p);
        } else if (at(p, TK_LET)) {
            arm.kind = PAT_TYPE_BIND;
            advance(p);
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected binder name in case let");
            arm.name = tok(p)->name;
            advance(p);
            expect(p, TK_COLON, "':' in case let pattern");
            arm.ty = parse_type(p);
        } else if (at(p, TK_IDENT)) {
            arm.kind = PAT_TYPE_VAL;
            arm.name = tok(p)->name;
            advance(p);
        } else {
            fatal_at(tok(p)->span, "expected match pattern");
        }
        expect(p, TK_FATARROW, "'=>' in match arm");
        if (at(p, TK_RETURN) || at(p, TK_BREAK) || at(p, TK_CONTINUE)) {
            arm.jump = parse_arm_jump(p, tok(p)->span);
        } else if (at(p, TK_LBRACE)) {
            // block-bodied arm: legal in statement matches only
            arm.jump = stmt_new(ST_BLOCK, tok(p)->span);
            arm.jump->u.block = parse_block(p);
        } else {
            arm.val = parse_expr(p);
        }
        da_push(&e->u.match.arms, arm);
        if (at(p, TK_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TK_RBRACE, "'}' after match arms");
    return e;
}

static Expr *parse_primary(Parser *p)
{
    Span sp = tok(p)->span;
    switch (kind(p)) {
    case TK_MATCH:
        return parse_match(p);
    case TK_INT: {
        Expr *e = expr_new(EX_INT, sp);
        e->u.ilit.v = tok(p)->int_val;
        e->u.ilit.suffix = tok(p)->suffix;
        advance(p);
        return e;
    }
    case TK_FLOAT: {
        Expr *e = expr_new(EX_FLOAT, sp);
        e->u.flit.v = tok(p)->float_val;
        e->u.flit.suffix = tok(p)->suffix;
        advance(p);
        return e;
    }
    case TK_STR: {
        Expr *e = build_string_expr(p, tok(p)->str_val, tok(p)->str_len);
        advance(p);
        return e;
    }
    case TK_CHAR: {
        Expr *e = expr_new(EX_CHAR, sp);
        e->u.clit.v = tok(p)->char_val;
        advance(p);
        return e;
    }
    case TK_TRUE:
    case TK_FALSE: {
        Expr *e = expr_new(EX_BOOL, sp);
        e->u.blit.v = (kind(p) == TK_TRUE);
        advance(p);
        return e;
    }
    case TK_NULL: {
        advance(p);
        return expr_new(EX_NULL, sp);
    }
    case TK_IDENT: {
        Expr *e = expr_new(EX_IDENT, sp);
        e->u.ident = tok(p)->name;
        advance(p);
        return e;
    }
    case TK_LPAREN: {
        advance(p);
        Expr *e = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return e;
    }
    default:
        fatal_at(sp, "expected expression, found %s", tok_desc(p));
        return NULL;
    }
}

static Expr *parse_call_args(Parser *p, Expr *callee)
{
    Expr *e = expr_new(EX_CALL, callee->span);
    e->u.call.callee = callee;
    advance(p); /* '(' */
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            da_push(&e->u.call.args, parse_expr(p));
            if (at(p, TK_COMMA)) {
                advance(p);
                if (at(p, TK_RPAREN))
                    break;
                continue;
            }
            break;
        }
    }
    expect(p, TK_RPAREN, "')' or ','");
    return e;
}

/* '[' after an expression: type-keyword start means generic args;
 * anything else is indexing. */
static Expr *parse_postfix_bracket(Parser *p, Expr *base)
{
    Span sp = tok(p)->span;
    advance(p); /* '[' */
    switch (kind(p)) {
    case TK_VOID: case TK_I8: case TK_I16: case TK_I32: case TK_I64:
    case TK_ISIZE: case TK_U8: case TK_U16: case TK_U32: case TK_U64:
    case TK_USIZE: case TK_F32: case TK_F64: case TK_BOOL:
    case TK_CHAR_KW: case TK_STR_KW: case TK_NULLABLE:
    case TK_STAR: case TK_AMP: case TK_LBRACKET: {
        Expr *e = expr_new(EX_GENERIC, sp);
        e->u.generic.base = base;
        for (;;) {
            da_push(&e->u.generic.targs, parse_type(p));
            if (at(p, TK_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        expect(p, TK_RBRACKET, "']'");
        return e;
    }
    default: {
        Expr *idx = parse_expr(p);
        expect(p, TK_RBRACKET, "']'");
        Expr *e = expr_new(EX_INDEX, sp);
        e->u.index.base = base;
        e->u.index.index = idx;
        return e;
    }
    }
}

/* Name{ f = e, ... } / Name[T]{ ... }: struct literal. */
static Expr *parse_struct_lit(Parser *p, Expr *base)
{
    Expr *e = expr_new(EX_STRUCT_LIT, base->span);
    if (base->kind == EX_GENERIC) {
        Expr *gb = base->u.generic.base;
        if (gb->kind != EX_IDENT)
            fatal_at(base->span, "struct literal name must be an identifier");
        e->u.stlit.name = gb->u.ident;
        e->u.stlit.targs.items = base->u.generic.targs.items;
        e->u.stlit.targs.len = base->u.generic.targs.len;
        e->u.stlit.targs.cap = base->u.generic.targs.cap;
    } else if (base->kind == EX_IDENT) {
        e->u.stlit.name = base->u.ident;
    } else {
        fatal_at(base->span, "invalid struct literal");
    }
    expect(p, TK_LBRACE, "'{' in struct literal");
    while (!at(p, TK_RBRACE)) {
        if (at(p, TK_DOT))
            advance(p); /* accept .field = e and field = e */
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected field name in struct literal");
        FieldInit fi;
        memset(&fi, 0, sizeof(fi));
        fi.name = tok(p)->name;
        fi.span = tok(p)->span;
        advance(p);
        expect(p, TK_ASSIGN, "'=' in struct literal field");
        fi.val = parse_expr(p);
        da_push(&e->u.stlit.fields, fi);
        if (at(p, TK_COMMA)) {
            advance(p);
            continue;
        }
        break;
    }
    expect(p, TK_RBRACE, "'}' after struct literal");
    return e;
}

static Expr *parse_postfix(Parser *p)
{
    Expr *e = parse_primary(p);
    for (;;) {
        switch (kind(p)) {
        case TK_LPAREN:
            e = parse_call_args(p, e);
            break;
        case TK_LBRACKET:
            e = parse_postfix_bracket(p, e);
            break;
        case TK_LBRACE:
            e = parse_struct_lit(p, e);
            break;
        case TK_DOT:
            advance(p);
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected field name after '.'");
            {
                Expr *f = expr_new(EX_FIELD, e->span);
                f->u.field.base = e;
                f->u.field.name = tok(p)->name;
                advance(p);
                e = f;
            }
            break;
        case TK_COLONCOLON: {
            advance(p);
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected symbol after '::'");
            if (e->kind != EX_IDENT)
                fatal_at(e->span, "namespace qualifier must be an identifier");
            Expr *ns = expr_new(EX_NS_IDENT, e->span);
            ns->u.nident.ns = e->u.ident;
            ns->u.nident.name = tok(p)->name;
            advance(p);
            e = ns;
            break;
        }
        case TK_AS: {
            advance(p);
            Expr *c = expr_new(EX_CAST, e->span);
            c->u.cast.e = e;
            c->u.cast.t = parse_type(p);
            e = c;
            break;
        }
        case TK_QUESTION: {
            advance(p);
            Expr *t = expr_new(EX_TRY, e->span);
            t->u.tryy = e;
            e = t;
            break;
        }
        case TK_BANG: {
            advance(p);
            Expr *u = expr_new(EX_UNWRAP, e->span);
            u->u.unwrap = e;
            e = u;
            break;
        }
        default:
            return e;
        }
    }
}

static Expr *parse_unary(Parser *p)
{
    Span sp = tok(p)->span;
    if (at(p, TK_STAR)) {
        advance(p);
        Expr *e = expr_new(EX_DEREF, sp);
        e->u.deref = parse_unary(p);
        return e;
    }
    if (at(p, TK_MINUS) || at(p, TK_BANG) || at(p, TK_AMP)) {
        TokenKind op = kind(p);
        advance(p);
        Expr *e = expr_new(EX_UNARY, sp);
        e->u.unary.op = op;
        if (op == TK_AMP && at(p, TK_MUT)) {
            advance(p);
            e->borrow_mut = 1;
        }
        e->u.unary.e = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

static int bin_prec(TokenKind k)
{
    switch (k) {
    case TK_OROR: return 1;
    case TK_ANDAND: return 2;
    case TK_EQ:
    case TK_NEQ: return 3;
    case TK_LT:
    case TK_GT:
    case TK_LE:
    case TK_GE: return 4;
    case TK_PLUS:
    case TK_MINUS: return 5;
    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT: return 6;
    default: return 0;
    }
}

static Expr *parse_binary(Parser *p, int min_prec)
{
    Expr *lhs = parse_unary(p);
    for (;;) {
        int prec = bin_prec(kind(p));
        if (prec < min_prec || prec == 0)
            return lhs;
        TokenKind op = kind(p);
        advance(p);
        Expr *e = expr_new(EX_BINARY, lhs->span);
        e->u.binary.op = op;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_binary(p, prec + 1);
        lhs = e;
    }
}

static Expr *parse_expr(Parser *p)
{
    Expr *lhs = parse_binary(p, 1);
    if (at(p, TK_ASSIGN)) {
        advance(p);
        Expr *e = expr_new(EX_ASSIGN, lhs->span);
        e->u.assign.lhs = lhs;
        e->u.assign.rhs = parse_expr(p);
        return e;
    }
    return lhs;
}

/* ---- statements ---- */

static Block *parse_block(Parser *p);
static Stmt *parse_stmt(Parser *p);

static Block *parse_block(Parser *p)
{
    expect(p, TK_LBRACE, "'{'");
    Block *b = block_new();
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF))
        da_push(&b->stmts, parse_stmt(p));
    expect(p, TK_RBRACE, "'}'");
    return b;
}

static void check_defer_target(Stmt *inner)
{
    switch (inner->kind) {
    case ST_RETURN:
    case ST_BREAK:
    case ST_CONTINUE:
        fatal_at(inner->span, "defer cannot wrap %s",
                 inner->kind == ST_RETURN ? "return"
                 : inner->kind == ST_BREAK ? "break" : "continue");
    default:
        break;
    }
}

static Stmt *parse_if_stmt(Parser *p)
{
    Span sp = tok(p)->span;
    advance(p); /* 'if' */
    expect(p, TK_LPAREN, "'(' after 'if'");
    Stmt *s = stmt_new(ST_IF, sp);
    s->u.iff.cond = parse_expr(p);
    expect(p, TK_RPAREN, "')' after condition");
    s->u.iff.then_blk = parse_block(p);
    if (at(p, TK_ELSE)) {
        advance(p);
        if (at(p, TK_IF)) {
            s->u.iff.else_stmt = parse_if_stmt(p);
        } else {
            Stmt *eb = stmt_new(ST_BLOCK, tok(p)->span);
            eb->u.block = parse_block(p);
            s->u.iff.else_stmt = eb;
        }
    }
    eat_semi(p);
    return s;
}

static Stmt *parse_stmt(Parser *p)
{
    Span sp = tok(p)->span;
    switch (kind(p)) {
    case TK_LET: {
        advance(p);
        Stmt *s = stmt_new(ST_LET, sp);
        s->u.let.mut = 0;
        if (at(p, TK_MUT)) {
            s->u.let.mut = 1;
            advance(p);
        }
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected binding name");
        s->u.let.name = tok(p)->name;
        advance(p);
        if (at(p, TK_COLON)) {
            advance(p);
            s->u.let.ty = parse_type(p);
        }
        expect(p, TK_ASSIGN, "'=' in let binding");
        s->u.let.init = parse_expr(p);
        expect(p, TK_SEMI, "';' after let");
        return s;
    }
    case TK_DEFER: {
        advance(p);
        Stmt *inner = parse_stmt(p);
        check_defer_target(inner);
        Stmt *s = stmt_new(ST_DEFER, sp);
        s->u.defer = inner;
        return s;
    }
    case TK_IF:
        return parse_if_stmt(p);
    case TK_WHILE: {
        advance(p);
        expect(p, TK_LPAREN, "'(' after 'while'");
        Stmt *s = stmt_new(ST_WHILE, sp);
        s->u.whilee.cond = parse_expr(p);
        expect(p, TK_RPAREN, "')' after condition");
        s->u.whilee.body = parse_block(p);
        eat_semi(p);
        return s;
    }
    case TK_RETURN: {
        advance(p);
        Stmt *s = stmt_new(ST_RETURN, sp);
        if (!at(p, TK_SEMI))
            s->u.ret = parse_expr(p);
        expect(p, TK_SEMI, "';' after return");
        return s;
    }
    case TK_BREAK:
        advance(p);
        expect(p, TK_SEMI, "';' after break");
        return stmt_new(ST_BREAK, sp);
    case TK_CONTINUE:
        advance(p);
        expect(p, TK_SEMI, "';' after continue");
        return stmt_new(ST_CONTINUE, sp);
    case TK_LBRACE: {
        Stmt *s = stmt_new(ST_BLOCK, sp);
        s->u.block = parse_block(p);
        eat_semi(p);
        return s;
    }
    case TK_FOR: {
        // for (let i in a..b) { ... }: half-open range, immutable index
        advance(p);
        expect(p, TK_LPAREN, "'(' after 'for'");
        expect(p, TK_LET, "'let' in range for");
        Stmt *s = stmt_new(ST_FOR_RANGE, sp);
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected loop variable name");
        s->u.forr.name = tok(p)->name;
        advance(p);
        expect(p, TK_IN, "'in' after loop variable");
        s->u.forr.start = parse_expr(p);
        expect(p, TK_DOTDOT, "'..' in range for");
        s->u.forr.end = parse_expr(p);
        expect(p, TK_RPAREN, "')' after range");
        s->u.forr.body = parse_block(p);
        eat_semi(p);
        return s;
    }
    case TK_ENUM:
        fatal_at(sp, "'enum' is not supported in v0.4");
        return NULL;
    default: {
        Stmt *s = stmt_new(ST_EXPR, sp);
        s->u.expr = parse_expr(p);
        expect(p, TK_SEMI, "';' after expression");
        return s;
    }
    }
}

/* ---- top level ---- */

static Decl *parse_use(Parser *p, Span sp, int exported)
{
    Decl *d = decl_new(DL_USE, sp);
    d->exported = exported;
    d->u.use.alias = NULL;
    advance(p); /* 'use' */
    if (at(p, TK_LT)) {
        /* C header path: raw source slice up to the closing '>' token. */
        advance(p);
        const char *start = tok(p)->start;
        while (!at(p, TK_GT) && !at(p, TK_EOF))
            advance(p);
        if (start == tok(p)->start)
            fatal_at(tok(p)->span, "empty C header path");
        d->u.use.is_c_header = 1;
        d->u.use.path = str_intern(start, tok(p)->start);
        advance(p); /* '>' */
    } else if (at(p, TK_STR)) {
        d->u.use.is_c_header = 1;
        d->u.use.path = str_intern_c(tok(p)->str_val);
        advance(p);
    } else if (at(p, TK_IDENT)) {
        /* module path: std::io::x recorded verbatim for later phases */
        DA(char) path = {0};
        const char *seg = tok(p)->name;
        while (*seg)
            da_push(&path, *seg++);
        advance(p);
        while (at(p, TK_COLONCOLON)) {
            advance(p);
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected module segment after '::'");
            const char *sep = "::";
            while (*sep)
                da_push(&path, *sep++);
            seg = tok(p)->name;
            while (*seg)
                da_push(&path, *seg++);
            advance(p);
        }
        da_push(&path, '\0');
        d->u.use.is_c_header = 0;
        d->u.use.path = path.items;
        d->u.use.alias = seg; /* default alias: last module segment */
    } else {
        fatal_at(tok(p)->span, "expected '<header.h>', \"header.h\" or "
                                "module path after 'use'");
    }
    if (at(p, TK_AS)) {
        advance(p);
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected alias name after 'as'");
        d->u.use.alias = tok(p)->name;
        advance(p);
    } else if (d->u.use.is_c_header) {
        fatal_at(tok(p)->span, "C header imports require 'as <alias>'");
    }
    expect(p, TK_SEMI, "';' after use declaration");
    return d;
}

static Decl *parse_type_decl(Parser *p, Span sp, int exported)
{
    advance(p); /* 'type' */
    if (!at(p, TK_IDENT))
        fatal_at(tok(p)->span, "expected type name");
    Decl *d = decl_new(DL_TYPE, sp);
    d->exported = exported;
    d->u.type.name = tok(p)->name;
    advance(p);
    if (at(p, TK_LBRACKET)) {
        advance(p);
        for (;;) {
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected type parameter name");
            da_push(&d->u.type.tparams, tok(p)->name);
            advance(p);
            if (at(p, TK_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        expect(p, TK_RBRACKET, "']' after type parameters");
    }
    expect(p, TK_ASSIGN, "'=' after type name");
    if (at(p, TK_RESOURCE)) {
        d->u.type.is_resource = 1;
        advance(p);
    }
    if (at(p, TK_STRUCT)) {
        advance(p);
        expect(p, TK_LBRACE, "'{'");
        while (!at(p, TK_RBRACE)) {
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected field name");
            Field f;
            f.name = tok(p)->name;
            f.span = tok(p)->span;
            advance(p);
            expect(p, TK_COLON, "':' after field name");
            f.ty = parse_type(p);
            da_push(&d->u.type.fields, f);
            if (at(p, TK_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        expect(p, TK_RBRACE, "'}'");
        eat_semi(p);
        return d;
    }
    /* alias form: type Name = T; (primitives, unions, !T error types) */
    d->u.type.fields.len = 0; /* no fields; alias target rides on ret? */
    Type *alias = parse_type(p);
    d->u.type.alias_ty = alias;
    expect(p, TK_SEMI, "';' after type alias");
    return d;
}

static Decl *parse_fn_decl(Parser *p, Span sp, int exported)
{
    advance(p); /* 'fn' */
    Decl *d = decl_new(DL_FN, sp);
    d->exported = exported;
    if (at(p, TK_LPAREN)) {
        /* receiver method: fn (self: RecvType) name(...) */
        advance(p);
        Param recv;
        memset(&recv, 0, sizeof(recv));
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected receiver name");
        recv.name = tok(p)->name;
        recv.span = tok(p)->span;
        advance(p);
        expect(p, TK_COLON, "':' after receiver name");
        recv.ty = parse_type(p);
        d->u.fn.recv = zalloc(sizeof(Param));
        *d->u.fn.recv = recv;
        expect(p, TK_RPAREN, "')' after receiver type");
        if (!at(p, TK_IDENT))
            fatal_at(tok(p)->span, "expected method name");
        /* receiver structs name their methods: Vec__push, Point__len */
        Type *sname = recv.ty;
        while (sname) {
            if (sname->kind == TY_REF || sname->kind == TY_MUTREF)
                sname = sname->base;
            else if (sname->kind == TY_INST)
                sname = type_struct(sname->name);
            else
                break;
        }
        if (!sname || sname->kind != TY_STRUCT || !sname->name)
            fatal_at(recv.span, "receiver must be a struct type");
        char mangled[256];
        snprintf(mangled, sizeof(mangled), "%s__%s", sname->name,
                 tok(p)->name);
        d->u.fn.name = str_intern_c(mangled);
    }
    if (!at(p, TK_IDENT))
        fatal_at(tok(p)->span, "expected function name");
    if (!d->u.fn.name)
        d->u.fn.name = tok(p)->name;
    advance(p);
    if (at(p, TK_LBRACKET)) {
        advance(p);
        for (;;) {
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected type parameter name");
            da_push(&d->u.fn.tparams, tok(p)->name);
            advance(p);
            if (at(p, TK_COMMA)) {
                advance(p);
                continue;
            }
            break;
        }
        expect(p, TK_RBRACKET, "']' after type parameters");
    }
    expect(p, TK_LPAREN, "'(' after function name");
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            if (!at(p, TK_IDENT))
                fatal_at(tok(p)->span, "expected parameter name");
            Param pm;
            pm.name = tok(p)->name;
            pm.span = tok(p)->span;
            advance(p);
            expect(p, TK_COLON, "':' after parameter name");
            pm.ty = parse_type(p);
            da_push(&d->u.fn.params, pm);
            if (at(p, TK_COMMA)) {
                advance(p);
                if (at(p, TK_RPAREN))
                    break;
                continue;
            }
            break;
        }
    }
    expect(p, TK_RPAREN, "')'");
    if (at_type_start(p))
        d->u.fn.ret = parse_type(p);
    else
        d->u.fn.ret = type_prim(TY_VOID);
    expect(p, TK_ASSIGN, "'=' before function body");
    d->u.fn.body = parse_block(p);
    eat_semi(p);
    return d;
}

void parse_program(const char *src, const char *file, Program *prog)
{
    Parser *p = zalloc(sizeof(Parser));
    lex_init(&p->lx, src, file);
    advance(p);
    while (!at(p, TK_EOF)) {
        Span sp = tok(p)->span;
        int exported = 0;
        if (at(p, TK_EXPORT)) {
            exported = 1;
            advance(p);
        }
        switch (kind(p)) {
        case TK_USE:
            da_push(&prog->decls, parse_use(p, sp, exported));
            break;
        case TK_TYPE:
            da_push(&prog->decls, parse_type_decl(p, sp, exported));
            break;
        case TK_FN:
            da_push(&prog->decls, parse_fn_decl(p, sp, exported));
            break;
        default:
            fatal_at(sp, "expected 'use', 'type' or 'fn' at top level, "
                         "found %s", tok_desc(p));
        }
    }
}
