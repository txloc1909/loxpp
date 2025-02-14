#include "token.h"

#include <iostream>

// for debug print
std::ostream& operator<<(std::ostream& os, const TokenType& type) {
    switch (type) {
    case TokenType::LEFT_PAREN:
        return os << "LEFT_PAREN";
    case TokenType::RIGHT_PAREN:
        return os << "RIGHT_PAREN";
    case TokenType::LEFT_BRACE:
        return os << "LEFT_BRACE";
    case TokenType::RIGHT_BRACE:
        return os << "RIGHT_BRACE";
    case TokenType::COMMA:
        return os << "COMMA";
    case TokenType::DOT:
        return os << "DOT";
    case TokenType::MINUS:
        return os << "MINUS";
    case TokenType::PLUS:
        return os << "PLUS";
    case TokenType::SEMICOLON:
        return os << "SEMICOLON";
    case TokenType::STAR:
        return os << "STAR";
    case TokenType::BANG:
        return os << "BANG";
    case TokenType::BANG_EQUAL:
        return os << "BANG_EQUAL";
    case TokenType::EQUAL:
        return os << "EQUAL";
    case TokenType::EQUAL_EQUAL:
        return os << "EQUAL_EQUAL";
    case TokenType::GREATER:
        return os << "GREATER";
    case TokenType::GREATER_EQUAL:
        return os << "GREATER_EQUAL";
    case TokenType::LESS:
        return os << "LESS";
    case TokenType::LESS_EQUAL:
        return os << "LESS_EQUAL";
    case TokenType::IDENTIFIER:
        return os << "IDENTIFIER";
    case TokenType::STRING:
        return os << "STRING";
    case TokenType::NUMBER:
        return os << "NUMBER";
    case TokenType::AND:
        return os << "AND";
    case TokenType::CLASS:
        return os << "CLASS";
    case TokenType::ELSE:
        return os << "ELSE";
    case TokenType::FALSE:
        return os << "FALSE";
    case TokenType::FUN:
        return os << "FUN";
    case TokenType::FOR:
        return os << "FOR";
    case TokenType::IF:
        return os << "IF";
    case TokenType::NIL:
        return os << "NIL";
    case TokenType::OR:
        return os << "OR";
    case TokenType::PRINT:
        return os << "PRINT";
    case TokenType::RETURN:
        return os << "RETURN";
    case TokenType::SUPER:
        return os << "SUPER";
    case TokenType::THIS:
        return os << "THIS";
    case TokenType::TRUE:
        return os << "TRUE";
    case TokenType::VAR:
        return os << "VAR";
    case TokenType::WHILE:
        return os << "WHILE";
    case TokenType::EOF_:
        return os << "EOF";
    default:
        return os << "UNKNOWN";
    }
}