#include "compiler.h"
#include "debug.h"
#include "memory_manager.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <unistd.h>

ObjFunction* compile(const std::string& source, MemoryManager* mm) {
    ObjFunction* fn = mm->create<ObjFunction>();
    auto parser = std::make_unique<Parser>(source);
    auto compiler = std::make_unique<Compiler>(fn, parser.get(), mm);

    while (!parser->check(TokenType::EOF_))
        compiler->declaration();

    compiler->endCompiler();

    if (parser->m_hadError)
        return nullptr;
    return fn;
}

Compiler::Compiler(ObjFunction* function, Parser* parser, MemoryManager* mm)
    : m_function{function}, m_parser{parser}, m_mm{mm} {
    // Reserve slot 0 for the function/script itself. User-declared locals
    // start at slot 1. The implicit local has an empty name so resolveLocal
    // never accidentally matches it.
    Local* implicit = &m_locals[m_localCount++];
    implicit->depth = 0;
    implicit->name = Token{TokenType::IDENTIFIER, "", 0};
}

void Compiler::endCompiler() {
    emitReturn();
#ifdef LOXPP_DEBUG_PRINT_CODE
    bool color = isatty(STDOUT_FILENO) != 0;
    const char* name =
        m_function->name ? m_function->name->chars.c_str() : "script";
    disassembleChunk(*getCurrentChunk(), *m_mm, name, std::cout, color);
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

void Compiler::call() {
    // The callee is already on the stack. Parse the argument list.
    uint8_t argCount = 0;
    if (!m_parser->check(TokenType::RIGHT_PAREN)) {
        do {
            if (argCount == 255) {
                m_parser->error("Can't have more than 255 arguments.");
            }
            expression();
            argCount++;
        } while (m_parser->match(TokenType::COMMA));
    }
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after arguments.");
    emitBytes(Op::CALL, argCount);
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
    } else if (m_parser->match(TokenType::IF)) {
        ifStatement();
    } else if (m_parser->match(TokenType::WHILE)) {
        whileStatement();
    } else if (m_parser->match(TokenType::FOR)) {
        forStatement();
    } else if (m_parser->match(TokenType::BREAK)) {
        breakStatement();
    } else if (m_parser->match(TokenType::CONTINUE)) {
        continueStatement();
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

void Compiler::ifStatement() {
    m_parser->consume(TokenType::LEFT_PAREN, "Expect '(' after 'if'.");
    expression();
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");

    int thenJump = emitJump(Op::JUMP_IF_FALSE);
    emitByte(Op::POP);
    statement();

    int elseJump = emitJump(Op::JUMP);
    patchJump(thenJump);
    emitByte(Op::POP);

    if (m_parser->match(TokenType::ELSE))
        statement();
    patchJump(elseJump);
}

void Compiler::whileStatement() {
    int loopStart = static_cast<int>(getCurrentChunk()->size());
    m_loopStack.push_back({loopStart, m_localCount, {}});

    m_parser->consume(TokenType::LEFT_PAREN, "Expect '(' after 'while'.");
    expression();
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");

    int exitJump = emitJump(Op::JUMP_IF_FALSE);
    emitByte(Op::POP);
    statement();
    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(Op::POP);

    for (int offset : m_loopStack.back().breakJumps)
        patchJump(offset);
    m_loopStack.pop_back();
}

void Compiler::forStatement() {
    beginScope();

    m_parser->consume(TokenType::LEFT_PAREN, "Expect '(' after 'for'.");

    if (m_parser->match(TokenType::SEMICOLON)) {
        // no initializer
    } else if (m_parser->match(TokenType::VAR)) {
        varDeclaration();
    } else {
        expressionStatement();
    }

    int loopStart = static_cast<int>(getCurrentChunk()->size());
    m_loopStack.push_back({loopStart, m_localCount, {}});

    int exitJump = -1;
    if (!m_parser->match(TokenType::SEMICOLON)) {
        expression();
        m_parser->consume(TokenType::SEMICOLON,
                          "Expect ';' after loop condition.");
        exitJump = emitJump(Op::JUMP_IF_FALSE);
        emitByte(Op::POP);
    }

    if (!m_parser->match(TokenType::RIGHT_PAREN)) {
        int bodyJump = emitJump(Op::JUMP);
        int incrStart = static_cast<int>(getCurrentChunk()->size());
        expression();
        emitByte(Op::POP);
        m_parser->consume(TokenType::RIGHT_PAREN,
                          "Expect ')' after for clauses.");
        emitLoop(loopStart);
        loopStart = incrStart;
        patchJump(bodyJump);
        m_loopStack.back().start = incrStart;
    }

    statement();
    emitLoop(loopStart);

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(Op::POP);
    }

    for (int offset : m_loopStack.back().breakJumps)
        patchJump(offset);
    m_loopStack.pop_back();

    endScope();
}

void Compiler::breakStatement() {
    if (m_loopStack.empty()) {
        m_parser->error("Cannot use 'break' outside a loop.");
        return;
    }
    m_parser->consume(TokenType::SEMICOLON, "Expect ';' after 'break'.");
    emitLoopCleanup();
    int jump = emitJump(Op::JUMP);
    m_loopStack.back().breakJumps.push_back(jump);
}

void Compiler::continueStatement() {
    if (m_loopStack.empty()) {
        m_parser->error("Cannot use 'continue' outside a loop.");
        return;
    }
    m_parser->consume(TokenType::SEMICOLON, "Expect ';' after 'continue'.");
    emitLoopCleanup();
    emitLoop(m_loopStack.back().start);
}

void Compiler::and_() {
    int endJump = emitJump(Op::JUMP_IF_FALSE);
    emitByte(Op::POP);
    m_parser->parsePrecedence(Precedence::AND, this);
    patchJump(endJump);
}

void Compiler::or_() {
    int elseJump = emitJump(Op::JUMP_IF_FALSE);
    int endJump = emitJump(Op::JUMP);
    patchJump(elseJump);
    emitByte(Op::POP);
    m_parser->parsePrecedence(Precedence::OR, this);
    patchJump(endJump);
}

void Compiler::emitReturn() {
    emitByte(Op::NIL);
    emitByte(static_cast<Byte>(Op::RETURN));
}

void Compiler::emitByte(Byte byte) {
    getCurrentChunk()->write(byte, m_parser->m_previous.line);
}

void Compiler::emitByte(Op op) { emitByte(static_cast<Byte>(op)); }

void Compiler::emitBytes(Op op, Byte byte) {
    emitByte(op);
    emitByte(byte);
}

int Compiler::emitJump(Op op) {
    emitByte(op);
    emitByte(0xff);
    emitByte(0xff);
    return static_cast<int>(getCurrentChunk()->size()) - 2;
}

void Compiler::patchJump(int offset) {
    int jump = static_cast<int>(getCurrentChunk()->size()) - offset - 2;
    if (jump > UINT16_MAX) {
        m_parser->error("Too much code to jump over.");
        return;
    }
    getCurrentChunk()->patch(offset, static_cast<uint8_t>((jump >> 8) & 0xff));
    getCurrentChunk()->patch(offset + 1, static_cast<uint8_t>(jump & 0xff));
}

void Compiler::emitLoop(int loopStart) {
    emitByte(Op::LOOP);
    int offset = static_cast<int>(getCurrentChunk()->size()) - loopStart + 2;
    if (offset > UINT16_MAX) {
        m_parser->error("Loop body too large.");
        return;
    }
    emitByte(static_cast<uint8_t>((offset >> 8) & 0xff));
    emitByte(static_cast<uint8_t>(offset & 0xff));
}

void Compiler::emitLoopCleanup() {
    int toPop = m_localCount - m_loopStack.back().localCount;
    for (int i = 0; i < toPop; i++)
        emitByte(Op::POP);
}

uint8_t Compiler::makeConstant(Value value) {
    auto constant = getCurrentChunk()->addConstant(value);
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
