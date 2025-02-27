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
