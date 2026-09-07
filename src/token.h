#pragma once

#include <cstdint>
#include <string_view>
#include <string>

enum class TokenType : std::uint8_t {
    // Single-character tokens.
    LEFT_PAREN,
    RIGHT_PAREN,
    LEFT_BRACE,
    RIGHT_BRACE,
    LEFT_BRACKET,
    RIGHT_BRACKET,
    COMMA,
    COLON,
    DOT,
    ELIPSIS,
    MINUS,
    PLUS,
    SEMICOLON,
    SLASH,
    STAR,
    PERCENT,
    AT,

    // One or two character tokens.
    BANG,
    BANG_EQUAL,
    EQUAL,
    EQUAL_EQUAL,
    FAT_ARROW,
    GREATER,
    GREATER_EQUAL,
    LESS,
    LESS_EQUAL,

    // Literals.
    IDENTIFIER,
    STRING,
    NUMBER,

    // Keywords.
    AND,
    BREAK,
    CASE,
    CLASS,
    CONTINUE,
    DEFAULT,
    ELSE,
    ENUM,
    FALSE,
    FUN,
    FOR,
    IF,
    IN,
    NIL,
    OR,
    PRINT,
    RETURN,
    MATCH,
    SUPER,
    THIS,
    TRUE,
    VAR,
    WHILE,

    // End of file.
    EOF_,

    // Error token.
    ERROR,
};

struct Token {
    TokenType type;
    // The semantic value of the token, not always its source text: STRING has
    // the surrounding quotes removed, ERROR carries a message literal. Use
    // offset and length for the source span.
    std::string_view lexeme;
    std::size_t line;
    // Source span of the token: its text is source.substr(offset, length),
    // where offset is the byte index of the first character from the start of
    // the source buffer. This holds for every token type, including the quotes
    // of a STRING and the offending run of an ERROR. length can differ from
    // lexeme.size() (STRING, ERROR). A token that the compiler builds by hand
    // (see src/compiler.cpp) has no source behind it; both fields stay 0, and
    // 0 there means "no span", not "position 0".
    std::size_t offset = 0;
    std::size_t length = 0;
};

std::ostream& operator<<(std::ostream& os, const TokenType& type);