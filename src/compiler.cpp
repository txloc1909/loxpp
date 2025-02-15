#include "compiler.h"

#include <memory>

#define DEBUG_PRINT_CODE

std::unique_ptr<Chunk> compile(const std::string& source) {
    auto chunk = std::make_unique<Chunk>();
    auto parser = std::make_unique<Parser>(source);
    auto compiler = std::make_unique<Compiler>(chunk.get(), parser.get());

    compiler->expression();
    parser->consume(TokenType::EOF_, "Expect end of expression.");
    compiler->endCompiler();
    return chunk;
}

Compiler::Compiler(Chunk* chunk, Parser* parser)
    : m_currentChunk{chunk}, m_parser{parser} {}

void Compiler::endCompiler() {
    emitReturn();
#ifdef DEBUG_PRINT_CODE
    m_currentChunk->disassemble("code");
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
        emitBytes(Op::NEGATE, 0);
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

void Compiler::number() {
    Value value = std::stod(m_parser->m_previous.lexeme.data(), nullptr);
    emitBytes(Op::CONSTANT, makeConstant(value));
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
    uint8_t constant = m_currentChunk->addConstant(value);

    // ValueArray already hit illegal memory access at this point
    // TODO: Handle this gracefully
    if (constant >= UINT8_MAX) {
        m_parser->error("Too many constants in one chunk.");
        return 0;
    }

    return constant;
}
