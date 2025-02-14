#include "scanner.h"

#include <cstring>

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isAlphaNumeric(char c) { return isDigit(c) || isAlpha(c); }

Scanner::Scanner(const std::string& source)
    : m_source(&source), m_current(source.data()), m_start(source.data()),
      m_line(1) {}

Scanner::Scanner(const char* source)
    : m_source(nullptr), // we don't need m_source for char* constructor
      m_current(source), m_start(source), m_line(1) {}

Token Scanner::scanOneToken() {
    skipWhitespaceAndComments();
    m_start = m_current;

    if (isAtEnd())
        return makeToken(TokenType::EOF_);

    char c = advance();
    if (isAlpha(c))
        return consumeIdentifier();
    if (isDigit(c))
        return consumeNumber();
    switch (c) {
    case '(':
        return makeToken(TokenType::LEFT_PAREN);
    case ')':
        return makeToken(TokenType::RIGHT_PAREN);
    case '{':
        return makeToken(TokenType::LEFT_BRACE);
    case '}':
        return makeToken(TokenType::RIGHT_BRACE);
    case ',':
        return makeToken(TokenType::COMMA);
    case '.':
        return makeToken(TokenType::DOT);
    case '-':
        return makeToken(TokenType::MINUS);
    case '+':
        return makeToken(TokenType::PLUS);
    case ';':
        return makeToken(TokenType::SEMICOLON);
    case '*':
        return makeToken(TokenType::STAR);
    case '/':
        return makeToken(TokenType::SLASH);
    case '!':
        return makeToken(match('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
    case '=':
        return makeToken(match('=') ? TokenType::EQUAL_EQUAL
                                    : TokenType::EQUAL);
    case '>':
        return makeToken(match('=') ? TokenType::GREATER_EQUAL
                                    : TokenType::GREATER);
    case '<':
        return makeToken(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
    case '"':
        return consumeString();
    }

    return createErrorToken("Unexpected character.");
}

Token Scanner::makeToken(TokenType type) {
    return Token{type,
                 std::string_view(
                     m_start, static_cast<std::size_t>(m_current - m_start)),
                 m_line};
}

Token Scanner::makeToken(TokenType type, std::string_view lexeme) {
    return Token{type, lexeme, m_line};
}

Token Scanner::createErrorToken(const char* message) {
    return Token{TokenType::ERROR, message, m_line};
}

TokenType Scanner::checkKeyword(std::size_t start, std::size_t length,
                                const char* rest, TokenType type) {
    if (m_current - m_start == start + length &&
        std::memcmp(m_start + start, rest, length) == 0) {
        return type;
    }
    return TokenType::IDENTIFIER;
}

TokenType Scanner::identifierType() {
    switch (m_start[0]) {
    case 'a':
        return checkKeyword(1, 2, "nd", TokenType::AND);
    case 'c':
        return checkKeyword(1, 4, "lass", TokenType::CLASS);
    case 'e':
        return checkKeyword(1, 3, "lse", TokenType::ELSE);
    case 'i':
        return checkKeyword(1, 1, "f", TokenType::IF);
    case 'f': {
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
            case 'a':
                return checkKeyword(2, 3, "lse", TokenType::FALSE);
            case 'o':
                return checkKeyword(2, 1, "r", TokenType::FOR);
            case 'u':
                return checkKeyword(2, 1, "n", TokenType::FUN);
            }
        }
        break;
    }
    case 'n':
        return checkKeyword(1, 2, "il", TokenType::NIL);
    case 'o':
        return checkKeyword(1, 1, "r", TokenType::OR);
    case 'p':
        return checkKeyword(1, 4, "rint", TokenType::PRINT);
    case 'r':
        return checkKeyword(1, 5, "eturn", TokenType::RETURN);
    case 's':
        return checkKeyword(1, 4, "uper", TokenType::SUPER);
    case 't': {
        if (m_current - m_start > 1) {
            switch (m_start[1]) {
            case 'h':
                return checkKeyword(2, 2, "is", TokenType::THIS);
            case 'r':
                return checkKeyword(2, 2, "ue", TokenType::TRUE);
            }
        }
        break;
    }
    case 'v':
        return checkKeyword(1, 2, "ar", TokenType::VAR);
    case 'w':
        return checkKeyword(1, 4, "hile", TokenType::WHILE);
    }
    return TokenType::IDENTIFIER;
}

Token Scanner::consumeNumber() {
    while (isDigit(peek()))
        advance();

    // Look for a fractional part.
    if (peek() == '.' && isDigit(peekNext())) {
        //   Consume the '.'.
        advance();
        while (isDigit(peek()))
            advance();
    }
    return makeToken(TokenType::NUMBER);
}

Token Scanner::consumeString() {
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n')
            m_line++;
        advance();
    }

    if (isAtEnd())
        return createErrorToken("Unterminated string.");

    // The closing quote.
    advance();
    auto lexeme = std::string_view{
        m_start + 1, static_cast<std::size_t>(m_current - m_start) - 2};
    return makeToken(TokenType::STRING, lexeme);
}

Token Scanner::consumeIdentifier() {
    while (isAlphaNumeric(peek()))
        advance();
    return makeToken(identifierType());
}

void Scanner::skipWhitespaceAndComments() {
    for (;;) {
        char c = peek();
        switch (c) {
        case ' ':
        case '\r':
        case '\t':
            advance();
            break;
        case '\n':
            m_line++;
            advance();
            break;
        case '/': // handle comment
            if (peekNext() == '/') {
                while (peek() != '\n' && !isAtEnd())
                    advance();
            } else {
                return;
            }
            break;
        default:
            return;
        }
    }
}

bool Scanner::isAtEnd() const { return *m_current == '\0'; }

char Scanner::advance() { return *m_current++; }

char Scanner::peek() const { return *m_current; }

char Scanner::peekNext() const { return *(m_current + 1); }

bool Scanner::match(char expected) {
    if (isAtEnd())
        return false;
    if (*m_current != expected)
        return false;
    m_current++;
    return true;
}