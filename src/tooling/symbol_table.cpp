#include "tooling/symbol_table.h"

#include <ranges>
#include <utility>

namespace loxpp::tooling {

Symbol* Scope::declare(std::string name, SymbolKind kind, Span declaration) {
    auto sym = std::make_unique<Symbol>();
    sym->name = std::move(name);
    sym->kind = kind;
    sym->declaration = declaration;
    sym->fullRange = declaration;
    sym->scope = this;
    symbols.push_back(std::move(sym));
    return symbols.back().get();
}

Symbol* Scope::findLocal(std::string_view name) const {
    // Reverse order: when a name is declared twice in one scope (a static
    // error the resolver warns about, but still models), the most recent
    // declaration is the one a later reference binds to.
    for (const auto& sym : std::views::reverse(symbols)) {
        if (sym->name == name) {
            return sym.get();
        }
    }
    return nullptr;
}

SymbolTable::SymbolTable() {
    m_root = std::make_unique<Scope>();
    m_root->kind = ScopeKind::Global;
    m_global = m_root.get();
}

Scope* SymbolTable::addChild(Scope* parent, ScopeKind kind, Span span) {
    auto child = std::make_unique<Scope>();
    child->kind = kind;
    child->parent = parent;
    child->span = span;
    parent->children.push_back(std::move(child));
    return parent->children.back().get();
}

} // namespace loxpp::tooling
