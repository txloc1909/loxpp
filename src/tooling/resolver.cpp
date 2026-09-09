#include "tooling/resolver.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "tooling/stdlib_names.h"

namespace loxpp::tooling {

// ---------------------------------------------------------------------------
// ReferenceIndex
// ---------------------------------------------------------------------------

void ReferenceIndex::add(Entry entry) {
    if (entry.span.length == 0) {
        return;
    }
    if (entry.symbol == nullptr && !entry.knownGlobal) {
        return;
    }
    m_entries.push_back(entry);
}

void ReferenceIndex::finish() {
    std::ranges::sort(m_entries, {},
                      [](const Entry& e) { return e.span.offset; });
}

const ReferenceIndex::Entry* ReferenceIndex::at(std::size_t offset) const {
    // Spans never overlap, so the last entry whose start is <= offset is the
    // only candidate.
    auto it = std::ranges::upper_bound(
        m_entries, offset, {}, [](const Entry& e) { return e.span.offset; });
    if (it == m_entries.begin()) {
        return nullptr;
    }
    --it;
    return it->span.contains(offset) ? &*it : nullptr;
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

namespace {

Span nameSpan(const Name& n) { return {n.offset, n.length}; }
Span exprSpan(const Expr& e) { return {e.offset, e.length}; }
Span stmtSpan(const Stmt& s) { return {s.offset, s.length}; }

class Resolver {
  public:
    explicit Resolver(ResolvedDocument& out)
        : m_out(out), m_current(out.symbols.global()) {}

    void run(const Program& program) {
        m_out.symbols.global()->span = {program.offset, program.length};
        collectGlobals(program);
        for (const auto& stmt : program.body) {
            visitStmt(stmt.get());
        }
        reportUnused(m_out.symbols.global());
        m_out.references.finish();
    }

  private:
    ResolvedDocument& m_out;
    Scope* m_current;
    int m_functionDepth = 0;
    int m_loopDepth = 0;
    int m_matchDepth = 0;

    struct PendingVar {
        std::string_view name;
        Scope* scope = nullptr;
        bool active = false;
    } m_pendingVar;

    // -- diagnostics -------------------------------------------------------

    void warn(Span span, std::string message) {
        Diagnostic d;
        d.offset = span.offset;
        d.length = span.length;
        d.severity = Severity::Warning;
        d.message = std::move(message);
        m_out.diagnostics.push_back(std::move(d));
    }

    // -- scopes ----------------------------------------------------------

    Scope* push(ScopeKind kind, Span span) {
        Scope* child = SymbolTable::addChild(m_current, kind, span);
        m_current = child;
        return child;
    }
    void pop() { m_current = m_current->parent; }

    Symbol* declareIn(Scope* scope, std::string_view name, SymbolKind kind,
                      Span decl, Span full) {
        if (scope->kind != ScopeKind::Global) {
            if (Symbol* prior = scope->findLocal(name)) {
                (void)prior;
                warn(decl, "redeclaration of '" + std::string(name) +
                               "' in the same scope");
            }
        }
        Symbol* sym = scope->declare(std::string(name), kind, decl);
        sym->fullRange = full;
        return sym;
    }

    static Symbol* lookup(std::string_view name, Scope* from) {
        for (Scope* s = from; s != nullptr; s = s->parent) {
            if (Symbol* sym = s->findLocal(name)) {
                return sym;
            }
        }
        return nullptr;
    }

    static bool isConstructorSymbol(const Symbol* sym) {
        return sym != nullptr && (sym->kind == SymbolKind::EnumCtor ||
                                  sym->kind == SymbolKind::Class);
    }

    // -- first sweep: every top-level declaration ------------------------

    void collectGlobals(const Program& program) {
        Scope* g = m_out.symbols.global();
        for (const auto& stmtPtr : program.body) {
            const Stmt* stmt = stmtPtr.get();
            switch (stmt->kind) {
            case StmtKind::VarDecl: {
                const auto& v = static_cast<const VarDecl&>(*stmt);
                declareIn(g, v.name.text, SymbolKind::Var, nameSpan(v.name),
                          stmtSpan(v));
                break;
            }
            case StmtKind::DestructureDecl: {
                const auto& d = static_cast<const DestructureDecl&>(*stmt);
                for (const Name& t : d.targets) {
                    if (t.text == "_") {
                        continue;
                    }
                    declareIn(g, t.text, SymbolKind::Var, nameSpan(t),
                              nameSpan(t));
                }
                break;
            }
            case StmtKind::FunDecl: {
                const auto& f = static_cast<const FunDecl&>(*stmt);
                declareIn(g, f.name.text, SymbolKind::Function,
                          nameSpan(f.name), stmtSpan(f));
                break;
            }
            case StmtKind::ClassDecl: {
                const auto& c = static_cast<const ClassDecl&>(*stmt);
                declareIn(g, c.name.text, SymbolKind::Class, nameSpan(c.name),
                          stmtSpan(c));
                break;
            }
            case StmtKind::EnumDecl: {
                const auto& e = static_cast<const EnumDecl&>(*stmt);
                Symbol* enumSym = declareIn(g, e.name.text, SymbolKind::Class,
                                            nameSpan(e.name), stmtSpan(e));
                for (const EnumCtorDecl& ctor : e.ctors) {
                    Symbol* cs = declareIn(
                        g, ctor.name.text, SymbolKind::EnumCtor,
                        nameSpan(ctor.name), {ctor.offset, ctor.length});
                    enumSym->members.push_back(cs);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    // -- reference resolution ------------------------------------------

    void resolveReference(std::string_view name, Span span) {
        if (m_pendingVar.active && name == m_pendingVar.name &&
            scopeWithin(m_current, m_pendingVar.scope)) {
            warn(span,
                 "'" + std::string(name) + "' is used in its own initializer");
            return;
        }
        if (Symbol* sym = lookup(name, m_current)) {
            sym->uses.push_back(span);
            m_out.references.add({span, sym, false, false});
            return;
        }
        if (isStdlibGlobal(name)) {
            m_out.references.add({span, nullptr, false, true});
            return;
        }
        warn(span, "unknown name '" + std::string(name) + "'");
    }

    static bool scopeWithin(const Scope* inner, const Scope* outer) {
        for (const Scope* s = inner; s != nullptr; s = s->parent) {
            if (s == outer) {
                return true;
            }
        }
        return false;
    }

    void resolveImplicit(std::string_view name, Span span,
                         const std::string& diagnostic) {
        if (Symbol* sym = lookup(name, m_current)) {
            sym->uses.push_back(span);
            m_out.references.add({span, sym, false, false});
            return;
        }
        warn(span, diagnostic);
    }

    // -- statements ---------------------------------------------------

    void visitBody(const std::vector<StmtPtr>& body) {
        for (const auto& s : body) {
            visitStmt(s.get());
        }
    }

    void visitStmt(const Stmt* stmt) {
        if (stmt == nullptr) {
            return;
        }
        switch (stmt->kind) {
        case StmtKind::VarDecl:
            visitVarDecl(static_cast<const VarDecl&>(*stmt));
            break;
        case StmtKind::DestructureDecl:
            visitDestructure(static_cast<const DestructureDecl&>(*stmt));
            break;
        case StmtKind::FunDecl:
            visitFunDecl(static_cast<const FunDecl&>(*stmt));
            break;
        case StmtKind::ClassDecl:
            visitClassDecl(static_cast<const ClassDecl&>(*stmt));
            break;
        case StmtKind::EnumDecl:
            visitEnumDecl(static_cast<const EnumDecl&>(*stmt));
            break;
        case StmtKind::Block: {
            const auto& b = static_cast<const Block&>(*stmt);
            push(ScopeKind::Block, stmtSpan(b));
            visitBody(b.body);
            pop();
            break;
        }
        case StmtKind::If: {
            const auto& s = static_cast<const IfStmt&>(*stmt);
            visitExpr(s.condition.get());
            visitStmt(s.then_branch.get());
            visitStmt(s.else_branch.get());
            break;
        }
        case StmtKind::While: {
            const auto& s = static_cast<const WhileStmt&>(*stmt);
            visitExpr(s.condition.get());
            ++m_loopDepth;
            visitStmt(s.body.get());
            --m_loopDepth;
            break;
        }
        case StmtKind::For:
            visitFor(static_cast<const ForStmt&>(*stmt));
            break;
        case StmtKind::ForIn:
            visitForIn(static_cast<const ForInStmt&>(*stmt));
            break;
        case StmtKind::Print:
            visitExpr(static_cast<const PrintStmt&>(*stmt).value.get());
            break;
        case StmtKind::Return: {
            const auto& s = static_cast<const ReturnStmt&>(*stmt);
            if (m_functionDepth == 0) {
                warn(stmtSpan(s), "'return' outside a function");
            }
            visitExpr(s.value.get());
            break;
        }
        case StmtKind::Break: {
            if (m_loopDepth == 0 && m_matchDepth == 0) {
                warn(stmtSpan(*stmt), "'break' outside a loop or match");
            }
            break;
        }
        case StmtKind::Continue: {
            if (m_loopDepth == 0) {
                warn(stmtSpan(*stmt), "'continue' outside a loop");
            }
            break;
        }
        case StmtKind::ExprStmt:
            visitExpr(static_cast<const ExprStmt&>(*stmt).expr.get());
            break;
        }
    }

    void visitVarDecl(const VarDecl& v) {
        if (v.initializer) {
            PendingVar saved = m_pendingVar;
            m_pendingVar = {
                .name = v.name.text, .scope = m_current, .active = true};
            visitExpr(v.initializer.get());
            m_pendingVar = saved;
        }
        if (m_current->kind != ScopeKind::Global) {
            declareIn(m_current, v.name.text, SymbolKind::Var, nameSpan(v.name),
                      stmtSpan(v));
        }
        m_out.references.add(
            {nameSpan(v.name), lookup(v.name.text, m_current), true, false});
    }

    void visitDestructure(const DestructureDecl& d) {
        if (d.initializer) {
            visitExpr(d.initializer.get());
        }
        for (const Name& t : d.targets) {
            if (t.text == "_") {
                continue;
            }
            if (m_current->kind != ScopeKind::Global) {
                declareIn(m_current, t.text, SymbolKind::Var, nameSpan(t),
                          nameSpan(t));
            }
            m_out.references.add(
                {nameSpan(t), lookup(t.text, m_current), true, false});
        }
    }

    void visitFunDecl(const FunDecl& f) {
        Symbol* sym = nullptr;
        if (m_current->kind == ScopeKind::Global) {
            sym = m_current->findLocal(f.name.text);
        } else {
            sym = declareIn(m_current, f.name.text, SymbolKind::Function,
                            nameSpan(f.name), stmtSpan(f));
        }
        if (sym != nullptr) {
            m_out.references.add({nameSpan(f.name), sym, true, false});
        }
        Scope* fnScope = push(ScopeKind::Function, stmtSpan(f));
        if (sym != nullptr) {
            sym->innerScope = fnScope;
        }
        enterFunction();
        for (const Param& p : f.params) {
            declareIn(fnScope, p.name, SymbolKind::Param, {p.offset, p.length},
                      {p.offset, p.length});
            m_out.references.add({{p.offset, p.length},
                                  fnScope->findLocal(p.name),
                                  true,
                                  false});
        }
        visitBody(f.body);
        leaveFunction();
        pop();
    }

    void visitClassDecl(const ClassDecl& c) {
        Symbol* sym = nullptr;
        if (m_current->kind == ScopeKind::Global) {
            sym = m_current->findLocal(c.name.text);
        } else {
            sym = declareIn(m_current, c.name.text, SymbolKind::Class,
                            nameSpan(c.name), stmtSpan(c));
        }
        if (sym != nullptr) {
            m_out.references.add({nameSpan(c.name), sym, true, false});
        }
        bool hasSuper = c.superclass.has_value();
        if (hasSuper) {
            resolveReference(c.superclass->text, nameSpan(*c.superclass));
        }

        Scope* memberScope = push(ScopeKind::Block, stmtSpan(c));
        if (sym != nullptr) {
            sym->innerScope = memberScope;
        }
        for (const MethodDecl& m : c.methods) {
            Symbol* methodSym =
                declareIn(memberScope, m.name.text, SymbolKind::Method,
                          nameSpan(m.name), {m.offset, m.length});
            m_out.references.add({nameSpan(m.name), methodSym, true, false});

            // The method body scope hangs off the scope that encloses the
            // class, not off memberScope: methods are not visible as bare
            // names, so memberScope must stay off every lookup parent-chain.
            Scope* saved = m_current;
            m_current = memberScope->parent;
            Scope* fnScope = push(ScopeKind::Function, {m.offset, m.length});
            methodSym->innerScope = fnScope;
            enterFunction();

            Symbol* thisSym =
                fnScope->declare("this", SymbolKind::Param, nameSpan(m.name));
            thisSym->implicit = true;
            if (hasSuper) {
                Symbol* superSym = fnScope->declare("super", SymbolKind::Param,
                                                    nameSpan(m.name));
                superSym->implicit = true;
            }
            for (const Param& p : m.params) {
                declareIn(fnScope, p.name, SymbolKind::Param,
                          {p.offset, p.length}, {p.offset, p.length});
                m_out.references.add({{p.offset, p.length},
                                      fnScope->findLocal(p.name),
                                      true,
                                      false});
            }
            visitBody(m.body);
            leaveFunction();
            pop(); // fnScope
            m_current = saved;
        }
        pop(); // memberScope
    }

    void visitEnumDecl(const EnumDecl& e) {
        if (m_current->kind != ScopeKind::Global) {
            warn(nameSpan(e.name), "enum must be declared at global scope");
            // The misplaced enum is already diagnosed here; mark its symbols
            // implicit so the unused-local pass does not add a second warning
            // for the same construct.
            Symbol* enumSym =
                declareIn(m_current, e.name.text, SymbolKind::Class,
                          nameSpan(e.name), stmtSpan(e));
            enumSym->implicit = true;
            for (const EnumCtorDecl& ctor : e.ctors) {
                Symbol* cs =
                    declareIn(m_current, ctor.name.text, SymbolKind::EnumCtor,
                              nameSpan(ctor.name), {ctor.offset, ctor.length});
                cs->implicit = true;
                enumSym->members.push_back(cs);
            }
            return;
        }
        Symbol* enumSym = m_current->findLocal(e.name.text);
        if (enumSym != nullptr) {
            m_out.references.add({nameSpan(e.name), enumSym, true, false});
        }
        for (const EnumCtorDecl& ctor : e.ctors) {
            Symbol* cs = m_current->findLocal(ctor.name.text);
            if (cs != nullptr) {
                m_out.references.add({nameSpan(ctor.name), cs, true, false});
            }
        }
    }

    void visitFor(const ForStmt& s) {
        push(ScopeKind::ForHeader, stmtSpan(s));
        if (s.initializer) {
            if (s.initializer->kind == StmtKind::VarDecl) {
                const auto& v = static_cast<const VarDecl&>(*s.initializer);
                if (v.initializer) {
                    visitExpr(v.initializer.get());
                }
                declareIn(m_current, v.name.text, SymbolKind::LoopVar,
                          nameSpan(v.name), stmtSpan(v));
                m_out.references.add({nameSpan(v.name),
                                      m_current->findLocal(v.name.text), true,
                                      false});
            } else {
                visitStmt(s.initializer.get());
            }
        }
        visitExpr(s.condition.get());
        visitExpr(s.increment.get());
        ++m_loopDepth;
        visitStmt(s.body.get());
        --m_loopDepth;
        pop();
    }

    void visitForIn(const ForInStmt& s) {
        push(ScopeKind::ForHeader, stmtSpan(s));
        // The iterable is evaluated in the enclosing scope; resolve it before
        // the loop variable is declared so `for (var x in x)` cannot bind to
        // itself.
        visitExpr(s.iterable.get());
        declareIn(m_current, s.variable.text, SymbolKind::LoopVar,
                  nameSpan(s.variable), nameSpan(s.variable));
        m_out.references.add({nameSpan(s.variable),
                              m_current->findLocal(s.variable.text), true,
                              false});
        ++m_loopDepth;
        visitStmt(s.body.get());
        --m_loopDepth;
        pop();
    }

    void enterFunction() {
        ++m_functionDepth;
        m_savedLoop.push_back(m_loopDepth);
        m_savedMatch.push_back(m_matchDepth);
        m_loopDepth = 0;
        m_matchDepth = 0;
    }
    void leaveFunction() {
        --m_functionDepth;
        m_loopDepth = m_savedLoop.back();
        m_savedLoop.pop_back();
        m_matchDepth = m_savedMatch.back();
        m_savedMatch.pop_back();
    }
    std::vector<int> m_savedLoop;
    std::vector<int> m_savedMatch;

    // -- expressions ------------------------------------------------

    void visitExpr(const Expr* expr) {
        if (expr == nullptr) {
            return;
        }
        switch (expr->kind) {
        case ExprKind::Literal:
            break;
        case ExprKind::Identifier: {
            const auto& e = static_cast<const IdentifierExpr&>(*expr);
            resolveReference(e.name, exprSpan(e));
            break;
        }
        case ExprKind::Unary:
            visitExpr(static_cast<const UnaryExpr&>(*expr).operand.get());
            break;
        case ExprKind::Binary: {
            const auto& e = static_cast<const BinaryExpr&>(*expr);
            visitExpr(e.left.get());
            visitExpr(e.right.get());
            break;
        }
        case ExprKind::Logical: {
            const auto& e = static_cast<const LogicalExpr&>(*expr);
            visitExpr(e.left.get());
            visitExpr(e.right.get());
            break;
        }
        case ExprKind::Call: {
            const auto& e = static_cast<const CallExpr&>(*expr);
            visitExpr(e.callee.get());
            for (const auto& a : e.arguments) {
                visitExpr(a.get());
            }
            break;
        }
        case ExprKind::Get:
            // `obj.name` -- `name` is a member and is resolved dynamically at
            // run time; the tooling does not link it to a class member.
            // Only the object sub-expression is resolved.
            visitExpr(static_cast<const GetExpr&>(*expr).object.get());
            break;
        case ExprKind::Index: {
            const auto& e = static_cast<const IndexExpr&>(*expr);
            visitExpr(e.object.get());
            visitExpr(e.index.get());
            break;
        }
        case ExprKind::Slice: {
            const auto& e = static_cast<const SliceExpr&>(*expr);
            visitExpr(e.object.get());
            visitExpr(e.start.get());
            visitExpr(e.end.get());
            break;
        }
        case ExprKind::Assign: {
            const auto& e = static_cast<const AssignExpr&>(*expr);
            visitExpr(e.target.get());
            visitExpr(e.value.get());
            break;
        }
        case ExprKind::ListLiteral: {
            const auto& e = static_cast<const ListLiteralExpr&>(*expr);
            for (const auto& el : e.elements) {
                visitExpr(el.get());
            }
            break;
        }
        case ExprKind::MapLiteral: {
            const auto& e = static_cast<const MapLiteralExpr&>(*expr);
            for (const auto& entry : e.entries) {
                visitExpr(entry.key.get());
                visitExpr(entry.value.get());
            }
            break;
        }
        case ExprKind::Grouping:
            visitExpr(static_cast<const GroupingExpr&>(*expr).inner.get());
            break;
        case ExprKind::This: {
            resolveImplicit("this", exprSpan(*expr), "'this' outside a method");
            break;
        }
        case ExprKind::Super: {
            const auto& e = static_cast<const SuperExpr&>(*expr);
            resolveImplicit("super", {e.offset, 5},
                            "'super' outside a method of a subclass");
            break;
        }
        case ExprKind::Match:
            visitMatch(static_cast<const MatchExpr&>(*expr));
            break;
        }
    }

    void visitMatch(const MatchExpr& m) {
        visitExpr(m.subject.get());
        ++m_matchDepth;
        for (const MatchArm& arm : m.arms) {
            Scope* armScope =
                push(ScopeKind::MatchArm, {arm.offset, arm.length});
            for (const auto& pat : arm.patterns) {
                bindPattern(pat.get(), armScope);
            }
            visitExpr(arm.guard.get());
            visitBody(arm.body_decls);
            visitExpr(arm.body_expr.get());
            pop();
        }
        --m_matchDepth;
    }

    void declareMatchBinding(Scope* armScope, std::string_view name,
                             Span span) {
        if (name == "_") {
            return;
        }
        if (armScope->findLocal(name) != nullptr) {
            // Or-pattern alternatives bind the same name set; a repeat is the
            // same binding, not a redeclaration.
            return;
        }
        Symbol* sym = armScope->declare(std::string(name),
                                        SymbolKind::MatchBinding, span);
        sym->fullRange = span;
        m_out.references.add({span, sym, true, false});
    }

    void bindPatternHead(std::string_view name, Span span, Scope* armScope) {
        Symbol* sym = lookup(name, armScope);
        if (isConstructorSymbol(sym)) {
            sym->uses.push_back(span);
            m_out.references.add({span, sym, false, false});
            return;
        }
        declareMatchBinding(armScope, name, span);
    }

    void resolveCtorName(std::string_view name, Span span, Scope* armScope) {
        Symbol* sym = lookup(name, armScope);
        if (sym != nullptr) {
            sym->uses.push_back(span);
            m_out.references.add({span, sym, false, false});
            return;
        }
        if (isStdlibGlobal(name)) {
            m_out.references.add({span, nullptr, false, true});
            return;
        }
        warn(span, "unknown name '" + std::string(name) + "'");
    }

    void bindPattern(const Pattern* pat, Scope* armScope) {
        if (pat == nullptr) {
            return;
        }
        switch (pat->kind) {
        case PatternKind::Literal:
        case PatternKind::Wildcard:
            break;
        case PatternKind::Binding: {
            const auto& p = static_cast<const BindingPat&>(*pat);
            bindPatternHead(p.name, {p.name_offset, p.name_length}, armScope);
            break;
        }
        case PatternKind::Ctor: {
            const auto& p = static_cast<const CtorPat&>(*pat);
            resolveCtorName(p.name, {p.name_offset, p.name_length}, armScope);
            for (const Name& f : p.fields) {
                declareMatchBinding(armScope, f.text, nameSpan(f));
            }
            break;
        }
        case PatternKind::Class: {
            const auto& p = static_cast<const ClassPat&>(*pat);
            resolveCtorName(p.name, {p.name_offset, p.name_length}, armScope);
            for (const Name& f : p.fields) {
                declareMatchBinding(armScope, f.text, nameSpan(f));
            }
            break;
        }
        case PatternKind::Seq: {
            const auto& p = static_cast<const SeqPat&>(*pat);
            for (const SeqPatElem& el : p.elements) {
                declareMatchBinding(armScope, el.name, {el.offset, el.length});
            }
            break;
        }
        case PatternKind::AtBinding: {
            const auto& p = static_cast<const AtBindingPat&>(*pat);
            declareMatchBinding(armScope, p.name,
                                {p.name_offset, p.name_length});
            bindPattern(p.sub.get(), armScope);
            break;
        }
        case PatternKind::Or: {
            const auto& p = static_cast<const OrPat&>(*pat);
            for (const auto& alt : p.alternatives) {
                bindPattern(alt.get(), armScope);
            }
            checkOrPatternBindings(p, armScope);
            break;
        }
        }
    }

    // The names a single pattern would bind in the arm scope. A bare identifier
    // that names a constructor is a use, not a binding, so it is left out.
    void collectBindingNames(const Pattern* pat, Scope* armScope,
                             std::vector<std::string_view>& names) const {
        if (pat == nullptr) {
            return;
        }
        auto keep = [&](std::string_view name) {
            if (name != "_") {
                names.push_back(name);
            }
        };
        switch (pat->kind) {
        case PatternKind::Literal:
        case PatternKind::Wildcard:
            break;
        case PatternKind::Binding: {
            const auto& p = static_cast<const BindingPat&>(*pat);
            if (!isConstructorSymbol(lookup(p.name, armScope))) {
                keep(p.name);
            }
            break;
        }
        case PatternKind::Ctor: {
            const auto& p = static_cast<const CtorPat&>(*pat);
            for (const Name& f : p.fields) {
                keep(f.text);
            }
            break;
        }
        case PatternKind::Class: {
            const auto& p = static_cast<const ClassPat&>(*pat);
            for (const Name& f : p.fields) {
                keep(f.text);
            }
            break;
        }
        case PatternKind::Seq: {
            const auto& p = static_cast<const SeqPat&>(*pat);
            for (const SeqPatElem& el : p.elements) {
                keep(el.name);
            }
            break;
        }
        case PatternKind::AtBinding: {
            const auto& p = static_cast<const AtBindingPat&>(*pat);
            keep(p.name);
            collectBindingNames(p.sub.get(), armScope, names);
            break;
        }
        case PatternKind::Or: {
            const auto& p = static_cast<const OrPat&>(*pat);
            for (const auto& alt : p.alternatives) {
                collectBindingNames(alt.get(), armScope, names);
            }
            break;
        }
        }
    }

    static std::vector<std::string_view>
    sortedUnique(std::vector<std::string_view> names) {
        std::ranges::sort(names);
        names.erase(std::ranges::begin(std::ranges::unique(names)),
                    names.end());
        return names;
    }

    // spec/02-syntax.md requires every or-pattern alternative to bind the same
    // names; the compiler owns that as a compile error. Here it is a Warning
    // so the
    // editor shows that one branch leaves a name unbound at run time.
    void checkOrPatternBindings(const OrPat& p, Scope* armScope) {
        if (p.alternatives.size() < 2) {
            return;
        }
        std::vector<std::string_view> first;
        collectBindingNames(p.alternatives.front().get(), armScope, first);
        const std::vector<std::string_view> expected = sortedUnique(first);
        for (const auto& alt : p.alternatives | std::views::drop(1)) {
            std::vector<std::string_view> cur;
            collectBindingNames(alt.get(), armScope, cur);
            const std::vector<std::string_view> got = sortedUnique(cur);
            if (got == expected) {
                continue;
            }
            std::string_view onlyExpected;
            for (std::string_view n : expected) {
                if (std::ranges::find(got, n) == got.end()) {
                    onlyExpected = n;
                    break;
                }
            }
            std::string_view onlyGot;
            for (std::string_view n : got) {
                if (std::ranges::find(expected, n) == expected.end()) {
                    onlyGot = n;
                    break;
                }
            }
            Span span = {p.offset, p.length};
            if (!onlyExpected.empty() && !onlyGot.empty()) {
                warn(span, "or-pattern alternatives bind different names: '" +
                               std::string(onlyExpected) + "' vs '" +
                               std::string(onlyGot) + "'");
            } else {
                const std::string_view missing =
                    onlyExpected.empty() ? onlyGot : onlyExpected;
                warn(span, "or-pattern alternatives bind different names: '" +
                               std::string(missing) +
                               "' is not bound by every alternative");
            }
            return; // one warning per or-pattern
        }
    }

    // -- unused-local pass --------------------------------------------

    void reportUnused(Scope* scope) {
        if (scope->kind != ScopeKind::Global) {
            for (const auto& sym : scope->symbols) {
                if (!shouldCheckUnused(*sym)) {
                    continue;
                }
                if (sym->uses.empty()) {
                    warn(sym->declaration, "unused local '" + sym->name + "'");
                }
            }
        }
        for (const auto& child : scope->children) {
            reportUnused(child.get());
        }
    }

    static bool shouldCheckUnused(const Symbol& sym) {
        if (sym.implicit || sym.name == "_") {
            return false;
        }
        switch (sym.kind) {
        case SymbolKind::Var:
        case SymbolKind::Function:
        case SymbolKind::Class:
        case SymbolKind::LoopVar:
            return true;
        case SymbolKind::Param:
        case SymbolKind::Method:
        case SymbolKind::Field:
        case SymbolKind::EnumCtor:
        case SymbolKind::MatchBinding:
            return false;
        }
        return false;
    }
};

} // namespace

ResolvedDocument resolve(const Program& program) {
    ResolvedDocument out;
    Resolver resolver(out);
    resolver.run(program);
    return out;
}

} // namespace loxpp::tooling
