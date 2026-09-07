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
    std::string_view lexeme;
    std::size_t line;
    // Byte offset of the token's first character from the start of the source
    // buffer. The token's source span is [offset, offset + lexeme.size()) for a
    // real token. This does NOT hold for an ERROR token: its lexeme is a
    // message literal, not a slice of the source, so a consumer must not treat
    // an ERROR token's lexeme.size() as a source span. For an ERROR token
    // offset marks the scanner position where the error was found.
    std::size_t offset = 0;
};

std::ostream& operator<<(std::ostream& os, const TokenType& type);