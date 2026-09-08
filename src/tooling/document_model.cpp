#include "tooling/document_model.h"

#include <algorithm>
#include <ranges>
#include <utility>

#include "tooling/tooling_parser.h"

namespace loxpp::tooling {

DocumentModel::DocumentModel(std::string text) : m_text(std::move(text)) {
    build();
}

void DocumentModel::rebuild(std::string newText) {
    m_text = std::move(newText);
    build();
}

void DocumentModel::build() {
    buildLineIndex();
    m_program = parse(m_text);
    m_resolved = resolve(m_program);

    // The resolver works from offsets only; fill the 1-based line and column
    // so downstream text formatters do not each repeat the line search.
    for (Diagnostic& d : m_resolved.diagnostics) {
        const Position p = offsetToPosition(d.offset);
        d.line = p.line + 1;
        d.column = p.character + 1;
    }
    std::ranges::sort(m_resolved.diagnostics, {},
                      [](const Diagnostic& d) { return d.offset; });
}

void DocumentModel::buildLineIndex() {
    m_lineStarts.clear();
    m_lineStarts.push_back(0);
    for (std::size_t i = 0; i < m_text.size(); ++i) {
        if (m_text[i] == '\n') {
            m_lineStarts.push_back(i + 1);
        }
    }
}

Position DocumentModel::offsetToPosition(std::size_t offset) const {
    offset = std::min(offset, m_text.size());
    auto it = std::ranges::upper_bound(m_lineStarts, offset);
    const std::size_t line =
        static_cast<std::size_t>(it - m_lineStarts.begin()) - 1;
    return {.line = line, .character = offset - m_lineStarts[line]};
}

std::size_t DocumentModel::positionToOffset(Position position) const {
    if (m_lineStarts.empty()) {
        return 0;
    }
    const std::size_t line = std::min(position.line, m_lineStarts.size() - 1);
    const std::size_t lineStart = m_lineStarts[line];
    const std::size_t lineEnd = (line + 1 < m_lineStarts.size())
                                    ? m_lineStarts[line + 1]
                                    : m_text.size();
    return std::min(lineStart + position.character, lineEnd);
}

std::optional<Span> DocumentModel::definitionAt(std::size_t offset) const {
    const ReferenceIndex::Entry* entry = m_resolved.references.at(offset);
    if (entry == nullptr || entry->symbol == nullptr) {
        return std::nullopt;
    }
    return entry->symbol->declaration;
}

std::vector<Span> DocumentModel::referencesAt(std::size_t offset,
                                              bool includeDeclaration) const {
    const ReferenceIndex::Entry* entry = m_resolved.references.at(offset);
    if (entry == nullptr || entry->symbol == nullptr) {
        return {};
    }
    const Symbol* sym = entry->symbol;
    std::vector<Span> out;
    out.reserve(sym->uses.size() + 1);
    if (includeDeclaration) {
        out.push_back(sym->declaration);
    }
    out.insert(out.end(), sym->uses.begin(), sym->uses.end());
    std::ranges::sort(out, {}, [](const Span& s) { return s.offset; });
    out.erase(std::ranges::begin(std::ranges::unique(out)), out.end());
    return out;
}

const Symbol* DocumentModel::symbolAt(std::size_t offset) const {
    const ReferenceIndex::Entry* entry = m_resolved.references.at(offset);
    return entry != nullptr ? entry->symbol : nullptr;
}

namespace {

bool includeInOutline(const Symbol& sym, bool topLevel) {
    if (sym.implicit || sym.name == "_") {
        return false;
    }
    switch (sym.kind) {
    case SymbolKind::Function:
    case SymbolKind::Class:
    case SymbolKind::Method:
        return true;
    case SymbolKind::Var:
    case SymbolKind::Field:
        return topLevel;
    case SymbolKind::EnumCtor: // listed under its enum via Symbol::members
    case SymbolKind::Param:
    case SymbolKind::LoopVar:
    case SymbolKind::MatchBinding:
        return false;
    }
    return false;
}

void collectOutline(const Scope* scope, bool topLevel,
                    std::vector<DocumentSymbolNode>& out) {
    for (const auto& symPtr : scope->symbols) {
        const Symbol& sym = *symPtr;
        if (!includeInOutline(sym, topLevel)) {
            continue;
        }
        DocumentSymbolNode node;
        node.name = sym.name;
        node.kind = sym.kind;
        node.range = sym.fullRange;
        node.selectionRange = sym.declaration;
        if (sym.innerScope != nullptr) {
            collectOutline(sym.innerScope, false, node.children);
        }
        for (const Symbol* member : sym.members) {
            DocumentSymbolNode child;
            child.name = member->name;
            child.kind = member->kind;
            child.range = member->fullRange;
            child.selectionRange = member->declaration;
            node.children.push_back(std::move(child));
        }
        out.push_back(std::move(node));
    }
}

} // namespace

std::vector<DocumentSymbolNode> DocumentModel::documentSymbols() const {
    std::vector<DocumentSymbolNode> out;
    collectOutline(m_resolved.symbols.global(), true, out);
    return out;
}

} // namespace loxpp::tooling
