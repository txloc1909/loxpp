#include "compiler.h"
#include "debug.h"
#include "memory_manager.h"
#include "utility.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <set>
#include <unistd.h>

ObjFunction* compile(const std::string& source, MemoryManager* mm) {
    ObjFunction* fn = mm->create<ObjFunction>();
    auto parser = std::make_unique<Parser>(source);
    auto compiler = std::make_unique<Compiler>(fn, parser.get(), mm,
                                               FunctionType::SCRIPT, nullptr);

    while (!parser->check(TokenType::EOF_))
        compiler->declaration();

    compiler->endCompiler();

    if (parser->m_hadError)
        return nullptr;
    return fn;
}

Compiler::Compiler(ObjFunction* function, Parser* parser, MemoryManager* mm,
                   FunctionType type, Compiler* enclosing)
    : m_function{function}, m_parser{parser}, m_mm{mm}, m_type{type},
      m_enclosing{enclosing} {
    // Reserve slot 0: "this" for methods/initializers, empty name for others.
    // The empty name ensures resolveLocal never accidentally matches it for
    // non-method functions; "this" allows method bodies to capture the
    // receiver.
    Local* implicit = &m_locals[m_localCount++];
    implicit->depth = 0;
    if (type == FunctionType::METHOD || type == FunctionType::INITIALIZER)
        implicit->name = Token{TokenType::THIS, "this", 0};
    else
        implicit->name = Token{TokenType::IDENTIFIER, "", 0};

    // Inherit class context so inner compilers (method bodies, closures inside
    // methods) can still resolve 'this'.
    if (m_enclosing != nullptr)
        m_currentClass = m_enclosing->m_currentClass;

    m_mm->setCurrentCompiler(this);
}

Compiler::~Compiler() { m_mm->setCurrentCompiler(m_enclosing); }

void Compiler::markRoots(MemoryManager& mm) const {
    mm.markObject(m_function);
    if (m_enclosing)
        m_enclosing->markRoots(mm);
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
    case TokenType::PERCENT:
        emitByte(Op::MODULO);
        break;
    case TokenType::IN:
        emitByte(Op::IN);
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
    if (m_parser->match(TokenType::CLASS)) {
        classDeclaration();
    } else if (m_parser->match(TokenType::ENUM)) {
        enumDeclaration();
    } else if (m_parser->match(TokenType::FUN)) {
        funDeclaration();
    } else if (m_parser->match(TokenType::VAR)) {
        varDeclaration();
    } else {
        statement();
    }
    if (m_parser->m_panicMode)
        m_parser->synchronize();
}

void Compiler::varDestructure() {
    m_parser->consume(TokenType::LEFT_BRACE,
                      "Expect '{' in destructuring pattern.");

    std::vector<Token> fields;
    do {
        if (fields.size() >= UINT8_COUNT)
            m_parser->error("Too many fields in destructuring pattern.");
        m_parser->consume(TokenType::IDENTIFIER, "Expect field name.");
        fields.push_back(m_parser->m_previous);
    } while (m_parser->match(TokenType::COMMA) &&
             !m_parser->check(TokenType::RIGHT_BRACE));

    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after field names.");
    m_parser->consume(TokenType::EQUAL,
                      "Expect '=' after destructuring pattern.");

    expression();

    m_parser->consume(TokenType::SEMICOLON,
                      "Expect ';' after variable declaration.");

    if (m_scopeDepth > 0)
        emitDestructureLocal(fields);
    else
        emitDestructureGlobal(fields);
}

void Compiler::emitDestructureLocal(const std::vector<Token>& fields) {
    // src_obj is already on the stack from expression(); claim it as a hidden
    // local.
    Token hidden;
    hidden.type = TokenType::IDENTIFIER;
    hidden.lexeme = "#src";
    hidden.line = m_parser->m_previous.line;
    addLocal(hidden);
    int srcSlot = m_localCount - 1;
    m_locals[srcSlot].depth = m_scopeDepth;

    for (const Token& field : fields) {
        for (int i = m_localCount - 1; i >= 0; i--) {
            const Local& local = m_locals[i];
            if (local.depth != -1 && local.depth < m_scopeDepth)
                break;
            if (local.name.lexeme == field.lexeme)
                m_parser->error(
                    "Already a variable with this name in this scope.");
        }
        addLocal(field);
        emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(srcSlot));
        emitBytes(Op::GET_PROPERTY, identifierConstant(field));
        m_locals[m_localCount - 1].depth = m_scopeDepth;
    }
}

void Compiler::emitDestructureGlobal(const std::vector<Token>& fields) {
    Token hidden;
    hidden.type = TokenType::IDENTIFIER;
    hidden.lexeme = "#destruct";
    hidden.line = m_parser->m_previous.line;
    uint8_t hiddenIdx = identifierConstant(hidden);
    emitBytes(Op::DEFINE_GLOBAL, hiddenIdx);

    for (const Token& field : fields) {
        emitBytes(Op::GET_GLOBAL, hiddenIdx);
        emitBytes(Op::GET_PROPERTY, identifierConstant(field));
        emitBytes(Op::DEFINE_GLOBAL, identifierConstant(field));
    }
}

