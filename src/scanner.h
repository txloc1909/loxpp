#pragma once

#include "token.h"

class Scanner {
  public:
    Scanner(const std::string& source);
    Token scanOneToken();

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

    bool isAtEnd() const;
    char advance();
    char peek() const;
    char peekNext() const;
    bool match(char expected);

    const std::string& m_source;
    const char* m_current;
    const char* m_start;
    std::size_t m_line;
};