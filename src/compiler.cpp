#include "compiler.h"
#include "debug.h"
#include "memory_manager.h"

#include <iostream>
#include <memory>
#include <unistd.h>

std::unique_ptr<Chunk> compile(const std::string& source, MemoryManager* mm) {
    auto chunk = std::make_unique<Chunk>();
    auto parser = std::make_unique<Parser>(source);
    auto compiler = std::make_unique<Compiler>(chunk.get(), parser.get(), mm);

    while (!parser->check(TokenType::EOF_))
        compiler->declaration();

    compiler->endCompiler();

    if (parser->m_hadError)
        return nullptr;
    return chunk;
}

Compiler::Compiler(Chunk* chunk, Parser* parser, MemoryManager* mm)
    : m_currentChunk{chunk}, m_parser{parser}, m_mm{mm} {}

void Compiler::endCompiler() {
    emitReturn();
#ifdef LOXPP_DEBUG_PRINT_CODE
    bool color = isatty(STDOUT_FILENO) != 0;
    disassembleChunk(*m_currentChunk, *m_mm, "code", std::cout, color);
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
    ObjString* s = m_mm->makeString(m_parser->m_previous.lexeme);
    emitBytes(Op::CONSTANT, makeConstant(Value{static_cast<Obj*>(s)}));
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
    m_parser->consume(TokenType::IDENTIFIER, "Expect variable name.");
    Token name = m_parser->m_previous;
    declareVariable();

    if (m_parser->match(TokenType::EQUAL)) {
        expression();
    } else {
        emitByte(Op::NIL);
    }
    m_parser->consume(TokenType::SEMICOLON,
                      "Expect ';' after variable declaration.");

    if (m_scopeDepth > 0) {
        markInitialized();
    } else {
        uint8_t nameConst = identifierConstant(name);
        emitBytes(Op::DEFINE_GLOBAL, nameConst);
    }
}

void Compiler::block() {
    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        declaration();
    }
    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after block.");
}

void Compiler::beginScope() { m_scopeDepth++; }

void Compiler::endScope() {
    m_scopeDepth--;
    while (m_localCount > 0 &&
           m_locals[m_localCount - 1].depth > m_scopeDepth) {
        emitByte(Op::POP);
        m_localCount--;
    }
}

void Compiler::addLocal(const Token& name) {
    if (m_localCount == UINT8_COUNT) {
        m_parser->error("Too many local variables in function.");
        return;
    }
    m_locals[m_localCount++] = Local{name, -1};
}

int Compiler::resolveLocal(const Token& name) const {
    for (int i = m_localCount - 1; i >= 0; i--) {
        const Local& local = m_locals[i];
        if (local.name.lexeme == name.lexeme) {
            if (local.depth == -1)
                m_parser->error(
                    "Can't read local variable in its own initializer.");
            return i;
        }
    }
    return -1;
}

void Compiler::declareVariable() {
    if (m_scopeDepth == 0)
        return;

    const Token& name = m_parser->m_previous;
    for (int i = m_localCount - 1; i >= 0; i--) {
        const Local& local = m_locals[i];
        if (local.depth != -1 && local.depth < m_scopeDepth)
            break;
        if (local.name.lexeme == name.lexeme)
            m_parser->error("Already a variable with this name in this scope.");
    }
    addLocal(name);
}

void Compiler::markInitialized() {
    if (m_scopeDepth == 0)
        return;
    m_locals[m_localCount - 1].depth = m_scopeDepth;
}

void Compiler::statement() {
    if (m_parser->match(TokenType::LEFT_BRACE)) {
        beginScope();
        block();
        endScope();
    } else if (m_parser->match(TokenType::PRINT)) {
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
    ObjString* s = m_mm->makeString(name.lexeme);
    return makeConstant(Value{static_cast<Obj*>(s)});
}

void Compiler::namedVariable(const Token& name, bool canAssign) {
    Op getOp, setOp;
    int slot = resolveLocal(name);
    uint8_t operand;

    if (slot != -1) {
        getOp = Op::GET_LOCAL;
        setOp = Op::SET_LOCAL;
        operand = static_cast<uint8_t>(slot);
    } else {
        getOp = Op::GET_GLOBAL;
        setOp = Op::SET_GLOBAL;
        operand = identifierConstant(name);
    }

    if (canAssign && m_parser->match(TokenType::EQUAL)) {
        expression();
        emitBytes(setOp, operand);
    } else {
        emitBytes(getOp, operand);
    }
}
