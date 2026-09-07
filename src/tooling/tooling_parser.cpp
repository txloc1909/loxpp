#include "tooling/tooling_parser.h"

#include <utility>
#include <vector>

#include "scanner.h"
#include "token.h"

namespace loxpp::tooling {
namespace {

bool isLiteralToken(TokenType t) {
    switch (t) {
    case TokenType::NUMBER:
    case TokenType::STRING:
    case TokenType::TRUE:
    case TokenType::FALSE:
    case TokenType::NIL:
        return true;
    default:
        return false;
    }
}

LiteralKind literalKindOf(TokenType t) {
    switch (t) {
    case TokenType::NUMBER:
        return LiteralKind::Number;
    case TokenType::STRING:
        return LiteralKind::String;
    case TokenType::TRUE:
        return LiteralKind::True;
    case TokenType::FALSE:
        return LiteralKind::False;
    default:
        return LiteralKind::Nil;
    }
}

// A token that begins a declaration or statement -- used to decide, inside a
// block-form match arm body, whether the next construct is a statement or the
// arm's final expression, and as a synchronise stop.
bool startsStatement(TokenType t) {
    switch (t) {
    case TokenType::VAR:
    case TokenType::FUN:
    case TokenType::CLASS:
    case TokenType::ENUM:
    case TokenType::PRINT:
    case TokenType::IF:
    case TokenType::WHILE:
    case TokenType::FOR:
    case TokenType::RETURN:
    case TokenType::BREAK:
    case TokenType::CONTINUE:
    case TokenType::LEFT_BRACE:
        return true;
    default:
        return false;
    }
}

class Parser {
  public:
    explicit Parser(const std::string& source) : m_source(source) {
        Scanner scanner(source);
        for (;;) {
            Token tok = scanner.scanOneToken();
            bool eof = tok.type == TokenType::EOF_;
            m_tokens.push_back(tok);
            if (eof) {
                break;
            }
        }
    }

    Program parseProgram() {
        Program prog;
        prog.offset = 0;
        prog.length = m_source.size();
        while (!isAtEnd()) {
            std::size_t before = m_pos;
            StmtPtr decl = declaration();
            if (decl) {
                prog.body.push_back(std::move(decl));
            }
            if (m_pos == before) {
                advance(); // guarantee forward progress
            }
        }
        return prog;
    }

  private:
    // -- recursion-depth guard --------------------------------------------

    // Cap on nested grammar recursion. Every nested expression (a paren
    // group, list or map element, call argument, subscript, unary prefix,
    // `=` right side, or `match` subject) and every nested block or control-
    // flow statement adds C++ stack frames; an editor buffer can hold
    // thousands of unbalanced `(` or `{`, which would overflow the stack.
    // Measured overflow under the ASan build is near 2000 levels, so 500
    // keeps a safety factor of four and is far deeper than any hand-written
    // nesting. Past the cap the parser stops descending, records an error,
    // and unwinds to the next recovery point, still returning a Program.
    static constexpr int kMaxNestingDepth = 500;

    class NestingGuard {
      public:
        explicit NestingGuard(Parser& parser) : m_parser(parser) {
            ++m_parser.m_depth;
        }
        ~NestingGuard() { --m_parser.m_depth; }
        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;

      private:
        Parser& m_parser;
    };

    [[nodiscard]] bool tooDeep() const { return m_depth > kMaxNestingDepth; }

    // -- token cursor -------------------------------------------------------

    [[nodiscard]] const Token& peek() const { return m_tokens[m_pos]; }
    [[nodiscard]] const Token& peekAt(std::size_t ahead) const {
        std::size_t i = m_pos + ahead;
        return i < m_tokens.size() ? m_tokens[i] : m_tokens.back();
    }
    // Before the first advance() the cursor sits at 0 and there is no previous
    // token; callers that reach here early (a first token that cannot start a
    // declaration or statement, an unterminated string) get token 0 rather
    // than a SIZE_MAX index.
    [[nodiscard]] const Token& previous() const {
        return m_tokens[m_pos == 0 ? 0 : m_pos - 1];
    }
    [[nodiscard]] bool isAtEnd() const {
        return peek().type == TokenType::EOF_;
    }

    const Token& advance() {
        if (!isAtEnd()) {
            m_pos++;
        }
        return previous();
    }

