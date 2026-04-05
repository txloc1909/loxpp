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
    // Parse-time context: set by parsePrecedence before invoking a prefix
    // parselet so the parselet knows whether '=' is a valid assignment.
    // Lives here (not on Compiler) because it is parse context — the parser
    // derives it from the calling precedence. A different compilation backend
    // can read this field without any change to the parser.
    //
    // Note: canAssign is NOT a per-rule property of ParseRule. A ParseRule
    // (parselet) does not statically know if it is an l-value; that depends on
    // the calling context (whether precedence <= ASSIGNMENT). Adding it to
    // ParseRule would conflate static rule metadata with dynamic call context.
    bool m_canAssign{false};

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