void Compiler::varDeclaration() {
    if (m_parser->check(TokenType::LEFT_BRACE)) {
        varDestructure();
        return;
    }

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
        if (m_locals[m_localCount - 1].isCaptured)
            emitByte(Op::CLOSE_UPVALUE);
        else
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

int Compiler::addUpvalue(uint8_t index, bool isLocal) {
    for (int i = 0; i < m_upvalueCount; i++) {
        if (m_upvalues[i].index == index && m_upvalues[i].isLocal == isLocal)
            return i;
    }
    if (m_upvalueCount == UINT8_COUNT) {
        m_parser->error("Too many closure variables in function.");
        return 0;
    }
    m_upvalues[m_upvalueCount] = {index, isLocal};
    m_function->upvalueCount = m_upvalueCount + 1;
    return m_upvalueCount++;
}

int Compiler::resolveUpvalue(const Token& name) {
    if (m_enclosing == nullptr)
        return -1;
    int local = m_enclosing->resolveLocal(name);
    if (local != -1) {
        m_enclosing->m_locals[local].isCaptured = true;
        return addUpvalue(static_cast<uint8_t>(local), true);
    }
    int upvalue = m_enclosing->resolveUpvalue(name);
    if (upvalue != -1)
        return addUpvalue(static_cast<uint8_t>(upvalue), false);
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
    } else if (m_parser->match(TokenType::MATCH)) {
        matchStatement();
    } else if (m_parser->match(TokenType::PRINT)) {
        printStatement();
    } else if (m_parser->match(TokenType::RETURN)) {
        returnStatement();
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
        if (m_parser->check(TokenType::IDENTIFIER)) {
            m_parser->advance(); // consume identifier -> m_previous = itemName
            Token itemName = m_parser->m_previous;
            if (m_parser->match(TokenType::IN)) {
                forInStatement(itemName);
                endScope();
                return;
            }
            // C-style for (var x = ...; ...) fall-through.
            // VAR + identifier already consumed; replicate varDeclaration()
            // tail.
            declareVariable(); // uses m_previous (= itemName)
            if (m_parser->match(TokenType::EQUAL)) {
                expression();
            } else {
                emitByte(Op::NIL);
            }
            m_parser->consume(TokenType::SEMICOLON,
                              "Expect ';' after variable declaration.");
            markInitialized();
        } else {
            varDeclaration();
        }
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

void Compiler::forInStatement(const Token& itemName) {
    // At entry: 'for' '(' 'var' itemName 'in' have been consumed.
    // Caller (forStatement) owns beginScope()/endScope().

    // 1. Evaluate iterable expression; GET_ITER replaces it with an
    // ObjIterator.
    expression();
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after sequence.");
    emitByte(Op::GET_ITER);

    Token iterToken{TokenType::IDENTIFIER, "(iter)", m_parser->m_previous.line};
    addLocal(iterToken);
    markInitialized();
    int iterSlot = m_localCount - 1;

    // 2. Item variable starts as nil.
    emitByte(Op::NIL);
    addLocal(itemName);
    markInitialized();
    int itemSlot = m_localCount - 1;

    // 3. Loop header (re-entry point for LOOP and continue).
    int loopStart = static_cast<int>(getCurrentChunk()->size());
    m_loopStack.push_back({loopStart, m_localCount, {}});

    // 4. Check: push iterator copy, ITER_HAS_NEXT → bool on stack.
    emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(iterSlot));
    emitByte(Op::ITER_HAS_NEXT);
    int exitJump = emitJump(Op::JUMP_IF_FALSE);
    emitByte(Op::POP); // discard true

    // 5. Advance: push iterator copy, ITER_NEXT → next element; assign to item.
    emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(iterSlot));
    emitByte(Op::ITER_NEXT);
    emitBytes(Op::SET_LOCAL, static_cast<uint8_t>(itemSlot));
    emitByte(Op::POP);

    // 6. User body.
    statement();
    emitLoop(loopStart);

    // 7. Exit.
    patchJump(exitJump);
    emitByte(Op::POP); // discard false

    for (int offset : m_loopStack.back().breakJumps)
        patchJump(offset);
    m_loopStack.pop_back();
}

void Compiler::breakStatement() {
    if (m_loopStack.empty()) {
        m_parser->error("Cannot use 'break' outside a loop or match.");
        return;
    }
    m_parser->consume(TokenType::SEMICOLON, "Expect ';' after 'break'.");
    emitLoopCleanup(m_loopStack.back().localCount);
    int jump = emitJump(Op::JUMP);
    m_loopStack.back().breakJumps.push_back(jump);
}