    [[nodiscard]] bool check(TokenType t) const { return peek().type == t; }

    bool match(TokenType t) {
        if (check(t)) {
            advance();
            return true;
        }
        return false;
    }

    // Consume a token of the given type, or enter panic mode. Never throws.
    bool consume(TokenType t) {
        if (check(t)) {
            advance();
            return true;
        }
        m_panic = true;
        return false;
    }

    void error() { m_panic = true; }

    // Skip tokens until the next likely statement boundary: just past a `;`,
    // or at a `}` or a declaration-keyword start. This is the call the
    // recovery tests remove to prove error recovery is real. Forward progress
    // for a caller loop is guaranteed by that loop's own position guard, not
    // here -- synchronize stops without advancing when the cursor already sits
    // at a boundary.
    void synchronize() {
        m_panic = false;
        while (!isAtEnd()) {
            if (m_pos > 0 && previous().type == TokenType::SEMICOLON) {
                return;
            }
            if (peek().type == TokenType::RIGHT_BRACE ||
                startsStatement(peek().type)) {
                return;
            }
            advance();
        }
    }

    // -- span helpers -----------------------------------------------------

    [[nodiscard]] std::string_view tokenText(const Token& t) const {
        if (t.offset + t.length > m_source.size()) {
            return {};
        }
        return {m_source.data() + t.offset, t.length};
    }

    [[nodiscard]] std::size_t endOfPrevious() const {
        if (m_pos == 0) {
            // No token consumed yet: a span that ends here is empty, anchored
            // at the first token's start.
            return m_tokens.front().offset;
        }
        const Token& p = previous();
        return p.offset + p.length;
    }

    void spanTo(Expr& node, std::size_t start) const {
        node.offset = start;
        std::size_t end = endOfPrevious();
        node.length = end > start ? end - start : 0;
    }
    void spanTo(Stmt& node, std::size_t start) const {
        node.offset = start;
        std::size_t end = endOfPrevious();
        node.length = end > start ? end - start : 0;
    }
    void spanTo(Pattern& node, std::size_t start) const {
        node.offset = start;
        std::size_t end = endOfPrevious();
        node.length = end > start ? end - start : 0;
    }

    [[nodiscard]] Name nameFrom(const Token& t) const {
        return Name{
            .text = tokenText(t), .offset = t.offset, .length = t.length};
    }
    [[nodiscard]] Param paramFrom(const Token& t) const {
        return Param{
            .name = tokenText(t), .offset = t.offset, .length = t.length};
    }

    // -- declarations -----------------------------------------------------

    StmtPtr declaration() {
        NestingGuard guard(*this);
        if (m_panic) {
            return nullptr;
        }
        if (tooDeep()) {
            error();
            return nullptr;
        }
        StmtPtr stmt = declarationInner();
        if (m_panic) {
            synchronize();
        }
        return stmt;
    }

    StmtPtr declarationInner() {
        if (check(TokenType::CLASS)) {
            return classDecl();
        }
        if (check(TokenType::FUN)) {
            return funDecl();
        }
        if (check(TokenType::VAR)) {
            return varDecl();
        }
        // `enum` is global-scope-only in the real compiler; the tooling parser
        // still parses a misplaced one so N7 can note it.
        if (check(TokenType::ENUM)) {
            return enumDecl();
        }
        return statement();
    }

    StmtPtr funDecl() {
        Token kw = advance(); // fun
        auto node = std::make_unique<FunDecl>();
        node->offset = kw.offset;
        if (check(TokenType::IDENTIFIER)) {
            node->name = nameFrom(advance());
        } else {
            error();
            spanTo(*node, kw.offset);
            return node;
        }
        parseParams(node->params);
        parseBlockInto(node->body);
        spanTo(*node, kw.offset);
        return node;
    }

