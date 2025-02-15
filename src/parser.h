#pragma once

#include "scanner.h"

#include <unordered_map>

enum class Precedence {
    NONE,
    ASSIGNMENT, // =
    OR,         // ||
    AND,        // &&
    EQUALITY,   // == !=
    COMPARISON, // < > <= >=
    TERM,       // + -
    FACTOR,     // * /
    UNARY,      // - +
    CALL,       // . ()
    PRIMARY,    // literals, variables, numbers, strings
};

class Compiler;
using ParseFn = void (*)(Compiler&);

struct ParseRule {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
};

struct Parser {
    Scanner m_scanner;
    Token m_previous;
    Token m_current;
    bool m_hadError;
    bool m_panicMode;

    Parser(const std::string& source);

    void errorAt(const Token& token, const char* message);
    void errorAtCurrent(const char* message);
    void error(const char* message);

    void advance();
    void consume(TokenType type, const char* message);
    bool check(TokenType type) const;
    bool match(TokenType type);
    void synchronize();
};