void Compiler::continueStatement() {
    // Search upward for the nearest real loop (start != -1); skip switch
    // contexts.
    for (int i = static_cast<int>(m_loopStack.size()) - 1; i >= 0; i--) {
        if (m_loopStack[i].start != -1) {
            m_parser->consume(TokenType::SEMICOLON,
                              "Expect ';' after 'continue'.");
            emitLoopCleanup(m_loopStack[i].localCount);
            emitLoop(m_loopStack[i].start);
            return;
        }
    }
    m_parser->error("Cannot use 'continue' outside a loop.");
}

void Compiler::matchExpression() {
    compileMatchBody(/*asExpr=*/true);
}

void Compiler::matchStatement() {
    compileMatchBody(/*asExpr=*/false);
}

void Compiler::compileMatchBody(bool asExpr) {
    beginScope();

    // Compile subject into a hidden local so case arms can reload it with
    // GET_LOCAL.
    expression();
    Token syntheticName{TokenType::IDENTIFIER, "(match)",
                        m_parser->m_previous.line};
    addLocal(syntheticName);
    markInitialized();
    int subjectSlot = m_localCount - 1;

    // In expression mode, pre-allocate a result slot to hold each arm's value.
    int resultSlot = -1;
    if (asExpr) {
        emitByte(Op::NIL);
        Token resultName{TokenType::IDENTIFIER, "(match_result)",
                         m_parser->m_previous.line};
        addLocal(resultName);
        markInitialized();
        resultSlot = m_localCount - 1;
    }

    int armLocalBase = m_localCount;

    m_parser->consume(TokenType::LEFT_BRACE, "Expect '{' before match body.");

    // start = -1 flags this context as a match (not a loop).
    m_loopStack.push_back({-1, m_localCount, {}});

    bool hasUnguardedCatchAll = false;
    std::set<std::string> seenCtors;

    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        if (!m_parser->match(TokenType::CASE)) {
            m_parser->error("Expect 'case' in match body.");
            break;
        }
        if (hasUnguardedCatchAll) {
            m_parser->error("Unreachable arm after catch-all.");
        }

        int armLocalBase_iter = m_localCount;
        auto arm = compileMatchArm(subjectSlot, armLocalBase_iter, resultSlot, asExpr);
        if (arm.isUnguardedCatchAll) {
            hasUnguardedCatchAll = true;
        }
        if (!arm.ctorName.empty()) {
            seenCtors.insert(arm.ctorName);
        }

        // Guard-fail exit: pop guard bool, pop binding locals.
        // If lastMiss is also set (constructor arm), the guard-fail path must
        // jump over the lastMiss POP to avoid a spurious extra pop.
        int skipLastMissPop = -1;
        if (arm.guardMiss != -1) {
            patchJump(arm.guardMiss);
            emitByte(Op::POP); // pop false guard
            for (int i = 0; i < arm.bindingCount; i++) {
                emitByte(Op::POP); // pop binding value(s)
            }
            if (arm.lastMiss != -1) {
                skipLastMissPop = emitJump(Op::JUMP);
            }
        }

        // Literal/tag miss: pop false equality, fall through to next arm.
        if (arm.lastMiss != -1) {
            patchJump(arm.lastMiss);
            emitByte(Op::POP);
        }

        if (skipLastMissPop != -1) {
            patchJump(skipLastMissPop);
        }
    }

    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after match body.");

    if (!hasUnguardedCatchAll && !seenCtors.empty()) {
        checkEnumExhaustiveness(seenCtors);
    }

    // No unguarded catch-all → raise MatchError when no arm matched.
    if (!hasUnguardedCatchAll) {
        emitByte(Op::MATCH_ERROR);
    }

    for (int j : m_loopStack.back().breakJumps) {
        patchJump(j);
    }
    m_loopStack.pop_back();

    if (!asExpr) {
        endScope(); // pops the hidden subject local
    } else {
        // In expression mode: the result is in the result local.
        // We need to leave the result on the stack for the expression context.
        // The stack currently has values from SET_LOCAL (peek) operations.
        // We leave just the result by:
        // 1. Keep result value on stack (it was SET_LOCAL'd without final POP)
        // 2. Load result from local onto stack
        emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(resultSlot));
        // 3. Close the match scope without emitting POPs
        m_scopeDepth--;
        while (m_localCount > 0 &&
               m_locals[m_localCount - 1].depth > m_scopeDepth) {
            m_localCount--;
        }
    }
}

