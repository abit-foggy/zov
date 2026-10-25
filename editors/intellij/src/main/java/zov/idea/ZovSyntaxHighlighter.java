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

package zov.idea;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.HighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import static com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighter;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import org.jetbrains.annotations.NotNull;

public final class ZovSyntaxHighlighter implements SyntaxHighlighter {
    public static final TextAttributesKey KEYWORD =
        createTextAttributesKey("ZOV_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey PRIMITIVE_TYPE =
        createTextAttributesKey("ZOV_PRIMITIVE_TYPE", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey NUMBER =
        createTextAttributesKey("ZOV_NUMBER", DefaultLanguageHighlighterColors.NUMBER);
    public static final TextAttributesKey STRING =
        createTextAttributesKey("ZOV_STRING", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey ESCAPE =
        createTextAttributesKey("ZOV_ESCAPE", DefaultLanguageHighlighterColors.VALID_STRING_ESCAPE);
    public static final TextAttributesKey CHAR =
        createTextAttributesKey("ZOV_CHAR", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey COMMENT =
        createTextAttributesKey("ZOV_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
    public static final TextAttributesKey OPERATOR =
        createTextAttributesKey("ZOV_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    public static final TextAttributesKey PUNCTUATION =
        createTextAttributesKey("ZOV_PUNCTUATION", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    public static final TextAttributesKey STRING_MARK =
        createTextAttributesKey("ZOV_STRING_MARK", DefaultLanguageHighlighterColors.VALID_STRING_ESCAPE);
    public static final TextAttributesKey BRACES =
        createTextAttributesKey("ZOV_BRACES", DefaultLanguageHighlighterColors.BRACES);
    public static final TextAttributesKey BRACKETS =
        createTextAttributesKey("ZOV_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS);
    public static final TextAttributesKey PARENTHESES =
        createTextAttributesKey("ZOV_PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES);
    public static final TextAttributesKey BAD_CHARACTER =
        createTextAttributesKey("ZOV_BAD_CHARACTER", HighlighterColors.BAD_CHARACTER);

    private static final TextAttributesKey[] KEYWORD_KEYS = {KEYWORD};
    private static final TextAttributesKey[] PRIMITIVE_KEYS = {PRIMITIVE_TYPE};
    private static final TextAttributesKey[] NUMBER_KEYS = {NUMBER};
    private static final TextAttributesKey[] STRING_KEYS = {STRING};
    private static final TextAttributesKey[] ESCAPE_KEYS = {ESCAPE};
    private static final TextAttributesKey[] CHAR_KEYS = {CHAR};
    private static final TextAttributesKey[] COMMENT_KEYS = {COMMENT};
    private static final TextAttributesKey[] OPERATOR_KEYS = {OPERATOR};
    private static final TextAttributesKey[] PUNCTUATION_KEYS = {PUNCTUATION};
    private static final TextAttributesKey[] STRING_MARK_KEYS = {STRING_MARK};
    private static final TextAttributesKey[] BRACES_KEYS = {BRACES};
    private static final TextAttributesKey[] BRACKETS_KEYS = {BRACKETS};
    private static final TextAttributesKey[] PARENTHESES_KEYS = {PARENTHESES};
    private static final TextAttributesKey[] BAD_CHARACTER_KEYS = {BAD_CHARACTER};
    private static final TextAttributesKey[] EMPTY_KEYS = new TextAttributesKey[0];

    @Override
    public @NotNull Lexer getHighlightingLexer() {
        return new ZovLexer();
    }

    @Override
    public TextAttributesKey @NotNull [] getTokenHighlights(IElementType tokenType) {
        if (tokenType == ZovTokenTypes.KEYWORD) {
            return KEYWORD_KEYS;
        }
        if (tokenType == ZovTokenTypes.PRIMITIVE_TYPE) {
            return PRIMITIVE_KEYS;
        }
        if (tokenType == ZovTokenTypes.NUMBER) {
            return NUMBER_KEYS;
        }
        if (tokenType == ZovTokenTypes.STRING) {
            return STRING_KEYS;
        }
        if (tokenType == ZovTokenTypes.ESCAPE) {
            return ESCAPE_KEYS;
        }
        if (tokenType == ZovTokenTypes.CHAR) {
            return CHAR_KEYS;
        }
        if (tokenType == ZovTokenTypes.COMMENT) {
            return COMMENT_KEYS;
        }
        if (tokenType == ZovTokenTypes.OPERATOR) {
            return OPERATOR_KEYS;
        }
        if (tokenType == ZovTokenTypes.PUNCTUATION) {
            return PUNCTUATION_KEYS;
        }
        if (tokenType == ZovTokenTypes.STRING_MARK) {
            return STRING_MARK_KEYS;
        }
        if (tokenType == ZovTokenTypes.LBRACE || tokenType == ZovTokenTypes.RBRACE) {
            return BRACES_KEYS;
        }
        if (tokenType == ZovTokenTypes.LBRACKET || tokenType == ZovTokenTypes.RBRACKET) {
            return BRACKETS_KEYS;
        }
        if (tokenType == ZovTokenTypes.LPAREN || tokenType == ZovTokenTypes.RPAREN) {
            return PARENTHESES_KEYS;
        }
        if (tokenType == TokenType.BAD_CHARACTER) {
            return BAD_CHARACTER_KEYS;
        }
        return EMPTY_KEYS;
    }
}
