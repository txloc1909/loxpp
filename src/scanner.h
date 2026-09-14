#pragma once

#include "token.h"

// Null-terminated array of all Lox keywords, kept here as the single source
// of truth (used by the scanner and the REPL tab-completion).
const char* const* lox_keywords();

class Scanner {
  public:
    Scanner(const std::string& source);
    Scanner(const char* source);
    Token scanOneToken();

    // The start of the buffer this scanner was constructed over. Combined
    // with a Token's own `offset` (always the true source span, even for
    // STRING/ERROR whose `lexeme` is not verbatim source text — see
    // token.h), this recovers a `const char*` into the original source at
    // that token's position, for a caller that wants to start a second,
    // independent Scanner from there (compiler.cpp's defer look-ahead).
    [[nodiscard]] const char* sourceBegin() const { return m_source_begin; }

  private:
    Token makeToken(TokenType type);
    Token makeToken(TokenType type, std::string_view lexeme);
    Token createErrorToken(const char* message);

    TokenType checkKeyword(std::size_t start, std::size_t length,
                           const char* rest, TokenType type);
    TokenType identifierType();
    Token consumeNumber();
    Token consumeString();
    Token consumeIdentifier();
    void skipWhitespaceAndComments();

    [[nodiscard]] bool isAtEnd() const;
    char advance();
    [[nodiscard]] char peek() const;
    [[nodiscard]] char peekNext() const;
    bool match(char expected);

    const char* m_source_begin;
    const char* m_current;
    const char* m_start;
    std::size_t m_line;
};