Compiler::MatchArmResult Compiler::compileMatchArm(int subjectSlot,
                                                   int armLocalBase,
                                                   int resultSlot,
                                                   bool asExpr) {
    bool isIdentPat = m_parser->check(TokenType::IDENTIFIER);
    int lastMiss = -1;
    int bindingCount = 0;
    std::string matchedCtorName;

    if (isIdentPat) {
        Token patTok = m_parser->m_current;
        m_parser->advance();
        std::string patName(patTok.lexeme);

        const ConstructorInfo* info = findConstructor(patName);

        // Constructor pattern: emit tag check + field bindings.
        if (info != nullptr) {
            matchedCtorName = patName;

            emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
            emitByte(Op::GET_TAG);
            emitBytes(Op::CONSTANT,
                      makeConstant(Value{static_cast<double>(info->tag)}));
            emitByte(Op::EQUAL);
            lastMiss = emitJump(Op::JUMP_IF_FALSE);
            emitByte(Op::POP); // pop true

            bindingCount = compileCtorPattern(subjectSlot, *info, patTok);
        } else if (findClass(patName)) {
            // Class pattern: instanceof check + named-field bindings.
            ObjString* classNameStr = m_mm->makeString(patTok.lexeme);
            m_mm->pushTempRoot(classNameStr);
            uint8_t classNameConst =
                makeConstant(Value{static_cast<Obj*>(classNameStr)});
            m_mm->popTempRoot();
            emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
            emitBytes(Op::INSTANCEOF, classNameConst);
            lastMiss = emitJump(Op::JUMP_IF_FALSE);
            emitByte(Op::POP); // pop true
            bindingCount = compileClassPattern(subjectSlot, patTok);
        } else if (m_parser->check(TokenType::LEFT_BRACE) ||
                   m_parser->check(TokenType::LEFT_PAREN)) {
            // Looks like a constructor pattern but name is unknown.
            m_parser->error(
                ("Unknown constructor or class '" + patName + "'.").c_str());
        } else {
            // Plain binding or wildcard.
            if (m_parser->check(TokenType::COMMA)) {
                m_parser->error(
                    "A binding pattern must be the sole arm pattern.");
            }
            if (patName != "_") {
                emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
                Token bindTok{TokenType::IDENTIFIER, patTok.lexeme,
                              patTok.line};
                addLocal(bindTok);
                markInitialized();
                bindingCount = 1;
            }
        }
    } else {
        // Literal pattern(s): equality chain.
        // hitJumps: JUMP_IF_FALSE per non-last literal (miss → try next).
        // bodyJumps: unconditional JUMP per non-last literal (hit → body).
        std::vector<int> hitJumps;
        std::vector<int> bodyJumps;

        do {
            emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
            m_parser->parsePrecedence(Precedence::OR, this);
            emitByte(Op::EQUAL);

            if (m_parser->check(TokenType::COMMA)) {
                hitJumps.push_back(emitJump(Op::JUMP_IF_FALSE));
                emitByte(Op::POP);
                bodyJumps.push_back(emitJump(Op::JUMP));
                patchJump(hitJumps.back());
                emitByte(Op::POP);
            }
        } while (m_parser->match(TokenType::COMMA));

        lastMiss = emitJump(Op::JUMP_IF_FALSE);
        emitByte(Op::POP);
        for (int j : bodyJumps) {
            patchJump(j);
        }
    }

    // Guard clause (optional).
    int guardMiss = -1;
    bool hasGuard = m_parser->match(TokenType::IF);
    if (hasGuard) {
        expression();
        guardMiss = emitJump(Op::JUMP_IF_FALSE);
        emitByte(Op::POP);
    }

    m_parser->consume(TokenType::FAT_ARROW, "Expect '=>' after pattern.");

    // Body (single statement or braced block, or expression-mode arm).
    beginScope();
    if (asExpr) {
        // Expression mode: arm body must produce a value.
        // { decls* expr }  — final expression (no trailing ';') is the value.
        // or bare expr.
        if (m_parser->match(TokenType::LEFT_BRACE)) {
            while (!m_parser->check(TokenType::RIGHT_BRACE) &&
                   !m_parser->check(TokenType::EOF_)) {
                if (m_parser->check(TokenType::VAR) ||
                    m_parser->check(TokenType::FUN) ||
                    m_parser->check(TokenType::CLASS) ||
                    m_parser->check(TokenType::ENUM)) {
                    declaration();
                } else {
                    expression();
                    if (m_parser->match(TokenType::SEMICOLON)) {
                        emitByte(Op::POP);  // discard intermediate expression
                    } else {
                        break;  // last expression — the arm value
                    }
                }
            }
            m_parser->consume(TokenType::RIGHT_BRACE,
                              "Expect '}' after match arm body.");
        } else {
            expression();
        }
        // Save arm result into pre-allocated result slot, pop redundant copy.
        emitBytes(Op::SET_LOCAL, static_cast<uint8_t>(resultSlot));
        emitByte(Op::POP);
    } else {
        // Statement mode: original behavior (single statement or braced block).
        if (m_parser->match(TokenType::LEFT_BRACE)) {
            while (!m_parser->check(TokenType::RIGHT_BRACE) &&
                   !m_parser->check(TokenType::EOF_)) {
                declaration();
            }
            m_parser->consume(TokenType::RIGHT_BRACE,
                              "Expect '}' after match arm body.");
        } else {
            statement();
        }
    }
    endScope();

    // Normal arm exit: clean up binding locals, jump to match end.
    emitLoopCleanup(armLocalBase);
    m_localCount = armLocalBase;
    int endJump = emitJump(Op::JUMP);
    m_loopStack.back().breakJumps.push_back(endJump);

    // A plain identifier is a catch-all only if it emitted no conditional check
    // (lastMiss == -1 means no JUMP_IF_FALSE was emitted).
    bool isPlainIdentCatchAll =
        isIdentPat && matchedCtorName.empty() && lastMiss == -1 && !hasGuard;
    return {lastMiss, guardMiss, bindingCount, isPlainIdentCatchAll,
            matchedCtorName};
}

