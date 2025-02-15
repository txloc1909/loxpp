#include "parser.h"
#include "compiler.h"

#define T(token) static_cast<int>(TokenType::token)
#define RULE(token, prefix, infix, prec)                                       \
    [T(token)] = {prefix, infix, Precedence::prec}

// clang-format off
static const ParseRule rules[] = {
    // TokenType         Prefix              Infix              Precedence
    RULE(LEFT_PAREN,     &Compiler::grouping, nullptr,           CALL),
    RULE(RIGHT_PAREN,    nullptr,             nullptr,           NONE),
    RULE(LEFT_BRACE,     nullptr,             nullptr,           NONE),
    RULE(RIGHT_BRACE,    nullptr,             nullptr,           NONE),
    RULE(COMMA,          nullptr,             nullptr,           NONE),
    RULE(DOT,            nullptr,             nullptr,           NONE),
    RULE(MINUS,          &Compiler::unary,    &Compiler::binary, TERM),
    RULE(PLUS,           nullptr,             &Compiler::binary, TERM),
    RULE(SEMICOLON,      nullptr,             nullptr,           NONE),
    RULE(SLASH,          nullptr,             &Compiler::binary, FACTOR),
    RULE(STAR,           nullptr,             &Compiler::binary, FACTOR),
    RULE(BANG,           &Compiler::unary,    nullptr,           NONE),
    RULE(BANG_EQUAL,     nullptr,             &Compiler::binary, NONE),
    RULE(EQUAL,          nullptr,             nullptr,           NONE),
    RULE(EQUAL_EQUAL,    nullptr,             nullptr,           NONE),
    RULE(GREATER,        nullptr,             nullptr,           NONE),
    RULE(GREATER_EQUAL,  nullptr,             nullptr,           NONE),
    RULE(LESS,           nullptr,             nullptr,           NONE),
    RULE(LESS_EQUAL,     nullptr,             nullptr,           NONE),
    RULE(IDENTIFIER,     nullptr,             nullptr,           NONE),
    RULE(STRING,         nullptr,             nullptr,           NONE),
    RULE(NUMBER,         &Compiler::number,   nullptr,           NONE),
    RULE(AND,            nullptr,             nullptr,           NONE),
    RULE(CLASS,          nullptr,             nullptr,           NONE),
    RULE(ELSE,           nullptr,             nullptr,           NONE),
    RULE(FALSE,          nullptr,             nullptr,           NONE),
    RULE(FOR,            nullptr,             nullptr,           NONE),
    RULE(FUN,            nullptr,             nullptr,           NONE),
    RULE(IF,             nullptr,             nullptr,           NONE),
    RULE(NIL,            nullptr,             nullptr,           NONE),
    RULE(OR,             nullptr,             nullptr,           NONE),
    RULE(PRINT,          nullptr,             nullptr,           NONE),
    RULE(RETURN,         nullptr,             nullptr,           NONE),
    RULE(SUPER,          nullptr,             nullptr,           NONE),
    RULE(THIS,           nullptr,             nullptr,           NONE),
    RULE(TRUE,           nullptr,             nullptr,           NONE),
    RULE(VAR,            nullptr,             nullptr,           NONE),
    RULE(WHILE,          nullptr,             nullptr,           NONE),
    RULE(EOF_,           nullptr,             nullptr,           NONE),
    RULE(ERROR,          nullptr,             nullptr,           NONE),
};
// clang-format on

#undef RULE
#undef T

Parser::Parser(const std::string& source) : m_scanner(source) {
    m_current = m_scanner.scanOneToken();
    m_previous = m_current;
    m_hadError = false;
    m_panicMode = false;
}

const ParseRule* Parser::getRule(TokenType type) {
    return &rules[static_cast<int>(type)];
}

void Parser::parsePrecedence(Precedence precedence, Compiler* compiler) {
    advance();
    auto prefixRule = getRule(m_previous.type)->prefix;
    if (prefixRule == nullptr) {
        errorAtCurrent("Expect expression.");
        return;
    }

    (compiler->*prefixRule)();

    while (precedence <= getRule(m_current.type)->precedence) {
        advance();
        auto infixRule = getRule(m_previous.type)->infix;
        (compiler->*infixRule)();
    }
}

void Parser::errorAt(const Token& token, const char* message) {
    if (m_panicMode)
        return;
    m_panicMode = true;
    std::fprintf(stderr, "[line %zu] Error", token.line);
    if (token.type == TokenType::EOF_) {
        std::fprintf(stderr, " at end");
    } else if (token.type == TokenType::ERROR) {
        // Do nothing
    } else {
        std::fprintf(stderr, " at '%.*s'",
                     static_cast<int>(token.lexeme.length()),
                     token.lexeme.data());
    }
    std::fprintf(stderr, ": %s\n", message);
    m_hadError = true;
}

void Parser::errorAtCurrent(const char* message) {
    errorAt(m_current, message);
}

void Parser::error(const char* message) { errorAt(m_previous, message); }

void Parser::advance() {
    m_previous = m_current;
    for (;;) {
        m_current = m_scanner.scanOneToken();
        if (m_current.type != TokenType::ERROR)
            break;

        errorAtCurrent(m_current.lexeme.data());
    }
}

void Parser::consume(TokenType type, const char* message) {
    if (m_current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

bool Parser::check(TokenType type) const { return m_current.type == type; }

bool Parser::match(TokenType type) {
    if (!check(type))
        return false;
    advance();
    return true;
}

void Parser::synchronize() {
    m_panicMode = false;

    while (m_current.type != TokenType::EOF_) {
        if (m_previous.type == TokenType::SEMICOLON)
            return;

        switch (m_current.type) {
        case TokenType::CLASS:
        case TokenType::FUN:
        case TokenType::VAR:
        case TokenType::FOR:
        case TokenType::IF:
        case TokenType::WHILE:
        case TokenType::PRINT:
        case TokenType::RETURN:
            return;
        default:; // do nothing
        }
        advance();
    }
}