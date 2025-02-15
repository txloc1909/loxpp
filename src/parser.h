#pragma once

#include "scanner.h"

enum class Precedence : int {
    NONE = 0,
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

inline Precedence nextPrecedence(Precedence current) {
    return static_cast<Precedence>(static_cast<int>(current) + 1);
}

inline bool operator<=(Precedence a, Precedence b) {
    return static_cast<int>(a) <= static_cast<int>(b);
}

class Compiler;
using ParseFn = void (Compiler::*)();

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

    void parsePrecedence(Precedence precedence, Compiler* compiler);

    void errorAt(const Token& token, const char* message);
    void errorAtCurrent(const char* message);
    void error(const char* message);

    void advance();
    void consume(TokenType type, const char* message);
    bool check(TokenType type) const;
    bool match(TokenType type);
    void synchronize();

    static const ParseRule* getRule(TokenType type);
};