Compiler* Compiler::findRootCompiler() {
    Compiler* c = this;
    while (c->m_enclosing != nullptr) {
        c = c->m_enclosing;
    }
    return c;
}

const ConstructorInfo*
Compiler::findConstructor(const std::string& name) const {
    const Compiler* c = this;
    while (c != nullptr) {
        auto it = c->m_constructors.find(name);
        if (it != c->m_constructors.end()) {
            return &it->second;
        }
        c = c->m_enclosing;
    }
    return nullptr;
}

bool Compiler::findClass(const std::string& name) const {
    const Compiler* found = walkChain<Compiler>(
        this,
        [&name](const Compiler* c) {
            return c->m_classNames.count(name) != 0U;
        },
        [](const Compiler* c) { return c->m_enclosing; });
    return found != nullptr;
}

int Compiler::compileClassPattern(int subjectSlot, const Token& patTok) {
    if (m_parser->check(TokenType::LEFT_PAREN)) {
        m_parser->error("Class patterns only support named-field syntax "
                        "(e.g., 'ClassName{field1, field2}').");
        return 0;
    }
    if (!m_parser->check(TokenType::LEFT_BRACE))
        return 0; // zero-field class pattern — instanceof check only

    m_parser->advance(); // consume '{'
    int bindingCount = 0;
    if (!m_parser->check(TokenType::RIGHT_BRACE)) {
        do {
            m_parser->consume(TokenType::IDENTIFIER, "Expect field name.");
            Token fieldTok = m_parser->m_previous;
            uint8_t propConst = identifierConstant(fieldTok);
            emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
            emitBytes(Op::GET_PROPERTY, propConst);
            addLocal(fieldTok);
            markInitialized();
            bindingCount++;
        } while (m_parser->match(TokenType::COMMA));
    }
    m_parser->consume(TokenType::RIGHT_BRACE,
                      "Expect '}' after field pattern.");
    return bindingCount;
}

void Compiler::enumDeclaration() {
    if (m_scopeDepth > 0) {
        m_parser->error("Enum declarations are only allowed at global scope.");
        return;
    }

    m_parser->consume(TokenType::IDENTIFIER, "Expect enum name.");
    Token enumTok = m_parser->m_previous;
    std::string enumName(enumTok.lexeme);

    Compiler* root = findRootCompiler();
    if (root->m_enumCtors.count(enumName) != 0U) {
        m_parser->error("Duplicate enum name.");
        return;
    }

    m_parser->consume(TokenType::LEFT_BRACE, "Expect '{' before enum body.");

    uint8_t tag = 0;
    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        m_parser->consume(TokenType::IDENTIFIER, "Expect constructor name.");
        Token ctorTok = m_parser->m_previous;
        std::string ctorName(ctorTok.lexeme);

        if (root->m_constructors.count(ctorName) != 0U) {
            m_parser->error("Duplicate constructor name.");
            return;
        }

        std::vector<std::string> fieldNames;
        if (m_parser->match(TokenType::LEFT_PAREN)) {
            if (!m_parser->check(TokenType::RIGHT_PAREN)) {
                do {
                    m_parser->consume(TokenType::IDENTIFIER,
                                      "Expect field name.");
                    fieldNames.emplace_back(m_parser->m_previous.lexeme);
                } while (m_parser->match(TokenType::COMMA));
            }
            m_parser->consume(TokenType::RIGHT_PAREN,
                              "Expect ')' after field list.");
        }

        auto arity = static_cast<uint8_t>(fieldNames.size());

        ObjString* ctorNameStr = m_mm->makeString(ctorTok.lexeme);
        m_mm->pushTempRoot(ctorNameStr);
        ObjString* enumNameStr = m_mm->makeString(enumTok.lexeme);
        m_mm->pushTempRoot(enumNameStr);

        auto* ctor =
            m_mm->create<ObjEnumCtor>(tag, arity, ctorNameStr, enumNameStr);
        m_mm->pushTempRoot(ctor);
        uint8_t ctorConst = makeConstant(Value{static_cast<Obj*>(ctor)});
        m_mm->popTempRoot(); // ctor
        m_mm->popTempRoot(); // enumNameStr
        m_mm->popTempRoot(); // ctorNameStr

        uint8_t nameConst = identifierConstant(ctorTok);
        emitBytes(Op::CONSTANT, ctorConst);
        emitBytes(Op::DEFINE_GLOBAL, nameConst);

        root->m_constructors[ctorName] = {enumName, tag, arity, fieldNames};
        root->m_enumCtors[enumName].push_back(ctorName);
        tag++;
    }

    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after enum body.");
}