    void parseParams(std::vector<Param>& out) {
        if (!consume(TokenType::LEFT_PAREN)) {
            return;
        }
        if (!check(TokenType::RIGHT_PAREN)) {
            do {
                if (check(TokenType::RIGHT_PAREN)) {
                    break; // tolerate a trailing comma
                }
                if (check(TokenType::IDENTIFIER)) {
                    out.push_back(paramFrom(advance()));
                } else {
                    error();
                    break;
                }
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(TokenType::RIGHT_PAREN);
    }

    // `{` declaration* `}` -- collects into `out`, recovering per statement.
    void parseBlockInto(std::vector<StmtPtr>& out) {
        if (!consume(TokenType::LEFT_BRACE)) {
            return;
        }
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
            std::size_t before = m_pos;
            StmtPtr stmt = declaration();
            if (stmt) {
                out.push_back(std::move(stmt));
            }
            if (m_pos == before) {
                advance();
            }
        }
        consume(TokenType::RIGHT_BRACE);
    }

    StmtPtr classDecl() {
        Token kw = advance(); // class
        auto node = std::make_unique<ClassDecl>();
        node->offset = kw.offset;
        if (check(TokenType::IDENTIFIER)) {
            node->name = nameFrom(advance());
        } else {
            error();
            spanTo(*node, kw.offset);
            return node;
        }
        if (match(TokenType::LESS)) {
            if (check(TokenType::IDENTIFIER)) {
                node->superclass = nameFrom(advance());
            } else {
                error();
            }
        }
        if (consume(TokenType::LEFT_BRACE)) {
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                std::size_t before = m_pos;
                if (check(TokenType::IDENTIFIER)) {
                    node->methods.push_back(method());
                } else {
                    error();
                }
                if (m_panic) {
                    synchronize();
                }
                if (m_pos == before) {
                    advance();
                }
            }
            consume(TokenType::RIGHT_BRACE);
        }
        spanTo(*node, kw.offset);
        return node;
    }

    MethodDecl method() {
        Token nameTok = advance(); // IDENTIFIER
        MethodDecl m;
        m.offset = nameTok.offset;
        m.name = nameFrom(nameTok);
        parseParams(m.params);
        parseBlockInto(m.body);
        m.length = endOfPrevious() > nameTok.offset
                       ? endOfPrevious() - nameTok.offset
                       : 0;
        return m;
    }

    StmtPtr enumDecl() {
        Token kw = advance(); // enum
        auto node = std::make_unique<EnumDecl>();
        node->offset = kw.offset;
        if (check(TokenType::IDENTIFIER)) {
            node->name = nameFrom(advance());
        } else {
            error();
            spanTo(*node, kw.offset);
            return node;
        }
        if (consume(TokenType::LEFT_BRACE)) {
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd() && !m_panic) {
                std::size_t before = m_pos;
                if (check(TokenType::IDENTIFIER)) {
                    node->ctors.push_back(enumCtor());
                } else {
                    error();
                }
                if (m_pos == before) {
                    advance();
                }
            }
            consume(TokenType::RIGHT_BRACE);
        }
        spanTo(*node, kw.offset);
        return node;
    }

