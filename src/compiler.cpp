#include "compiler.h"
#include "debug.h"

#include <iostream>
#include <memory>
#include <unistd.h>

std::unique_ptr<Chunk> compile(const std::string& source, Allocator* alloc) {
    auto chunk = std::make_unique<Chunk>();
    auto parser = std::make_unique<Parser>(source);
    auto compiler =
        std::make_unique<Compiler>(chunk.get(), parser.get(), alloc);

    while (!parser->check(TokenType::EOF_))
        compiler->declaration();

    compiler->endCompiler();

    if (parser->m_hadError)
        return nullptr;
    return chunk;
}

Compiler::Compiler(Chunk* chunk, Parser* parser, Allocator* alloc)
    : m_currentChunk{chunk}, m_parser{parser}, m_allocator{alloc} {}

void Compiler::endCompiler() {
    emitReturn();
#ifdef LOXPP_DEBUG_PRINT_CODE
    bool color = isatty(STDOUT_FILENO) != 0;
    disassembleChunk(*m_currentChunk, *m_allocator, "code", std::cout, color);
#endif
}

void Compiler::expression() {
    m_parser->parsePrecedence(Precedence::ASSIGNMENT, this);
}

void Compiler::grouping() {
    expression();
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after expression.");
}

void Compiler::unary() {
    TokenType operatorType = m_parser->m_previous.type;

    m_parser->parsePrecedence(Precedence::UNARY, this);

    switch (operatorType) {
    case TokenType::MINUS:
        emitByte(Op::NEGATE);
        break;
    case TokenType::BANG:
        emitByte(Op::NOT);
        break;
    default:
        return; // Unreachable.
    }
}

void Compiler::binary() {
    TokenType operatorType = m_parser->m_previous.type;

    Precedence current = Parser::getRule(operatorType)->precedence;
    m_parser->parsePrecedence(nextPrecedence(current), this);

    switch (operatorType) {
    case TokenType::BANG_EQUAL: {
        emitByte(Op::EQUAL);
        emitByte(Op::NOT);
        break;
    }
    case TokenType::EQUAL_EQUAL: {
        emitByte(Op::EQUAL);
        break;
    }
    case TokenType::GREATER: {
        emitByte(Op::GREATER);
        break;
    }
    case TokenType::GREATER_EQUAL: {
        emitByte(Op::LESS);
        emitByte(Op::NOT);
        break;
    }
    case TokenType::LESS: {
        emitByte(Op::LESS);
        break;
    }
    case TokenType::LESS_EQUAL: {
        emitByte(Op::GREATER);
        emitByte(Op::NOT);
        break;
    }
    case TokenType::PLUS:
        emitByte(Op::ADD);
        break;
    case TokenType::MINUS:
        emitByte(Op::SUBTRACT);
        break;
    case TokenType::STAR:
        emitByte(Op::MULTIPLY);
        break;
    case TokenType::SLASH:
        emitByte(Op::DIVIDE);
        break;
    default:
        return; // Unreachable.
    }
}

void Compiler::literal() {
    switch (m_parser->m_previous.type) {
    case TokenType::FALSE:
        emitByte(Op::FALSE);
        break;
    case TokenType::NIL:
        emitByte(Op::NIL);
        break;
    case TokenType::TRUE:
        emitByte(Op::TRUE);
        break;
    default:
        return; // unreachable
    }
}

void Compiler::number() {
    double num = std::stod(m_parser->m_previous.lexeme.data(), nullptr);
    emitBytes(Op::CONSTANT, makeConstant(from<Number>(num)));
}

void Compiler::string() {
    ObjHandle handle = m_allocator->makeString(m_parser->m_previous.lexeme);
    emitBytes(Op::CONSTANT, makeConstant(Value{handle}));
}

void Compiler::variable() {
    namedVariable(m_parser->m_previous, m_parser->m_canAssign);
}

void Compiler::declaration() {
    if (m_parser->match(TokenType::VAR)) {
        varDeclaration();
    } else {
        statement();
    }
    if (m_parser->m_panicMode)
        m_parser->synchronize();
}

void Compiler::varDeclaration() {
    // Read the name from m_current, then consume the IDENTIFIER token.
    uint8_t global = identifierConstant(m_parser->m_current);
    m_parser->consume(TokenType::IDENTIFIER, "Expect variable name.");

    if (m_parser->match(TokenType::EQUAL)) {
        expression();
    } else {
        emitByte(Op::NIL);
    }
    m_parser->consume(TokenType::SEMICOLON,
                      "Expect ';' after variable declaration.");
    emitBytes(Op::DEFINE_GLOBAL, global);
}

void Compiler::statement() {
    if (m_parser->match(TokenType::PRINT)) {
        printStatement();
    } else {
        expressionStatement();
    }
}

void Compiler::printStatement() {
    expression();
    m_parser->consume(TokenType::SEMICOLON, "Expect ';' after value.");
    emitByte(Op::PRINT);
}

void Compiler::expressionStatement() {
    expression();
    m_parser->consume(TokenType::SEMICOLON, "Expect ';' after expression.");
    emitByte(Op::POP);
}

void Compiler::emitByte(Byte byte) {
    m_currentChunk->write(byte, m_parser->m_previous.line);
}

void Compiler::emitByte(Op op) { emitByte(static_cast<Byte>(op)); }

void Compiler::emitBytes(Op op, Byte byte) {
    emitByte(op);
    emitByte(byte);
}

void Compiler::emitReturn() { emitByte(static_cast<Byte>(Op::RETURN)); }

uint8_t Compiler::makeConstant(Value value) {
    auto constant = m_currentChunk->addConstant(value);
    if (!constant) {
        m_parser->error("Too many constants in one chunk.");
        return 0;
    }
    return *constant;
}

uint8_t Compiler::identifierConstant(const Token& name) {
    ObjHandle handle = m_allocator->makeString(name.lexeme);
    return makeConstant(Value{handle});
}

void Compiler::namedVariable(const Token& name, bool canAssign) {
    uint8_t nameConstant = identifierConstant(name);
    if (canAssign && m_parser->match(TokenType::EQUAL)) {
        expression();
        emitBytes(Op::SET_GLOBAL, nameConstant);
    } else {
        emitBytes(Op::GET_GLOBAL, nameConstant);
    }
}