int Compiler::compileCtorPattern(int subjectSlot, const ConstructorInfo& info,
                                 const Token& patTok) {
    int bindingCount = 0;
    bool namedFields = m_parser->check(TokenType::LEFT_BRACE) &&
                       !m_parser->check(TokenType::FAT_ARROW) &&
                       !m_parser->check(TokenType::IF);
    bool positionalFields =
        !namedFields && m_parser->check(TokenType::LEFT_PAREN);

    if (namedFields) {
        m_parser->advance(); // consume '{'
        if (!m_parser->check(TokenType::RIGHT_BRACE)) {
            do {
                m_parser->consume(TokenType::IDENTIFIER, "Expect field name.");
                std::string fieldName(m_parser->m_previous.lexeme);
                auto it = std::find(info.fieldNames.begin(),
                                    info.fieldNames.end(), fieldName);
                if (it == info.fieldNames.end()) {
                    m_parser->error(("Unknown field '" + fieldName + "' for '" +
                                     std::string(patTok.lexeme) + "'.")
                                        .c_str());
                    break;
                }
                auto idx = static_cast<uint8_t>(
                    std::distance(info.fieldNames.begin(), it));
                emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
                emitBytes(Op::CONSTANT,
                          makeConstant(Value{static_cast<double>(idx)}));
                emitByte(Op::GET_INDEX);
                addLocal(m_parser->m_previous);
                markInitialized();
                bindingCount++;
            } while (m_parser->match(TokenType::COMMA));
        }
        m_parser->consume(TokenType::RIGHT_BRACE,
                          "Expect '}' after field pattern.");
    } else if (positionalFields) {
        m_parser->advance(); // consume '('
        int i = 0;
        if (!m_parser->check(TokenType::RIGHT_PAREN)) {
            do {
                if (i >= static_cast<int>(info.arity)) {
                    m_parser->error("Too many fields in constructor pattern.");
                    break;
                }
                m_parser->consume(TokenType::IDENTIFIER,
                                  "Expect binding name.");
                emitBytes(Op::GET_LOCAL, static_cast<uint8_t>(subjectSlot));
                emitBytes(Op::CONSTANT,
                          makeConstant(Value{static_cast<double>(i)}));
                emitByte(Op::GET_INDEX);
                addLocal(m_parser->m_previous);
                markInitialized();
                bindingCount++;
                i++;
            } while (m_parser->match(TokenType::COMMA));
        }
        m_parser->consume(TokenType::RIGHT_PAREN,
                          "Expect ')' after field pattern.");
    }
    return bindingCount;
}

void Compiler::checkEnumExhaustiveness(const std::set<std::string>& seenCtors) {
    std::set<std::string> checkedEnums;
    for (const auto& ctorName : seenCtors) {
        const ConstructorInfo* info = findConstructor(ctorName);
        if (info == nullptr) {
            continue;
        }
        if (checkedEnums.count(info->enumName) != 0U) {
            continue;
        }
        checkedEnums.insert(info->enumName);

        Compiler* root = findRootCompiler();
        auto enumIt = root->m_enumCtors.find(info->enumName);
        if (enumIt == root->m_enumCtors.end()) {
            continue;
        }
        std::vector<std::string> missing;
        for (const auto& c : enumIt->second) {
            if (seenCtors.count(c) == 0U) {
                missing.push_back(c);
            }
        }
        if (!missing.empty()) {
            std::string msg = "Non-exhaustive match on enum '" +
                              info->enumName + "': missing arms for: ";
            for (size_t i = 0; i < missing.size(); i++) {
                if (i > 0) {
                    msg += ", ";
                }
                msg += missing[i];
            }
            m_parser->error(msg.c_str());
        }
    }
}