    EnumCtorDecl enumCtor() {
        Token nameTok = advance(); // IDENTIFIER
        EnumCtorDecl ctor;
        ctor.offset = nameTok.offset;
        ctor.name = nameFrom(nameTok);
        if (match(TokenType::LEFT_PAREN)) {
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    if (check(TokenType::RIGHT_PAREN)) {
                        break;
                    }
                    if (check(TokenType::IDENTIFIER)) {
                        ctor.fields.push_back(paramFrom(advance()));
                    } else {
                        error();
                        break;
                    }
                } while (match(TokenType::COMMA) && !m_panic);
            }
            consume(TokenType::RIGHT_PAREN);
        }
        ctor.length = endOfPrevious() > nameTok.offset
                          ? endOfPrevious() - nameTok.offset
                          : 0;
        return ctor;
    }

    StmtPtr varDecl() {
        Token kw = advance(); // var
        if (check(TokenType::LEFT_BRACE) || check(TokenType::LEFT_BRACKET)) {
            return destructureDecl(kw);
        }
        auto node = std::make_unique<VarDecl>();
        node->offset = kw.offset;
        if (check(TokenType::IDENTIFIER)) {
            node->name = nameFrom(advance());
        } else {
            error();
            spanTo(*node, kw.offset);
            return node;
        }
        if (match(TokenType::EQUAL)) {
            node->initializer = expression();
        }
        consume(TokenType::SEMICOLON);
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr destructureDecl(const Token& kw) {
        bool seq = check(TokenType::LEFT_BRACKET);
        advance(); // `{` or `[`
        auto node = std::make_unique<DestructureDecl>();
        node->is_sequence = seq;
        node->offset = kw.offset;
        TokenType close =
            seq ? TokenType::RIGHT_BRACKET : TokenType::RIGHT_BRACE;
        if (!check(close)) {
            do {
                if (check(close)) {
                    break; // trailing comma is allowed
                }
                if (check(TokenType::IDENTIFIER)) {
                    node->targets.push_back(nameFrom(advance()));
                } else {
                    error();
                    break;
                }
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(close);
        consume(TokenType::EQUAL);
        node->initializer = expression();
        consume(TokenType::SEMICOLON);
        spanTo(*node, kw.offset);
        return node;
    }

    // -- statements ------------------------------------------------------

    StmtPtr statement() {
        NestingGuard guard(*this);
        if (tooDeep()) {
            error();
            return nullptr;
        }
        switch (peek().type) {
        case TokenType::FOR:
            return forStatement();
        case TokenType::IF:
            return ifStatement();
        case TokenType::WHILE:
            return whileStatement();
        case TokenType::PRINT:
            return printStatement();
        case TokenType::RETURN:
            return returnStatement();
        case TokenType::BREAK:
        case TokenType::CONTINUE:
            return breakOrContinue();
        case TokenType::LEFT_BRACE:
            return blockStatement();
        default:
            return exprStatement();
        }
    }

    StmtPtr blockStatement() {
        Token brace = peek();
        auto node = std::make_unique<Block>();
        parseBlockInto(node->body);
        spanTo(*node, brace.offset);
        return node;
    }

    StmtPtr breakOrContinue() {
        Token kw = advance();
        consume(TokenType::SEMICOLON);
        StmtPtr node;
        if (kw.type == TokenType::BREAK) {
            node = std::make_unique<BreakStmt>();
        } else {
            node = std::make_unique<ContinueStmt>();
        }
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr printStatement() {
        Token kw = advance();
        auto node = std::make_unique<PrintStmt>();
        node->value = expression();
        consume(TokenType::SEMICOLON);
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr returnStatement() {
        Token kw = advance();
        auto node = std::make_unique<ReturnStmt>();
        if (!check(TokenType::SEMICOLON)) {
            node->value = expression();
        }
        consume(TokenType::SEMICOLON);
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr ifStatement() {
        Token kw = advance();
        auto node = std::make_unique<IfStmt>();
        consume(TokenType::LEFT_PAREN);
        node->condition = expression();
        consume(TokenType::RIGHT_PAREN);
        node->then_branch = statement();
        if (match(TokenType::ELSE)) {
            node->else_branch = statement();
        }
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr whileStatement() {
        Token kw = advance();
        auto node = std::make_unique<WhileStmt>();
        consume(TokenType::LEFT_PAREN);
        node->condition = expression();
        consume(TokenType::RIGHT_PAREN);
        node->body = statement();
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr forStatement() {
        Token kw = advance();
        consume(TokenType::LEFT_PAREN);

        if (check(TokenType::VAR) && peekAt(1).type == TokenType::IDENTIFIER &&
            peekAt(2).type == TokenType::IN) {
            advance(); // var
            Token loopVar = advance();
            advance(); // in
            auto node = std::make_unique<ForInStmt>();
            node->variable = nameFrom(loopVar);
            node->iterable = expression();
            consume(TokenType::RIGHT_PAREN);
            node->body = statement();
            spanTo(*node, kw.offset);
            return node;
        }

        auto node = std::make_unique<ForStmt>();
        if (match(TokenType::SEMICOLON)) {
            // no initializer
        } else if (check(TokenType::VAR)) {
            node->initializer = varDecl(); // consumes the `;`
        } else {
            node->initializer = exprStatement(); // consumes the `;`
        }
        if (!check(TokenType::SEMICOLON)) {
            node->condition = expression();
        }
        consume(TokenType::SEMICOLON);
        if (!check(TokenType::RIGHT_PAREN)) {
            node->increment = expression();
        }
        consume(TokenType::RIGHT_PAREN);
        node->body = statement();
        spanTo(*node, kw.offset);
        return node;
    }

    StmtPtr exprStatement() {
        std::size_t start = peek().offset;
        auto node = std::make_unique<ExprStmt>();
        node->expr = expression();
        consume(TokenType::SEMICOLON);
        spanTo(*node, start);
        return node;
    }

    // -- expressions ----------------------------------------------------

    ExprPtr expression() { return assignment(); }

    ExprPtr assignment() {
        ExprPtr left = logicOr();
        if (match(TokenType::EQUAL)) {
            Token eq = previous();
            ExprPtr value = assignment();
            auto node = std::make_unique<AssignExpr>();
            std::size_t start = left ? left->offset : eq.offset;
            node->target = std::move(left);
            node->value = std::move(value);
            node->offset = start;
            std::size_t end = node->value
                                  ? node->value->offset + node->value->length
                                  : eq.offset + eq.length;
            node->length = end > start ? end - start : 0;
            return node;
        }
        return left;
    }

    ExprPtr logicOr() {
        ExprPtr expr = logicAnd();
        while (check(TokenType::OR) && !m_panic) {
            Token op = advance();
            ExprPtr right = logicAnd();
            expr = makeLogical(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr logicAnd() {
        ExprPtr expr = equality();
        while (check(TokenType::AND) && !m_panic) {
            Token op = advance();
            ExprPtr right = equality();
            expr = makeLogical(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr makeLogical(ExprPtr left, const Token& op, ExprPtr right) {
        auto node = std::make_unique<LogicalExpr>();
        node->op = op.type;
        std::size_t start = left ? left->offset : op.offset;
        node->left = std::move(left);
        node->right = std::move(right);
        finishBinaryLike(*node, start);
        return node;
    }

    ExprPtr equality() {
        ExprPtr expr = comparison();
        while (
            (check(TokenType::BANG_EQUAL) || check(TokenType::EQUAL_EQUAL)) &&
            !m_panic) {
            Token op = advance();
            ExprPtr right = comparison();
            expr = makeBinary(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr comparison() {
        ExprPtr expr = term();
        while ((check(TokenType::GREATER) || check(TokenType::GREATER_EQUAL) ||
                check(TokenType::LESS) || check(TokenType::LESS_EQUAL) ||
                check(TokenType::IN)) &&
               !m_panic) {
            Token op = advance();
            ExprPtr right = term();
            expr = makeBinary(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr term() {
        ExprPtr expr = factor();
        while ((check(TokenType::MINUS) || check(TokenType::PLUS)) &&
               !m_panic) {
            Token op = advance();
            ExprPtr right = factor();
            expr = makeBinary(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr factor() {
        ExprPtr expr = unary();
        while ((check(TokenType::SLASH) || check(TokenType::STAR) ||
                check(TokenType::PERCENT)) &&
               !m_panic) {
            Token op = advance();
            ExprPtr right = unary();
            expr = makeBinary(std::move(expr), op, std::move(right));
        }
        return expr;
    }

    ExprPtr makeBinary(ExprPtr left, const Token& op, ExprPtr right) {
        auto node = std::make_unique<BinaryExpr>();
        node->op = op.type;
        std::size_t start = left ? left->offset : op.offset;
        node->left = std::move(left);
        node->right = std::move(right);
        finishBinaryLike(*node, start);
        return node;
    }

    void finishBinaryLike(Expr& node, std::size_t start) const {
        node.offset = start;
        std::size_t end = endOfPrevious();
        node.length = end > start ? end - start : 0;
    }

    // Every nested expression -- paren group, list or map element, call
    // argument, subscript, unary prefix, `=` right side, `match` subject --
    // passes through unary() once per nesting level, so a single guard here
    // bounds the whole expression grammar.
    ExprPtr unary() {
        NestingGuard guard(*this);
        if (tooDeep()) {
            error();
            return nullptr;
        }
        if ((check(TokenType::BANG) || check(TokenType::MINUS)) && !m_panic) {
            Token op = advance();
            auto node = std::make_unique<UnaryExpr>();
            node->op = op.type;
            node->operand = unary();
            spanTo(*node, op.offset);
            return node;
        }
        return call();
    }

    ExprPtr call() {
        ExprPtr expr = primary();
        std::size_t start = expr ? expr->offset : peek().offset;
        for (;;) {
            if (m_panic) {
                break;
            }
            if (match(TokenType::LEFT_PAREN)) {
                expr = finishCall(std::move(expr), start);
            } else if (match(TokenType::DOT)) {
                if (!check(TokenType::IDENTIFIER)) {
                    error();
                    break;
                }
                Token nameTok = advance();
                auto node = std::make_unique<GetExpr>();
                node->object = std::move(expr);
                node->name = tokenText(nameTok);
                node->name_offset = nameTok.offset;
                node->name_length = nameTok.length;
                spanTo(*node, start);
                expr = std::move(node);
            } else if (match(TokenType::LEFT_BRACKET)) {
                expr = finishSubscript(std::move(expr), start);
            } else {
                break;
            }
        }
        return expr;
    }

    ExprPtr finishCall(ExprPtr callee, std::size_t start) {
        auto node = std::make_unique<CallExpr>();
        node->callee = std::move(callee);
        if (!check(TokenType::RIGHT_PAREN)) {
            do {
                if (check(TokenType::RIGHT_PAREN)) {
                    break;
                }
                node->arguments.push_back(expression());
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(TokenType::RIGHT_PAREN);
        spanTo(*node, start);
        return node;
    }

    ExprPtr finishSubscript(ExprPtr object, std::size_t start) {
        ExprPtr first = expression();
        if (match(TokenType::COLON)) {
            ExprPtr second = expression();
            consume(TokenType::RIGHT_BRACKET);
            auto node = std::make_unique<SliceExpr>();
            node->object = std::move(object);
            node->start = std::move(first);
            node->end = std::move(second);
            spanTo(*node, start);
            return node;
        }
        consume(TokenType::RIGHT_BRACKET);
        auto node = std::make_unique<IndexExpr>();
        node->object = std::move(object);
        node->index = std::move(first);
        spanTo(*node, start);
        return node;
    }

    ExprPtr primary() {
        if (m_panic) {
            return nullptr;
        }
        Token t = peek();
        if (isLiteralToken(t.type)) {
            advance();
            auto node = std::make_unique<LiteralExpr>();
            node->literal_kind = literalKindOf(t.type);
            node->lexeme = tokenText(t);
            node->offset = t.offset;
            node->length = t.length;
            return node;
        }
        switch (t.type) {
        case TokenType::IDENTIFIER: {
            advance();
            auto node = std::make_unique<IdentifierExpr>();
            node->name = tokenText(t);
            node->role = IdentRole::Reference;
            node->offset = t.offset;
            node->length = t.length;
            return node;
        }
        case TokenType::THIS: {
            advance();
            auto node = std::make_unique<ThisExpr>();
            node->offset = t.offset;
            node->length = t.length;
            return node;
        }
        case TokenType::SUPER: {
            advance();
            auto node = std::make_unique<SuperExpr>();
            node->offset = t.offset;
            if (consume(TokenType::DOT) && check(TokenType::IDENTIFIER)) {
                Token nameTok = advance();
                node->name = tokenText(nameTok);
                node->name_offset = nameTok.offset;
                node->name_length = nameTok.length;
            }
            spanTo(*node, t.offset);
            return node;
        }
        case TokenType::LEFT_PAREN: {
            advance();
            auto node = std::make_unique<GroupingExpr>();
            node->inner = expression();
            consume(TokenType::RIGHT_PAREN);
            spanTo(*node, t.offset);
            return node;
        }
        case TokenType::LEFT_BRACKET:
            return listLiteral();
        case TokenType::LEFT_BRACE:
            return mapLiteral();
        case TokenType::MATCH:
            return matchExpr();
        default:
            error();
            return nullptr;
        }
    }

    ExprPtr listLiteral() {
        Token open = advance(); // [
        auto node = std::make_unique<ListLiteralExpr>();
        if (!check(TokenType::RIGHT_BRACKET)) {
            do {
                if (check(TokenType::RIGHT_BRACKET)) {
                    break;
                }
                node->elements.push_back(expression());
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(TokenType::RIGHT_BRACKET);
        spanTo(*node, open.offset);
        return node;
    }

    ExprPtr mapLiteral() {
        Token open = advance(); // {
        auto node = std::make_unique<MapLiteralExpr>();
        if (!check(TokenType::RIGHT_BRACE)) {
            do {
                if (check(TokenType::RIGHT_BRACE)) {
                    break;
                }
                MapEntry entry;
                entry.key = expression();
                consume(TokenType::COLON);
                entry.value = expression();
                node->entries.push_back(std::move(entry));
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(TokenType::RIGHT_BRACE);
        spanTo(*node, open.offset);
        return node;
    }

    ExprPtr matchExpr() {
        Token kw = advance(); // match
        auto node = std::make_unique<MatchExpr>();
        node->subject = expression();
        if (consume(TokenType::LEFT_BRACE)) {
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                std::size_t before = m_pos;
                if (check(TokenType::CASE)) {
                    node->arms.push_back(matchArm());
                } else {
                    error();
                }
                if (m_panic) {
                    m_panic = false;
                    recoverToArmBoundary();
                }
                if (m_pos == before) {
                    advance();
                }
            }
            consume(TokenType::RIGHT_BRACE);
        }
        spanTo(*node, kw.offset);
        return node;
    }

    void recoverToArmBoundary() {
        while (!isAtEnd() && !check(TokenType::CASE) &&
               !check(TokenType::RIGHT_BRACE)) {
            advance();
        }
    }

    MatchArm matchArm() {
        Token kw = advance(); // case
        MatchArm arm;
        arm.offset = kw.offset;
        parseArmPatterns(arm);
        if (match(TokenType::IF)) {
            arm.guard = expression();
        }
        consume(TokenType::FAT_ARROW);
        parseArmBody(arm);
        arm.length =
            endOfPrevious() > kw.offset ? endOfPrevious() - kw.offset : 0;
        return arm;
    }

    void parseArmPatterns(MatchArm& arm) {
        if (isLiteralToken(peek().type)) {
            arm.patterns.push_back(literalPattern());
            while (match(TokenType::COMMA) && !m_panic) {
                arm.patterns.push_back(literalPattern());
            }
            return;
        }
        PatternPtr first = pattern();
        if (!check(TokenType::OR)) {
            arm.patterns.push_back(std::move(first));
            return;
        }
        auto orPat = std::make_unique<OrPat>();
        std::size_t start = first ? first->offset : peek().offset;
        orPat->alternatives.push_back(std::move(first));
        while (match(TokenType::OR) && !m_panic) {
            orPat->alternatives.push_back(pattern());
        }
        spanTo(*orPat, start);
        arm.patterns.push_back(std::move(orPat));
    }

    PatternPtr literalPattern() {
        if (m_panic || !isLiteralToken(peek().type)) {
            error();
            return nullptr;
        }
        Token t = advance();
        auto node = std::make_unique<LiteralPat>();
        node->literal_kind = literalKindOf(t.type);
        node->lexeme = tokenText(t);
        node->offset = t.offset;
        node->length = t.length;
        return node;
    }

    PatternPtr pattern() {
        if (m_panic) {
            return nullptr;
        }
        Token t = peek();
        if (t.type == TokenType::LEFT_BRACKET) {
            return seqPattern();
        }
        if (isLiteralToken(t.type)) {
            return literalPattern();
        }
        if (t.type != TokenType::IDENTIFIER) {
            error();
            return nullptr;
        }
        Token nameTok = advance();
        if (match(TokenType::AT)) {
            auto node = std::make_unique<AtBindingPat>();
            node->name = tokenText(nameTok);
            node->name_offset = nameTok.offset;
            node->name_length = nameTok.length;
            node->sub = subPattern();
            spanTo(*node, nameTok.offset);
            return node;
        }
        if (check(TokenType::LEFT_BRACE)) {
            return classPattern(nameTok);
        }
        if (check(TokenType::LEFT_PAREN)) {
            return ctorPattern(nameTok);
        }
        if (tokenText(nameTok) == "_") {
            auto node = std::make_unique<WildcardPat>();
            node->offset = nameTok.offset;
            node->length = nameTok.length;
            return node;
        }
        auto node = std::make_unique<BindingPat>();
        node->name = tokenText(nameTok);
        node->name_offset = nameTok.offset;
        node->name_length = nameTok.length;
        node->offset = nameTok.offset;
        node->length = nameTok.length;
        return node;
    }

    PatternPtr subPattern() {
        if (m_panic) {
            return nullptr;
        }
        if (check(TokenType::LEFT_BRACKET)) {
            return seqPattern();
        }
        if (!check(TokenType::IDENTIFIER)) {
            error();
            return nullptr;
        }
        Token nameTok = advance();
        if (check(TokenType::LEFT_BRACE)) {
            return classPattern(nameTok);
        }
        if (check(TokenType::LEFT_PAREN)) {
            return ctorPattern(nameTok);
        }
        // A bare identifier here is a zero-field constructor or a class type
        // check; recorded as a BindingPat for N7 to classify.
        auto node = std::make_unique<BindingPat>();
        node->name = tokenText(nameTok);
        node->name_offset = nameTok.offset;
        node->name_length = nameTok.length;
        node->offset = nameTok.offset;
        node->length = nameTok.length;
        return node;
    }

    PatternPtr classPattern(const Token& nameTok) {
        advance(); // {
        auto node = std::make_unique<ClassPat>();
        node->name = tokenText(nameTok);
        node->name_offset = nameTok.offset;
        node->name_length = nameTok.length;
        parsePatternFieldList(node->fields, TokenType::RIGHT_BRACE);
        spanTo(*node, nameTok.offset);
        return node;
    }

    PatternPtr ctorPattern(const Token& nameTok) {
        advance(); // (
        auto node = std::make_unique<CtorPat>();
        node->name = tokenText(nameTok);
        node->name_offset = nameTok.offset;
        node->name_length = nameTok.length;
        parsePatternFieldList(node->fields, TokenType::RIGHT_PAREN);
        spanTo(*node, nameTok.offset);
        return node;
    }

    void parsePatternFieldList(std::vector<Name>& out, TokenType close) {
        if (!check(close)) {
            do {
                if (check(close)) {
                    break;
                }
                if (check(TokenType::IDENTIFIER)) {
                    out.push_back(nameFrom(advance()));
                } else {
                    error();
                    break;
                }
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(close);
    }

    PatternPtr seqPattern() {
        Token open = advance(); // [
        auto node = std::make_unique<SeqPat>();
        if (!check(TokenType::RIGHT_BRACKET)) {
            do {
                if (check(TokenType::RIGHT_BRACKET)) {
                    break;
                }
                SeqPatElem elem;
                std::size_t elemStart = peek().offset;
                if (match(TokenType::ELIPSIS)) {
                    elem.is_rest = true;
                }
                if (check(TokenType::IDENTIFIER)) {
                    Token id = advance();
                    elem.name = tokenText(id);
                    elem.offset = elem.is_rest ? elemStart : id.offset;
                    elem.length = id.offset + id.length - elem.offset;
                    node->elements.push_back(elem);
                } else {
                    error();
                    break;
                }
            } while (match(TokenType::COMMA) && !m_panic);
        }
        consume(TokenType::RIGHT_BRACKET);
        spanTo(*node, open.offset);
        return node;
    }

    void parseArmBody(MatchArm& arm) {
        if (m_panic) {
            return;
        }
        if (!match(TokenType::LEFT_BRACE)) {
            arm.body_expr = expression();
            return;
        }
        // Block form: `{` declaration* expression `}`. The block ends in an
        // expression, not a statement.
        while (!check(TokenType::RIGHT_BRACE) && !isAtEnd() && !m_panic) {
            std::size_t before = m_pos;
            if (startsStatement(peek().type)) {
                StmtPtr stmt = declaration();
                if (stmt) {
                    arm.body_decls.push_back(std::move(stmt));
                }
            } else {
                std::size_t exprStart = peek().offset;
                ExprPtr expr = expression();
                if (match(TokenType::SEMICOLON)) {
                    auto stmt = std::make_unique<ExprStmt>();
                    stmt->expr = std::move(expr);
                    spanTo(*stmt, exprStart);
                    arm.body_decls.push_back(std::move(stmt));
                } else {
                    arm.body_expr = std::move(expr);
                    break;
                }
            }
            if (m_pos == before) {
                advance();
            }
        }
        consume(TokenType::RIGHT_BRACE);
    }

    const std::string& m_source;
    std::vector<Token> m_tokens;
    std::size_t m_pos = 0;
    int m_depth = 0;
    bool m_panic = false;
};

} // namespace

Program parse(const std::string& source) {
    Parser parser(source);
    return parser.parseProgram();
}

} // namespace loxpp::tooling
