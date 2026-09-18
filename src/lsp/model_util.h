#pragma once

// Shared text and scope helpers for the LSP request handlers.
//
// Hover, completion (server.cpp), and signature help (signature_help.cpp)
// all walk raw document text and the tooling scope tree. One copy lives here
// so the paths cannot drift apart.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "tooling/symbol_table.h"

namespace loxpp::lsp {

inline bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

inline bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline std::string trimmed(std::string s) {
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    std::size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) {
        --b;
    }
    return s.substr(a, b - a);
}

inline std::string lineTextAt(const std::string& text, std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t start = text.rfind('\n', offset == 0 ? 0 : offset - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::size_t end = text.find('\n', offset);
    end = (end == std::string::npos) ? text.size() : end;
    return trimmed(text.substr(start, end - start));
}

inline const tooling::Scope* innermostScope(const tooling::Scope* scope,
                                            std::size_t offset) {
    for (const auto& child : scope->children) {
        const tooling::Span s = child->span;
        if (s.offset <= offset && offset < s.offset + s.length) {
            return innermostScope(child.get(), offset);
        }
    }
    return scope;
}

// Parameter names of a user function or method, in declaration order.
inline std::vector<std::string> functionParamNames(const tooling::Symbol& sym) {
    std::vector<std::pair<std::size_t, std::string>> found;
    if (sym.innerScope != nullptr) {
        for (const auto& s : sym.innerScope->symbols) {
            if (s->kind == tooling::SymbolKind::Param && !s->implicit) {
                found.emplace_back(s->declaration.offset, s->name);
            }
        }
    }
    std::ranges::sort(found);
    std::vector<std::string> names;
    names.reserve(found.size());
    for (auto& [off, name] : found) {
        names.push_back(name);
    }
    return names;
}

} // namespace loxpp::lsp