void Compiler::funDeclaration() {
    m_parser->consume(TokenType::IDENTIFIER, "Expect function name.");
    Token name = m_parser->m_previous;

    uint8_t nameConst = 0;
    if (m_scopeDepth > 0) {
        declareVariable();
        markInitialized(); // allow recursion inside the body
    } else {
        nameConst = identifierConstant(name);
    }

    ObjFunction* fn = m_mm->create<ObjFunction>();
    // Construct inner before makeString: inner's constructor calls
    // setCurrentCompiler(&inner), rooting fn through inner.markRoots()
    // before any allocation that can trigger GC.
    Compiler inner(fn, m_parser, m_mm, FunctionType::FUNCTION, this);
    fn->name = m_mm->makeString(name.lexeme);
    inner.parseFunction(FunctionType::FUNCTION);

    emitBytes(Op::CLOSURE, makeConstant(Value{static_cast<Obj*>(fn)}));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(inner.m_upvalues[i].isLocal ? 1 : 0);
        emitByte(inner.m_upvalues[i].index);
    }
    if (m_scopeDepth == 0) {
        emitBytes(Op::DEFINE_GLOBAL, nameConst);
    }
}

void Compiler::classDeclaration() {
    m_parser->consume(TokenType::IDENTIFIER, "Expect class name.");
    Token className = m_parser->m_previous;
    uint8_t nameConst = identifierConstant(className);

    if (m_scopeDepth > 0)
        declareVariable();

    emitBytes(Op::CLASS, nameConst);

    if (m_scopeDepth > 0) {
        markInitialized();
    } else {
        emitBytes(Op::DEFINE_GLOBAL, nameConst);
        findRootCompiler()->m_classNames.insert(std::string(className.lexeme));
    }

    ClassCompiler classCompiler;
    classCompiler.enclosing = m_currentClass;
    m_currentClass = &classCompiler;

    if (m_parser->match(TokenType::LESS)) {
        m_parser->consume(TokenType::IDENTIFIER, "Expect superclass name.");
        if (m_parser->m_previous.lexeme == className.lexeme)
            m_parser->error("A class can't inherit from itself.");
        namedVariable(m_parser->m_previous, false); // push superclass

        // Introduce "super" as a local so method closures can capture it.
        beginScope();
        addLocal(Token{TokenType::SUPER, "super", 0});
        markInitialized();

        namedVariable(className, false); // push subclass
        emitByte(
            Op::INHERIT); // pops subclass, copies methods, leaves superclass

        classCompiler.hasSuperclass = true;
    }

    // Push class so DEFINE_METHOD can find it at peek(1) throughout the method
    // loop. Must come after superclass setup: INHERIT leaves superclass on the
    // stack, so the class pushed here lands at the correct peek(1) position.
    namedVariable(className, false);

    m_parser->consume(TokenType::LEFT_BRACE, "Expect '{' before class body.");
    while (!m_parser->check(TokenType::RIGHT_BRACE) &&
           !m_parser->check(TokenType::EOF_)) {
        method();
    }
    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after class body.");

    emitByte(Op::POP); // pop the class pushed by namedVariable above

    if (classCompiler.hasSuperclass)
        endScope(); // closes the "super" local/upvalue

    m_currentClass = m_currentClass->enclosing;
}

void Compiler::method() {
    m_parser->consume(TokenType::IDENTIFIER, "Expect method name.");
    uint8_t nameConst = identifierConstant(m_parser->m_previous);

    FunctionType type = FunctionType::METHOD;
    if (m_parser->m_previous.lexeme == "init")
        type = FunctionType::INITIALIZER;

    ObjFunction* fn = m_mm->create<ObjFunction>();
    fn->name = m_mm->makeString(m_parser->m_previous.lexeme);

    Compiler inner(fn, m_parser, m_mm, type, this);
    inner.parseFunction(type);

    emitBytes(Op::CLOSURE, makeConstant(Value{static_cast<Obj*>(fn)}));
    for (int i = 0; i < fn->upvalueCount; i++) {
        emitByte(inner.m_upvalues[i].isLocal ? 1 : 0);
        emitByte(inner.m_upvalues[i].index);
    }
    emitBytes(Op::DEFINE_METHOD, nameConst);
}

void Compiler::this_() {
    if (m_currentClass == nullptr) {
        m_parser->error("Can't use 'this' outside of a class.");
        return;
    }
    namedVariable(m_parser->m_previous, false);
}

void Compiler::super_() {
    if (m_currentClass == nullptr)
        m_parser->error("Can't use 'super' outside of a class.");
    else if (!m_currentClass->hasSuperclass)
        m_parser->error("Can't use 'super' in a class with no superclass.");

    m_parser->consume(TokenType::DOT, "Expect '.' after 'super'.");
    m_parser->consume(TokenType::IDENTIFIER, "Expect superclass method name.");
    uint8_t nameConst = identifierConstant(m_parser->m_previous);

    namedVariable(Token{TokenType::THIS, "this", 0}, false); // push receiver

    if (m_parser->match(TokenType::LEFT_PAREN)) {
        uint8_t argCount = 0;
        if (!m_parser->check(TokenType::RIGHT_PAREN)) {
            do {
                if (argCount == 255)
                    m_parser->error("Can't have more than 255 arguments.");
                expression();
                argCount++;
            } while (m_parser->match(TokenType::COMMA));
        }
        m_parser->consume(TokenType::RIGHT_PAREN,
                          "Expect ')' after arguments.");
        namedVariable(Token{TokenType::SUPER, "super", 0}, false);
        emitBytes(Op::SUPER_INVOKE, nameConst);
        emitByte(argCount);
    } else {
        namedVariable(Token{TokenType::SUPER, "super", 0}, false);
        emitBytes(Op::GET_SUPER, nameConst);
    }
}

