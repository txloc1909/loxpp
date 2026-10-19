#pragma once

#include "scanner.h"

#include <cstdint>

enum class Precedence : std::uint8_t {
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
class DiagnosticSink;
using ParseFn = void (Compiler::*)();

struct ParseRule {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
    // A terminal prefix rule consumes a complete expression at its precedence
    // level: no infix operator may follow it. `yield` is the only such rule.
    // It sits at the assignment level (the lowest precedence) and cannot be an
    // operand of any operator.
    bool terminal{false};
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
    // Each precedence level saves the enclosing value and restores it when the
    // level ends. A nested expression (an argument, an index, a list element)
    // therefore cannot change the enclosing level's decision about '='.
    //
    // A parselet whose value is never an assignment target — an operator
    // expression — clears the flag before its level ends. The '=' is then left
    // for the caller, which reports the missing ';'.
    //
    // Note: canAssign is NOT a per-rule property of ParseRule. A ParseRule
    // (parselet) does not statically know if it is an l-value; that depends on
    // the calling context (whether precedence <= ASSIGNMENT). Adding it to
    // ParseRule would conflate static rule metadata with dynamic call context.
    bool m_canAssign{false};

    // True when parsePrecedence was entered at ASSIGNMENT precedence, the only
    // level at which a `yield` expression is valid. It is saved and restored
    // with m_canAssign. It records the precedence context itself, so a prefix
    // rule can tell an assignment-level call from an operand-position call.
    bool m_atAssignmentLevel{false};

    // When set, errorAt collects diagnostics here and prints nothing. Null (the
    // default) keeps the original stderr behaviour byte-for-byte. compile()
    // wires it; vm.cpp leaves it null.
    DiagnosticSink* m_sink{nullptr};

    Parser(const std::string& source);

    void parsePrecedence(Precedence precedence, Compiler* compiler);

    void errorAt(const Token& token, const char* message);
    void errorAtCurrent(const char* message);
    void error(const char* message);

    void advance();
    void consume(TokenType type, const char* message);
    [[nodiscard]] bool check(TokenType type) const;
    bool match(TokenType type);
    void synchronize();

    static const ParseRule* getRule(TokenType type);
};
