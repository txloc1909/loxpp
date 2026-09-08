#pragma once

// One open source file, parsed and resolved, with the queries the LSP
// navigation handlers (N8) need.
//
// rebuild() replaces the text and re-runs the whole pipeline (parse +
// resolve). It is called on every debounced keystroke in N8, so it allocates
// only what a fresh parse and resolve need -- no incremental state is kept
// between edits.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "diagnostic.h"
#include "tooling/ast.h"
#include "tooling/resolver.h"
#include "tooling/symbol_table.h"

namespace loxpp::tooling {

// Zero-based line/character, the shape LSP Position uses. Source is ASCII, so
// a character index equals a byte offset within the line.
struct Position {
    std::size_t line = 0;
    std::size_t character = 0;

    friend bool operator==(const Position& a, const Position& b) {
        return a.line == b.line && a.character == b.character;
    }
};

struct DocumentSymbolNode {
    std::string name;
    SymbolKind kind = SymbolKind::Var;
    Span range;          // the whole declaring construct
    Span selectionRange; // just the name
    std::vector<DocumentSymbolNode> children;
};

class DocumentModel {
  public:
    explicit DocumentModel(std::string text);

    void rebuild(std::string newText);

    [[nodiscard]] const std::string& text() const { return m_text; }
    [[nodiscard]] const Program& program() const { return m_program; }
    [[nodiscard]] const SymbolTable& symbols() const {
        return m_resolved.symbols;
    }
    // Resolver warnings, each with line and column filled in from its offset.
    [[nodiscard]] const std::vector<Diagnostic>& warnings() const {
        return m_resolved.diagnostics;
    }

    // -- position mapping ------------------------------------------------

    [[nodiscard]] Position offsetToPosition(std::size_t offset) const;
    [[nodiscard]] std::size_t positionToOffset(Position position) const;

    // -- navigation queries -------------------------------------------

    // The declaration span of the symbol referenced at offset, or nullopt when
    // offset is not on a navigable name (or names a stdlib global).
    [[nodiscard]] std::optional<Span> definitionAt(std::size_t offset) const;

    // Every use span of the symbol at offset. Includes the declaration span
    // when includeDeclaration is true. Empty when offset is not on a symbol.
    [[nodiscard]] std::vector<Span>
    referencesAt(std::size_t offset, bool includeDeclaration = true) const;

    // The symbol whose declaration or a use covers offset, or null.
    [[nodiscard]] const Symbol* symbolAt(std::size_t offset) const;

    // Hierarchical outline built from the scope tree.
    [[nodiscard]] std::vector<DocumentSymbolNode> documentSymbols() const;

  private:
    void build();
    void buildLineIndex();

    std::string m_text;
    std::vector<std::size_t> m_lineStarts;
    Program m_program;
    ResolvedDocument m_resolved;
};

} // namespace loxpp::tooling
