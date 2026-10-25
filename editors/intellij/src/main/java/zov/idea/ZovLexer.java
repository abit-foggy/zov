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

import com.intellij.lexer.LexerBase;
import com.intellij.lexer.LexerPosition;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import java.util.Locale;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

// Token-level lexer for Zov, mirroring the token rules of the reference
// grammar: line and block comments, char literals, strings with {expr}
// interpolation and \{ brace escapes, hex and decimal integers with type
// suffixes, floats with exponents, keywords, and the operator set.
//
// States track whether the lexer sits at top level, inside a string literal,
// inside an interpolation, inside a string nested within an interpolation,
// or inside an interpolation of such a nested string. Brace depth inside an
// interpolation lives in an instance field and is rebuilt on lexing restarts;
// token boundaries always align to positions the state machine can resume
// from deterministically.
public final class ZovLexer extends LexerBase {
    private static final int NORMAL = 0;
    private static final int IN_STRING = 1;
    private static final int IN_INTERP = 2;
    private static final int IN_NESTED_STRING = 3;
    private static final int IN_NESTED_INTERP = 4;

    private static final String[] INT_SUFFIXES = {
        "usize", "isize", "u64", "i64", "f64", "f32", "u32", "i32", "u16", "i16", "u8", "i8",
    };
    private static final String[] FLOAT_SUFFIXES = {"f64", "f32"};

    private static final String[] KEYWORDS = {
        "as", "break", "case", "continue", "defer", "else", "enum", "export",
        "false", "fn", "for", "if", "in", "let", "match", "mut", "null",
        "nullable", "orelse", "resource", "return", "struct", "test", "true",
        "type", "use", "while",
    };
    private static final String[] PRIMITIVES = {
        "bool", "char", "f32", "f64", "i16", "i32", "i64", "i8", "isize",
        "str", "u16", "u32", "u64", "u8", "usize", "void",
    };

    private CharSequence buffer;
    private int bufferEnd;
    private int tokenStart;
    private int tokenEnd;
    private IElementType tokenType;
    private int state;
    private int stateBeforeToken;
    private int braceDepth;

    @Override
    public void start(@NotNull CharSequence buffer, int startOffset, int endOffset, int initialState) {
        this.buffer = buffer;
        this.bufferEnd = endOffset;
        this.tokenStart = startOffset;
        this.tokenEnd = startOffset;
        this.tokenType = null;
        this.state = initialState;
        this.stateBeforeToken = initialState;
        this.braceDepth = 0;
        advance();
    }

    @Override
    public int getState() {
        return state;
    }

    @Override
    public @Nullable IElementType getTokenType() {
        return tokenType;
    }

    @Override
    public int getTokenStart() {
        return tokenStart;
    }

    @Override
    public int getTokenEnd() {
        return tokenEnd;
    }

    @Override
    public @NotNull CharSequence getBufferSequence() {
        return buffer;
    }

    @Override
    public int getBufferEnd() {
        return bufferEnd;
    }

    @Override
    public void advance() {
        if (tokenEnd >= bufferEnd) {
            tokenStart = tokenEnd;
            tokenType = null;
            return;
        }
        tokenStart = tokenEnd;
        stateBeforeToken = state;
        switch (state) {
            case IN_STRING:
                lexStringBody(NORMAL);
                break;
            case IN_NESTED_STRING:
                lexStringBody(IN_INTERP);
                break;
            case IN_INTERP:
                lexInterpBody(IN_STRING);
                break;
            case IN_NESTED_INTERP:
                lexInterpBody(IN_NESTED_STRING);
                break;
            default:
                lexNormal();
                break;
        }
    }

    @Override
    public @NotNull LexerPosition getCurrentPosition() {
        // Save the state in effect when the current token began, so that a
        // restore replays the current token exactly (the live state field
        // may already reflect the transition this token triggers).
        return new ZovLexerPosition(stateBeforeToken, tokenStart);
    }

    @Override
    public void restore(@NotNull LexerPosition position) {
        int offset = position.getOffset();
        int savedState = position.getState();
        start(buffer, offset, bufferEnd, savedState);
    }

    private char at(int offset) {
        return buffer.charAt(offset);
    }

    private void done(IElementType type, int end) {
        tokenType = type;
        tokenEnd = Math.min(end, bufferEnd);
    }

