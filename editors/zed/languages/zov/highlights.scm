; Copyright (c) 2026 foggy (abit-foggy)
; All rights reserved.
;
; Redistribution and use in source and binary forms, with or without
; modification, are permitted provided that the following conditions are met:
;
; 1. Redistributions of source code must retain the above copyright notice,
;    this list of conditions and the following disclaimer.
; 2. Redistributions in binary form must reproduce the above copyright
;    notice, this list of conditions and the following disclaimer in the
;    documentation and/or other materials provided with the distribution.
;
; THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
; AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
; IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
; ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
; LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
; CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
; SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
; INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
; CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
; ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
; POSSIBILITY OF SUCH DAMAGE.

(line_comment) @comment
(block_comment) @comment

(fn_keyword) @keyword
(let_keyword) @keyword
(mut_keyword) @keyword
(type_keyword) @keyword
(resource_keyword) @keyword
(struct_keyword) @keyword
(enum_keyword) @keyword
(match_keyword) @keyword
(case_keyword) @keyword
(defer_keyword) @keyword
(orelse_keyword) @keyword
(use_keyword) @keyword
(as_keyword) @keyword
(export_keyword) @keyword
(test_keyword) @keyword
(return_keyword) @keyword
(for_keyword) @keyword
(in_keyword) @keyword
(while_keyword) @keyword
(if_keyword) @keyword
(else_keyword) @keyword
(nullable_keyword) @keyword
(null_keyword) @constant.builtin
(true_keyword) @constant.builtin
(false_keyword) @constant.builtin

(primitive_type) @type.builtin
(type_name) @type
(function_name) @function
(parameter_name) @variable.parameter
(field_name) @property
(member_name) @variable.member
(variant_name) @constant
(identifier) @variable

(int_literal) @number
(float_literal) @number
(char_literal) @string.special
(escape_sequence) @string.escape
(string_literal) @string
(interpolation) @embedded

(qualified_identifier) @variable

[
  "=" "==" "!=" "<" ">" "<=" ">="
  "+" "-" "*" "/" "%" "&" "|" "^" "~"
  "<<" ">>"
  "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  "&&" "||" "?" "!" "=>" "::" ".."
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "." "|"] @punctuation.delimiter
