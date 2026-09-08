#pragma once

// Scope tree and symbol table for the editor-tooling resolver.
//
// The resolver (resolver.h) walks the N5 AST and fills this structure: one
// Scope per lexical region, one Symbol per declared name, and a use-span list
// on each Symbol. The LSP navigation handlers (N8) read it back through
// DocumentModel.
//
// Names are stored as owned std::string, not string_view into the source, so
// the table stays valid independent of the parsed Program's lifetime.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loxpp::tooling {

// A byte range into the document source: source.substr(offset, length).
struct Span {
    std::size_t offset = 0;
    std::size_t length = 0;

    Span() = default;
    Span(std::size_t offset, std::size_t length)
        : offset(offset), length(length) {}

    [[nodiscard]] bool contains(std::size_t pos) const {
        return pos >= offset && pos < offset + length;
    }
    [[nodiscard]] bool valid() const { return length != 0 || offset != 0; }

    friend bool operator==(const Span& a, const Span& b) {
        return a.offset == b.offset && a.length == b.length;
    }
};

enum class SymbolKind : std::uint8_t {
    Var,
    Param,
    Function,
    Class,
    Method,
    Field,
    EnumCtor,
    LoopVar,
    MatchBinding,
};

enum class ScopeKind : std::uint8_t {
    Global,
    Function,
    Block,
    ForHeader,
    MatchArm,
};

struct Scope;

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Var;
    Span declaration; // span of the declaring identifier
    Span fullRange;   // span of the whole declaring construct (for outlines)
    Scope* scope = nullptr;
    std::vector<Span> uses;

    // For Function / Method / Class symbols: the scope that holds the body's
    // declarations. Null for everything else. Class symbols point at a Block
    // scope whose symbols are the methods; that scope is never on any lookup
    // parent-chain, only reachable by walking Scope::children.
    Scope* innerScope = nullptr;

    // For an enum type symbol: its constructor symbols (owned by the scope the
    // enum was declared in, listed here for the document outline).
    std::vector<Symbol*> members;

    // Implicit `this` / `super` bindings and `_` are never reported unused.
    bool implicit = false;
};

struct Scope {
    ScopeKind kind = ScopeKind::Block;
    Scope* parent = nullptr;
    Span span;
    std::vector<std::unique_ptr<Symbol>> symbols;
    std::vector<std::unique_ptr<Scope>> children;

    // Declare a name in this scope. Returns the new Symbol. Does not check for
    // a clash -- the resolver does that so it can choose whether to warn.
    Symbol* declare(std::string name, SymbolKind kind, Span declaration);

    // Look up a name in this scope only.
    [[nodiscard]] Symbol* findLocal(std::string_view name) const;
};

class SymbolTable {
  public:
    SymbolTable();
    SymbolTable(SymbolTable&&) noexcept = default;
    SymbolTable& operator=(SymbolTable&&) noexcept = default;
    SymbolTable(const SymbolTable&) = delete;
    SymbolTable& operator=(const SymbolTable&) = delete;

    [[nodiscard]] Scope* global() { return m_global; }
    [[nodiscard]] const Scope* global() const { return m_global; }

    static Scope* addChild(Scope* parent, ScopeKind kind, Span span);

  private:
    std::unique_ptr<Scope> m_root;
    Scope* m_global = nullptr;
};

} // namespace loxpp::tooling
