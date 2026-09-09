#pragma once

// Name resolution over the editor-tooling AST.
//
// resolve() walks a parsed Program, builds the scope tree and symbol table,
// links every identifier reference to the Symbol it names (or to a stdlib
// global, or to nothing), and reports lint-level problems as Warning / Info
// diagnostics. The authoritative compile errors stay with src/analyze.h;
// this pass never produces an Error.
//
// Globals in Lox++ are late-bound: a function body may name a global declared
// later in the file. resolve() therefore collects every top-level declaration
// in a first sweep before it resolves any reference, so a forward reference to
// a global is never flagged as unknown. Locals stay strictly lexical: a local
// is visible only to references that come after its declaration.

#include <cstddef>
#include <vector>

#include "diagnostic.h"
#include "tooling/ast.h"
#include "tooling/symbol_table.h"

namespace loxpp::tooling {

// Offset -> Symbol lookup over every declaration and every use in the file.
class ReferenceIndex {
  public:
    struct Entry {
        Span span;
        Symbol* symbol = nullptr; // null for a stdlib global
        bool isDeclaration = false;
        bool knownGlobal = false; // symbol == null, the name is a stdlib global

        Entry(Span span, Symbol* symbol, bool isDeclaration, bool knownGlobal)
            : span(span), symbol(symbol), isDeclaration(isDeclaration),
              knownGlobal(knownGlobal) {}
    };

    void add(Entry entry);
    void finish(); // sort by offset; call once after resolve

    // The entry whose span covers offset, or null.
    [[nodiscard]] const Entry* at(std::size_t offset) const;

    [[nodiscard]] const std::vector<Entry>& entries() const {
        return m_entries;
    }

  private:
    std::vector<Entry> m_entries;
};

struct ResolvedDocument {
    SymbolTable symbols;
    ReferenceIndex references;
    std::vector<Diagnostic> diagnostics; // Warning / Info only

    ResolvedDocument() = default;
    ResolvedDocument(ResolvedDocument&&) noexcept = default;
    ResolvedDocument& operator=(ResolvedDocument&&) noexcept = default;
    ResolvedDocument(const ResolvedDocument&) = delete;
    ResolvedDocument& operator=(const ResolvedDocument&) = delete;
};

ResolvedDocument resolve(const Program& program);

} // namespace loxpp::tooling
