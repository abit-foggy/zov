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

import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.TokenSet;

public final class ZovTokenTypes {
    public static final IElementType KEYWORD =
        new IElementType("ZOV_KEYWORD", ZovLanguage.INSTANCE);
    public static final IElementType PRIMITIVE_TYPE =
        new IElementType("ZOV_PRIMITIVE_TYPE", ZovLanguage.INSTANCE);
    public static final IElementType IDENTIFIER =
        new IElementType("ZOV_IDENTIFIER", ZovLanguage.INSTANCE);
    public static final IElementType NUMBER =
        new IElementType("ZOV_NUMBER", ZovLanguage.INSTANCE);
    public static final IElementType STRING =
        new IElementType("ZOV_STRING", ZovLanguage.INSTANCE);
    public static final IElementType ESCAPE =
        new IElementType("ZOV_ESCAPE", ZovLanguage.INSTANCE);
    public static final IElementType CHAR =
        new IElementType("ZOV_CHAR", ZovLanguage.INSTANCE);
    public static final IElementType COMMENT =
        new IElementType("ZOV_COMMENT", ZovLanguage.INSTANCE);
    public static final IElementType OPERATOR =
        new IElementType("ZOV_OPERATOR", ZovLanguage.INSTANCE);
    public static final IElementType PUNCTUATION =
        new IElementType("ZOV_PUNCTUATION", ZovLanguage.INSTANCE);
    public static final IElementType STRING_MARK =
        new IElementType("ZOV_STRING_MARK", ZovLanguage.INSTANCE);
    public static final IElementType LBRACE =
        new IElementType("ZOV_LBRACE", ZovLanguage.INSTANCE);
    public static final IElementType RBRACE =
        new IElementType("ZOV_RBRACE", ZovLanguage.INSTANCE);
    public static final IElementType LBRACKET =
        new IElementType("ZOV_LBRACKET", ZovLanguage.INSTANCE);
    public static final IElementType RBRACKET =
        new IElementType("ZOV_RBRACKET", ZovLanguage.INSTANCE);
    public static final IElementType LPAREN =
        new IElementType("ZOV_LPAREN", ZovLanguage.INSTANCE);
    public static final IElementType RPAREN =
        new IElementType("ZOV_RPAREN", ZovLanguage.INSTANCE);

    public static final TokenSet KEYWORDS = TokenSet.create(KEYWORD, PRIMITIVE_TYPE);
    public static final TokenSet COMMENTS = TokenSet.create(COMMENT);
    public static final TokenSet STRINGS = TokenSet.create(STRING, ESCAPE, CHAR, STRING_MARK);

    private ZovTokenTypes() {
    }
}