    private boolean isWhitespace(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    private boolean isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    private boolean isHexDigit(char c) {
        return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    private boolean isIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    private boolean isIdentPart(char c) {
        return isIdentStart(c) || isDigit(c);
    }

    private void lexNormal() {
        char c = at(tokenEnd);
        if (isWhitespace(c)) {
            int i = tokenEnd + 1;
            while (i < bufferEnd && isWhitespace(at(i))) {
                i++;
            }
            done(TokenType.WHITE_SPACE, i);
            return;
        }
        if (c == '/' && tokenEnd + 1 < bufferEnd && at(tokenEnd + 1) == '/') {
            int i = tokenEnd + 2;
            while (i < bufferEnd && at(i) != '\n') {
                i++;
            }
            done(ZovTokenTypes.COMMENT, i);
            return;
        }
        if (c == '/' && tokenEnd + 1 < bufferEnd && at(tokenEnd + 1) == '*') {
            done(ZovTokenTypes.COMMENT, blockCommentEnd(tokenEnd + 2));
            return;
        }
        if (c == '"') {
            done(ZovTokenTypes.STRING, tokenEnd + 1);
            state = IN_STRING;
            return;
        }
        if (c == '\'') {
            lexCharLiteral();
            return;
        }
        if (isDigit(c)) {
            lexNumber();
            return;
        }
        if (isIdentStart(c)) {
            lexIdentifier();
            return;
        }
        lexOperator();
    }

    private int blockCommentEnd(int from) {
        int i = from;
        while (i < bufferEnd) {
            if (at(i) == '*' && i + 1 < bufferEnd && at(i + 1) == '/') {
                return i + 2;
            }
            i++;
        }
        return bufferEnd;
    }

    private void lexCharLiteral() {
        // matches the reference rule '(\\.|[^'\\])': one char or one escape,
        // then the closing quote; resilient at end of buffer
        int i = tokenEnd + 1;
        if (i < bufferEnd && at(i) == '\\' && i + 1 < bufferEnd) {
            i += 2;
        } else if (i < bufferEnd && at(i) != '\'' && at(i) != '\\') {
            i++;
        }
        if (i < bufferEnd && at(i) == '\'') {
            i++;
        }
        done(ZovTokenTypes.CHAR, i);
    }

    private void lexNumber() {
        int i = tokenEnd;
        if (at(i) == '0' && i + 1 < bufferEnd && (at(i + 1) == 'x' || at(i + 1) == 'X')) {
            i += 2;
            while (i < bufferEnd && isHexDigit(at(i))) {
                i++;
            }
            i += suffixLength(i, INT_SUFFIXES);
            done(ZovTokenTypes.NUMBER, i);
            return;
        }
        while (i < bufferEnd && isDigit(at(i))) {
            i++;
        }
        boolean isFloat = false;
        if (i + 1 < bufferEnd && at(i) == '.' && isDigit(at(i + 1))) {
            isFloat = true;
            i++;
            while (i < bufferEnd && isDigit(at(i))) {
                i++;
            }
        }
        if (i < bufferEnd && (at(i) == 'e' || at(i) == 'E')) {
            int j = i + 1;
            if (j < bufferEnd && (at(j) == '+' || at(j) == '-')) {
                j++;
            }
            if (j < bufferEnd && isDigit(at(j))) {
                isFloat = true;
                i = j;
                while (i < bufferEnd && isDigit(at(i))) {
                    i++;
                }
            }
        }
        i += suffixLength(i, isFloat ? FLOAT_SUFFIXES : INT_SUFFIXES);
        done(ZovTokenTypes.NUMBER, i);
    }

    private int suffixLength(int offset, String[] suffixes) {
        for (String suffix : suffixes) {
            int end = offset + suffix.length();
            if (end <= bufferEnd && buffer.subSequence(offset, end).toString()
                    .regionMatches(0, suffix, 0, suffix.length())) {
                return suffix.length();
            }
        }
        return 0;
    }

    private void lexIdentifier() {
        int i = tokenEnd + 1;
        while (i < bufferEnd && isIdentPart(at(i))) {
            i++;
        }
        String text = buffer.subSequence(tokenEnd, i).toString();
        if (isKeyword(text)) {
            done(ZovTokenTypes.KEYWORD, i);
        } else if (isPrimitive(text)) {
            done(ZovTokenTypes.PRIMITIVE_TYPE, i);
        } else {
            done(ZovTokenTypes.IDENTIFIER, i);
        }
    }

    private boolean isKeyword(String text) {
        for (String keyword : KEYWORDS) {
            if (keyword.equals(text)) {
                return true;
            }
        }
        return false;
    }

    private boolean isPrimitive(String text) {
        for (String primitive : PRIMITIVES) {
            if (primitive.equals(text)) {
                return true;
            }
        }
        return false;
    }

    private static final String[] THREE_CHAR_OPS = {"<<=", ">>="};
    private static final String[] TWO_CHAR_OPS = {
        "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "..", "=>", "::",
    };
    private static final String OPERATOR_CHARS = "+-*/%<>=!&|^~?";

    private void lexOperator() {
        int i = tokenEnd;
        if (i + 3 <= bufferEnd) {
            for (String op : THREE_CHAR_OPS) {
                if (matchesAt(op, i)) {
                    done(ZovTokenTypes.OPERATOR, i + 3);
                    return;
                }
            }
        }
        if (i + 2 <= bufferEnd) {
            for (String op : TWO_CHAR_OPS) {
                if (matchesAt(op, i)) {
                    done(ZovTokenTypes.OPERATOR, i + 2);
                    return;
                }
            }
        }
        char c = at(i);
        if (OPERATOR_CHARS.indexOf(c) >= 0) {
            done(ZovTokenTypes.OPERATOR, i + 1);
            return;
        }
        switch (c) {
            case '{':
                done(ZovTokenTypes.LBRACE, i + 1);
                return;
            case '}':
                done(ZovTokenTypes.RBRACE, i + 1);
                return;
            case '[':
                done(ZovTokenTypes.LBRACKET, i + 1);
                return;
            case ']':
                done(ZovTokenTypes.RBRACKET, i + 1);
                return;
            case '(':
                done(ZovTokenTypes.LPAREN, i + 1);
                return;
            case ')':
                done(ZovTokenTypes.RPAREN, i + 1);
                return;
            default:
                break;
        }
        if (c == ':' || c == ';' || c == ',' || c == '.') {
            done(ZovTokenTypes.PUNCTUATION, i + 1);
            return;
        }
        done(TokenType.BAD_CHARACTER, i + 1);
    }

    private boolean matchesAt(String op, int offset) {
        int end = offset + op.length();
        if (end > bufferEnd) {
            return false;
        }
        for (int i = 0; i < op.length(); i++) {
            if (at(offset + i) != op.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    private void lexStringBody(int stateAfterQuote) {
        char c = at(tokenEnd);
        if (c == '"') {
            done(ZovTokenTypes.STRING, tokenEnd + 1);
            state = stateAfterQuote;
            return;
        }
        if (c == '\\' && tokenEnd + 1 < bufferEnd) {
            int i = tokenEnd + 2;
            char next = at(tokenEnd + 1);
            if (next == 'x') {
                while (i < bufferEnd && isHexDigit(at(i)) && i - (tokenEnd + 2) < 2) {
                    i++;
                }
            }
            done(ZovTokenTypes.ESCAPE, i);
            return;
        }
        if (c == '{') {
            done(ZovTokenTypes.STRING_MARK, tokenEnd + 1);
            braceDepth = 0;
            state = state == IN_NESTED_STRING ? IN_NESTED_INTERP : IN_INTERP;
            return;
        }
        int i = tokenEnd + 1;
        while (i < bufferEnd) {
            char c2 = at(i);
            if (c2 == '"' || c2 == '\\' || c2 == '{') {
                break;
            }
            i++;
        }
        done(ZovTokenTypes.STRING, i);
    }

    private void lexInterpBody(int stateAfterClose) {
        char c = at(tokenEnd);
        if (isWhitespace(c)) {
            int i = tokenEnd + 1;
            while (i < bufferEnd && isWhitespace(at(i))) {
                i++;
            }
            done(TokenType.WHITE_SPACE, i);
            return;
        }
        if (c == '"') {
            done(ZovTokenTypes.STRING, tokenEnd + 1);
            state = IN_NESTED_STRING;
            return;
        }
        if (c == '}') {
            if (braceDepth > 0) {
                braceDepth--;
                done(ZovTokenTypes.RBRACE, tokenEnd + 1);
            } else {
                done(ZovTokenTypes.STRING_MARK, tokenEnd + 1);
                state = stateAfterClose;
            }
            return;
        }
        if (c == '{') {
            braceDepth++;
            done(ZovTokenTypes.LBRACE, tokenEnd + 1);
            return;
        }
        if (c == '/' && tokenEnd + 1 < bufferEnd && at(tokenEnd + 1) == '/') {
            int i = tokenEnd + 2;
            while (i < bufferEnd && at(i) != '\n') {
                i++;
            }
            done(ZovTokenTypes.COMMENT, i);
            return;
        }
        if (isDigit(c)) {
            lexNumber();
            return;
        }
        if (isIdentStart(c)) {
            lexIdentifier();
            return;
        }
        lexOperator();
    }

    private static final class ZovLexerPosition implements LexerPosition {
        private final int state;
        private final int offset;

        private ZovLexerPosition(int state, int offset) {
            this.state = state;
            this.offset = offset;
        }

        @Override
        public int getOffset() {
            return offset;
        }

        @Override
        public int getState() {
            return state;
        }

        @Override
        public String toString() {
            return String.format(Locale.ROOT, "zov lexer position: state=%d offset=%d", state, offset);
        }
    }
}