void Compiler::dot() {
    m_parser->consume(TokenType::IDENTIFIER, "Expect property name after '.'.");
    uint8_t nameConst = identifierConstant(m_parser->m_previous);

    if (m_parser->m_canAssign && m_parser->match(TokenType::EQUAL)) {
        expression();
        emitBytes(Op::SET_PROPERTY, nameConst);
    } else if (m_parser->match(TokenType::LEFT_PAREN)) {
        // Fuse GET_PROPERTY + CALL into a single INVOKE superinstruction.
        uint8_t argCount = 0;
        if (!m_parser->check(TokenType::RIGHT_PAREN)) {
            do {
                if (argCount == 255)
                    m_parser->error("Can't have more than 255 arguments.");
                expression();
                argCount++;
            } while (m_parser->match(TokenType::COMMA));
        }
        m_parser->consume(TokenType::RIGHT_PAREN,
                          "Expect ')' after arguments.");
        emitBytes(Op::INVOKE, nameConst);
        emitByte(argCount);
    } else {
        emitBytes(Op::GET_PROPERTY, nameConst);
    }
}

void Compiler::listLiteral() {
    uint8_t count = 0;
    if (!m_parser->check(TokenType::RIGHT_BRACKET)) {
        do {
            expression();
            count++;
        } while (m_parser->match(TokenType::COMMA));
    }
    m_parser->consume(TokenType::RIGHT_BRACKET,
                      "Expect ']' after list elements.");
    emitBytes(Op::BUILD_LIST, count);
}

void Compiler::mapLiteral() {
    uint8_t count = 0;
    if (!m_parser->check(TokenType::RIGHT_BRACE)) {
        do {
            expression(); // key
            m_parser->consume(TokenType::COLON, "Expect ':' after map key.");
            expression(); // value
            if (count == 255) {
                m_parser->error(
                    "Cannot have more than 255 entries in a map literal.");
            }
            count++;
        } while (m_parser->match(TokenType::COMMA));
    }
    m_parser->consume(TokenType::RIGHT_BRACE, "Expect '}' after map entries.");
    emitBytes(Op::BUILD_MAP, count);
}

void Compiler::subscript() {
    bool canAssign = m_parser->m_canAssign; // save: compiling the index
                                            // expression clobbers m_canAssign
                                            // via nested parsePrecedence calls.
    expression();                           // compile index
    m_parser->consume(TokenType::RIGHT_BRACKET, "Expect ']' after index.");
    if (canAssign && m_parser->match(TokenType::EQUAL)) {
        expression(); // compile RHS
        emitByte(Op::SET_INDEX);
    } else {
        emitByte(Op::GET_INDEX);
    }
}

void Compiler::returnStatement() {
    if (m_type == FunctionType::SCRIPT) {
        m_parser->error("Can't return from top-level code.");
        return;
    }
    if (m_parser->match(TokenType::SEMICOLON)) {
        emitReturn();
    } else {
        if (m_type == FunctionType::INITIALIZER)
            m_parser->error("Can't return a value from an initializer.");
        expression();
        m_parser->consume(TokenType::SEMICOLON,
                          "Expect ';' after return value.");
        emitByte(Op::RETURN);
    }
}

void Compiler::parseFunction(FunctionType /*type*/) {
    beginScope();
    m_parser->consume(TokenType::LEFT_PAREN, "Expect '(' after function name.");
    if (!m_parser->check(TokenType::RIGHT_PAREN)) {
        do {
            m_function->arity++;
            if (m_function->arity > 255)
                m_parser->error("Can't have more than 255 parameters.");
            m_parser->consume(TokenType::IDENTIFIER, "Expect parameter name.");
            declareVariable();
            markInitialized();
        } while (m_parser->match(TokenType::COMMA));
    }
    m_parser->consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters.");
    m_parser->consume(TokenType::LEFT_BRACE,
                      "Expect '{' before function body.");
    block();
    endCompiler();
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
    if (m_type == FunctionType::INITIALIZER)
        emitBytes(Op::GET_LOCAL, 0); // implicit return of 'this'
    else
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

void Compiler::emitLoopCleanup(int targetLocalCount) {
    int toPop = m_localCount - targetLocalCount;
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
    } else if ((slot = resolveUpvalue(name)) != -1) {
        getOp = Op::GET_UPVALUE;
        setOp = Op::SET_UPVALUE;
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
