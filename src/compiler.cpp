#include "compiler.h"

#include <memory>

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

void Compiler::endCompiler() { emitReturn(); }

void Compiler::expression() {
    // TODO: Implement actual expression parsing & code generation
    auto constant = m_currentChunk->addConstant(Value(42.0));
    emitBytes(Op::CONSTANT, constant);
}

void Compiler::emitByte(Byte byte) {
    m_currentChunk->write(byte, m_parser->m_previous.line);
}

void Compiler::emitBytes(Op op, Byte byte) {
    emitByte(static_cast<Byte>(op));
    emitByte(byte);
}

void Compiler::emitReturn() { emitByte(static_cast<Byte>(Op::RETURN)); }
