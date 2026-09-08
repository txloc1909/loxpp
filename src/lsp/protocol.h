#pragma once

// LSP protocol value types and their JSON conversions.
//
// Only the types this server actually produces or consumes are here. Every
// conversion is an ADL to_json / from_json so nlohmann::json picks it up for
// free. Position.character is a UTF-16 code-unit count; Lox++ source is ASCII
// (spec/01-lexical.md), so it equals the byte offset inside the line and the
// server advertises positionEncoding "utf-16" honestly.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace loxpp::lsp {

using json = nlohmann::json;

// -- Position / Range / Location -------------------------------------------

struct Position {
    std::uint32_t line = 0;
    std::uint32_t character = 0;
};

inline void to_json(json& j, const Position& p) {
    j = json{{"line", p.line}, {"character", p.character}};
}
inline void from_json(const json& j, Position& p) {
    j.at("line").get_to(p.line);
    j.at("character").get_to(p.character);
}

struct Range {
    Position start;
    Position end;
};

inline void to_json(json& j, const Range& r) {
    j = json{{"start", r.start}, {"end", r.end}};
}
inline void from_json(const json& j, Range& r) {
    j.at("start").get_to(r.start);
    j.at("end").get_to(r.end);
}

struct Location {
    std::string uri;
    Range range;
};

inline void to_json(json& j, const Location& l) {
    j = json{{"uri", l.uri}, {"range", l.range}};
}

// -- Diagnostic ------------------------------------------------------------

// LSP DiagnosticSeverity: 1 Error, 2 Warning, 3 Information, 4 Hint.
enum class DiagnosticSeverity : std::uint8_t {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

struct Diagnostic {
    Range range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string source;
    std::string message;
};

inline void to_json(json& j, const Diagnostic& d) {
    j = json{
        {"range", d.range},
        {"severity", static_cast<int>(d.severity)},
        {"source", d.source},
        {"message", d.message},
    };
}

// -- Document symbols ----------------------------------------------------

// LSP SymbolKind (subset used here).
enum class SymbolKind : std::uint8_t {
    File = 1,
    Namespace = 3,
    Class = 5,
    Method = 6,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Function = 12,
    Variable = 13,
    Constant = 14,
    EnumMember = 22,
};

struct DocumentSymbol {
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    Range range;
    Range selectionRange;
    std::vector<DocumentSymbol> children;
};

inline void to_json(json& j, const DocumentSymbol& s) {
    j = json{
        {"name", s.name},
        {"kind", static_cast<int>(s.kind)},
        {"range", s.range},
        {"selectionRange", s.selectionRange},
    };
    if (!s.children.empty()) {
        j["children"] = s.children;
    }
}

// -- Hover / MarkupContent --------------------------------------------

struct MarkupContent {
    std::string kind = "markdown";
    std::string value;
};

inline void to_json(json& j, const MarkupContent& m) {
    j = json{{"kind", m.kind}, {"value", m.value}};
}

struct Hover {
    MarkupContent contents;
    std::optional<Range> range;
};

inline void to_json(json& j, const Hover& h) {
    j = json{{"contents", h.contents}};
    if (h.range) {
        j["range"] = *h.range;
    }
}

// -- Completion --------------------------------------------------------

// LSP CompletionItemKind (subset).
enum class CompletionItemKind : std::uint8_t {
    Text = 1,
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    Constant = 21,
    EnumMember = 20,
};

// LSP InsertTextFormat: 1 PlainText, 2 Snippet.
enum class InsertTextFormat : std::uint8_t {
    PlainText = 1,
    Snippet = 2,
};

struct CompletionItem {
    std::string label;
    CompletionItemKind kind = CompletionItemKind::Text;
    std::optional<std::string> detail;
    std::optional<std::string> documentation;
    std::optional<std::string> insertText;
    InsertTextFormat insertTextFormat = InsertTextFormat::PlainText;
};

inline void to_json(json& j, const CompletionItem& c) {
    j = json{{"label", c.label}, {"kind", static_cast<int>(c.kind)}};
    if (c.detail) {
        j["detail"] = *c.detail;
    }
    if (c.documentation) {
        j["documentation"] = *c.documentation;
    }
    if (c.insertText) {
        j["insertText"] = *c.insertText;
    }
    if (c.insertTextFormat != InsertTextFormat::PlainText) {
        j["insertTextFormat"] = static_cast<int>(c.insertTextFormat);
    }
}

// -- Text document sync -------------------------------------------------

struct TextDocumentItem {
    std::string uri;
    std::string languageId;
    std::int64_t version = 0;
    std::string text;
};

inline void from_json(const json& j, TextDocumentItem& d) {
    j.at("uri").get_to(d.uri);
    if (j.contains("languageId")) {
        j.at("languageId").get_to(d.languageId);
    }
    if (j.contains("version")) {
        j.at("version").get_to(d.version);
    }
    j.at("text").get_to(d.text);
}

// Params shared by most textDocument/* requests.
struct TextDocumentPositionParams {
    std::string uri;
    Position position;
};

inline void from_json(const json& j, TextDocumentPositionParams& p) {
    j.at("textDocument").at("uri").get_to(p.uri);
    j.at("position").get_to(p.position);
}

} // namespace loxpp::lsp
