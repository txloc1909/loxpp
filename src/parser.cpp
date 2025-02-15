#include "parser.h"

Parser::Parser(const std::string& source) : m_scanner(source) {
    m_current = m_scanner.scanOneToken();
    m_previous = m_current;
    m_hadError = false;
    m_panicMode = false;
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
