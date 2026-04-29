#pragma once

#include <string_view>
#include <string>

enum class TokenType : int {
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
    MINUS,
    PLUS,
    SEMICOLON,
    SLASH,
    STAR,
    PERCENT,

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
};

std::ostream& operator<<(std::ostream& os, const TokenType& type);