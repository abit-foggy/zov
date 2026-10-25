// Copyright (c) 2026 foggy (abit-foggy)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/**
 * @type {Rules}
 *
 * Bracket postfix is unified: '[' after an expression covers indexing,
 * slicing, and generic instantiation (contents are types or expressions,
 * GLR-resolved). Struct literals are a '{' postfix on any expression so
 * `Vec[T]{...}` chains naturally through the subscript rule.
 */
module.exports = grammar({
  name: 'zov',
  word: $ => $.identifier,

  extras: $ => [
    /[\s\uFEFF\u2060\u200B]/,
    $.line_comment,
    $.block_comment,
  ],

  conflicts: $ => [
    [$.type_name, $._expr],
    [$.type_name, $.qualified_identifier],
    [$.if_statement],
    [$._expr, $._type],
    [$.array_type, $._expr],
  ],

  rules: {
    source_file: $ => repeat(choice(
      $.use_declaration,
      $.type_declaration,
      $.function_declaration,
      $.test_declaration,
    )),

    // ---- declarations ----

    use_declaration: $ => seq(
      $.use_keyword,
      choice(
        seq('<', $.header_path, '>'),
        $.string_literal,
        $.module_path,
      ),
      optional(seq($.as_keyword, $.identifier)),
      ';',
    ),
    header_path: _ => /[a-zA-Z0-9_.\/-]+/,
    module_path: $ => seq($.identifier, repeat1(seq('::', $.identifier))),

    type_declaration: $ => seq(
      optional($.export_keyword),
      $.type_keyword,
      field('name', $.type_name),
      optional($.type_parameters),
      '=',
      choice($.struct_definition, $.enum_definition, $._type),
      optional(';'),
    ),
    type_parameters: $ => seq('[', commaSep1Trailing($.identifier), ']'),

    struct_definition: $ => seq(
      optional($.resource_keyword),
      $.struct_keyword,
      '{',
      optional(commaSep1Trailing($.struct_field)),
      '}',
    ),
    struct_field: $ => seq(field('name', $.field_name), ':', field('type', $._type)),
    field_name: $ => $.identifier,

    enum_definition: $ => seq(
      $.enum_keyword,
      '{',
      optional(commaSep1Trailing($.enum_variant)),
      '}',
    ),
    enum_variant: $ => seq($.variant_name, optional(seq('=', $.int_literal))),
    variant_name: $ => $.identifier,

    function_declaration: $ => seq(
      optional($.export_keyword),
      $.fn_keyword,
      optional(seq('(', field('receiver', $.parameter), ')')),
      field('name', $.function_name),
      optional($.type_parameters),
      '(',
      optional(commaSep1Trailing(field('parameter', $.parameter))),
      ')',
      optional(field('return_type', $._type)),
      '=',
      field('body', $.block),
      optional(';'),
    ),
    function_name: $ => $.identifier,

    parameter: $ => seq(field('name', $.parameter_name), ':', field('type', $._type)),
    parameter_name: $ => $.identifier,

    test_declaration: $ => seq(
      $.test_keyword,
      field('description', $.string_literal),
      field('body', $.block),
      optional(';'),
    ),

    // ---- types ----

    _type: $ => choice(
      $.primitive_type,
      $.pointer_type,
      $.nullable_pointer_type,
      $.reference_type,
      $.slice_type,
      $.array_type,
      $.union_type,
      $.error_type,
      $.named_type,
    ),
    primitive_type: _ => choice(
      'i8', 'i16', 'i32', 'i64', 'isize',
      'u8', 'u16', 'u32', 'u64', 'usize',
      'f32', 'f64', 'bool', 'char', 'str', 'void',
    ),
    pointer_type: $ => seq('*', $._type),
    nullable_pointer_type: $ => seq($.nullable_keyword, '*', $._type),
    reference_type: $ => seq('&', optional($.mut_keyword), $._type),
    slice_type: $ => seq('[', ']', $._type),
    array_type: $ => seq('[', $.int_literal, ']', $._type),
    union_type: $ => seq('(', $._type, repeat1(seq('|', $._type)), ')'),
    error_type: $ => seq('!', $._type),
    named_type: $ => prec.right(seq(
      optional(seq($.identifier, '::')),
      $.type_name,
      optional($.type_arguments),
    )),
    type_name: $ => $.identifier,
    type_arguments: $ => seq('[', commaSep1Trailing($._type), ']'),

    // ---- statements ----

    block: $ => seq('{', repeat($._statement), '}'),

    _statement: $ => choice(
      $.let_declaration,
      $.defer_statement,
      $.if_statement,
      $.while_statement,
      $.for_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.expression_statement,
      $.block,
    ),

    let_declaration: $ => seq(
      $.let_keyword,
      optional($.mut_keyword),
      $.identifier,
      optional(seq(':', $._type)),
      optional(seq('=', $._expr)),
      ';',
    ),

    defer_statement: $ => seq($.defer_keyword, $._statement),

    if_statement: $ => seq(
      $.if_keyword,
      '(',
      $._expr,
      ')',
      $.block,
      optional(seq($.else_keyword, choice($.if_statement, $.block))),
      optional(';'),
    ),

    while_statement: $ => seq(
      $.while_keyword,
      '(',
      $._expr,
      ')',
      $.block,
      optional(';'),
    ),

    for_statement: $ => seq(
      $.for_keyword,
      '(',
      $.let_keyword,
      $.identifier,
      optional(seq(',', $.identifier)),
      $.in_keyword,
      choice($.range_expression, $._expr),
      ')',
      $.block,
      optional(';'),
    ),

    range_expression: $ => prec.right(1, seq($._expr, optional(seq('..', optional($._expr))))),

    return_statement: $ => seq($.return_keyword, optional($._expr), ';'),
    break_statement: _ => seq('break', ';'),
    continue_statement: _ => seq('continue', ';'),
    expression_statement: $ => seq($._expr, ';'),

    // ---- match ----

    match_expression: $ => seq(
      $.match_keyword,
      '(',
      $._expr,
      ')',
      '{',
      repeat(choice(
        seq($.match_arm, ','),
        $.match_arm,
      )),
      '}',
    ),

    match_arm: $ => seq(
      $.case_keyword,
      choice(
        $.null_keyword,
        $.wildcard_pattern,
        $.bind_pattern,
        $.literal_pattern,
        $.type_pattern,
      ),
      '=>',
      choice(
        $._expr,
        $.return_jump,
        $.break_jump,
        $.continue_jump,
        $.block,
      ),
    ),
    wildcard_pattern: _ => '_',
    bind_pattern: $ => seq($.let_keyword, $.identifier, ':', $._type),
    literal_pattern: $ => choice(
      $.int_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.true_keyword,
      $.false_keyword,
    ),
    type_pattern: $ => seq($.identifier, repeat(seq('::', $.identifier))),
    return_jump: $ => prec.right(1, seq($.return_keyword, $._expr)),
    break_jump: _ => 'break',
    continue_jump: _ => 'continue',

    // ---- expressions ----

    _expr: $ => choice(
      $.orelse_expression,
      $.assignment_expression,
      $.binary_expression,
      $.unary_expression,
      $.dereference_expression,
      $.borrow_expression,
      $.call_expression,
      $.field_expression,
      $.subscript,
      $.struct_literal,
      $.array_literal,
      $.try_expression,
      $.unwrap_expression,
      $.cast_expression,
      $.match_expression,
      $.identifier,
      $.qualified_identifier,
      $.int_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.true_keyword,
      $.false_keyword,
      $.null_keyword,
      $.parenthesized_expression,
    ),

    parenthesized_expression: $ => seq('(', $._expr, ')'),

    // orelse binds tighter than '=' so `let x = f() orelse 0` unwraps f()
    orelse_expression: $ => prec.right(1, seq(
      $._expr,
      $.orelse_keyword,
      choice($._expr, $.return_jump, $.break_jump, $.continue_jump),
    )),

    assignment_expression: $ => prec.right(0, seq(
      $._expr,
      choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='),
      $._expr,
    )),

    binary_expression: $ => {
      const table = [
        ['||', 1], ['&&', 2],
        ['|', 3], ['^', 4], ['&', 5],
        ['==', 6], ['!=', 6],
        ['<', 7], ['>', 7], ['<=', 7], ['>=', 7],
        ['<<', 8], ['>>', 8],
        ['+', 9], ['-', 9],
        ['*', 10], ['/', 10], ['%', 10],
      ];
      return choice(...table.map(([op, precedence]) =>
        prec.left(precedence, seq($._expr, op, $._expr))));
    },

    unary_expression: $ => prec(11, seq(choice('!', '-', '~'), $._expr)),
    dereference_expression: $ => prec(11, seq('*', $._expr)),
    borrow_expression: $ => prec(11, seq('&', optional($.mut_keyword), $._expr)),

    call_expression: $ => prec.left(12, seq($._expr, '(', optional($.arguments), ')')),
    arguments: $ => commaSep1Trailing($._expr),

    field_expression: $ => prec.left(13, seq($._expr, '.', $.member_name)),
    member_name: $ => $.identifier,

    // unified '[' postfix: index, slice, or generic instantiation
    subscript: $ => prec.left(12, seq(
      $._expr,
      '[',
      optional(choice(
        $.slice_expression,
        commaSep1Trailing(choice($._expr, $._type)),
      )),
      ']',
    )),
    slice_expression: $ => seq(
      optional(choice($._expr, $._type)),
      '..',
      optional(choice($._expr, $._type)),
    ),

    try_expression: $ => prec.left(13, seq($._expr, '?')),
    unwrap_expression: $ => prec.left(13, seq($._expr, '!')),
    cast_expression: $ => prec.left(12, seq($._expr, $.as_keyword, $._type)),

    qualified_identifier: $ => prec.right(11, seq(
      $.identifier,
      repeat1(seq('::', $.identifier)),
    )),

    // '{' postfix: `Name{...}`, `Vec[T]{...}` chains through _expr
    struct_literal: $ => prec.left(12, seq(
      $._expr,
      '{',
      optional(commaSep1Trailing($.init_field)),
      optional('..'),
      '}',
    )),
    init_field: $ => seq(optional('.'), $.field_name, '=', $._expr),

    // bare '[' at expression start: fixed-array literal
    array_literal: $ => prec(12, seq(
      '[',
      optional(commaSep1Trailing($._expr)),
      ']',
    )),

    // ---- literals ----

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,

    int_literal: _ => /((0[xX][0-9a-fA-F]+)|[0-9]+)(i8|i16|i32|i64|isize|u8|u16|u32|u64|usize)?/,

    float_literal: _ => /([0-9]+\.[0-9]+([eE][-+]?[0-9]+)?(f32|f64)?|[0-9]+[eE][-+]?[0-9]+(f32|f64)?|[0-9]+(f32|f64))/,

    char_literal: _ => /'(\\.|[^'\\])'/,

    // '{' inside strings is interpolation; '\{' is a literal brace escape
    string_literal: $ => seq(
      '"',
      repeat(choice(
        $.escape_sequence,
        $.interpolation,
        token(prec(-1, /[^"\\\{]+/)),
      )),
      '"',
    ),

    interpolation: $ => seq('{', $._expr, '}'),
    escape_sequence: _ => /\\(x[0-9a-fA-F]{2}|.)/,

    line_comment: _ => token(seq('//', /[^\n]*/)),
    block_comment: _ => token(seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/')),

    // ---- keywords (named for query capture) ----
    fn_keyword: _ => 'fn',
    let_keyword: _ => 'let',
    mut_keyword: _ => 'mut',
    type_keyword: _ => 'type',
    resource_keyword: _ => 'resource',
    struct_keyword: _ => 'struct',
    enum_keyword: _ => 'enum',
    match_keyword: _ => 'match',
    case_keyword: _ => 'case',
    defer_keyword: _ => 'defer',
    orelse_keyword: _ => 'orelse',
    use_keyword: _ => 'use',
    as_keyword: _ => 'as',
    export_keyword: _ => 'export',
    test_keyword: _ => 'test',
    return_keyword: _ => 'return',
    for_keyword: _ => 'for',
    in_keyword: _ => 'in',
    while_keyword: _ => 'while',
    if_keyword: _ => 'if',
    else_keyword: _ => 'else',
    nullable_keyword: _ => 'nullable',
    null_keyword: _ => 'null',
    true_keyword: _ => 'true',
    false_keyword: _ => 'false',
  },
});

function commaSep1Trailing(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}
