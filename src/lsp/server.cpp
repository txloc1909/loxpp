#include "lsp/server.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lsp/keyword_docs.h"
#include "lsp/stdlib_docs.h"
#include "tooling/document_model.h"
#include "tooling/symbol_table.h"

namespace loxpp::lsp {

namespace {

using tooling::DocumentModel;
using tooling::Scope;
using tooling::ScopeKind;
using tooling::Span;
using tooling::Symbol;
using tooling::SymbolKind;

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// The [A-Za-z0-9_] run that covers `offset` (or ends just before it).
std::pair<std::size_t, std::size_t> wordAround(const std::string& text,
                                               std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t start = offset;
    while (start > 0 && isWordChar(text[start - 1])) {
        --start;
    }
    std::size_t end = offset;
    while (end < text.size() && isWordChar(text[end])) {
        ++end;
    }
    return {start, end - start};
}

std::string trimmed(std::string s) {
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

std::string lineTextAt(const std::string& text, std::size_t offset) {
    offset = std::min(offset, text.size());
    std::size_t start = text.rfind('\n', offset == 0 ? 0 : offset - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::size_t end = text.find('\n', offset);
    end = (end == std::string::npos) ? text.size() : end;
    return trimmed(text.substr(start, end - start));
}

// The identifier that a `.` before `wordStart` is a member of, if any.
std::optional<std::string> receiverBeforeDot(const std::string& text,
                                             std::size_t wordStart) {
    std::size_t i = wordStart;
    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t')) {
        --i;
    }
    if (i == 0 || text[i - 1] != '.') {
        return std::nullopt;
    }
    --i;
    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t')) {
        --i;
    }
    std::size_t end = i;
    while (i > 0 && isWordChar(text[i - 1])) {
        --i;
    }
    if (i == end) {
        return std::nullopt;
    }
    return text.substr(i, end - i);
}

Range spanToRange(const DocumentModel& model, Span span) {
    const tooling::Position a = model.offsetToPosition(span.offset);
    const tooling::Position b =
        model.offsetToPosition(span.offset + span.length);
    return Range{
        Position{static_cast<std::uint32_t>(a.line),
                 static_cast<std::uint32_t>(a.character)},
        Position{static_cast<std::uint32_t>(b.line),
                 static_cast<std::uint32_t>(b.character)},
    };
}

SymbolKind kindOf(const Symbol& s) { return s.kind; }

lsp::SymbolKind toLspSymbolKind(SymbolKind k) {
    switch (k) {
    case SymbolKind::Function:
        return lsp::SymbolKind::Function;
    case SymbolKind::Class:
        return lsp::SymbolKind::Class;
    case SymbolKind::Method:
        return lsp::SymbolKind::Method;
    case SymbolKind::Field:
        return lsp::SymbolKind::Field;
    case SymbolKind::EnumCtor:
        return lsp::SymbolKind::EnumMember;
    case SymbolKind::Var:
    case SymbolKind::Param:
    case SymbolKind::LoopVar:
    case SymbolKind::MatchBinding:
        return lsp::SymbolKind::Variable;
    }
    return lsp::SymbolKind::Variable;
}

std::string symbolKindWord(SymbolKind k) {
    switch (k) {
    case SymbolKind::Var:
        return "variable";
    case SymbolKind::Param:
        return "parameter";
    case SymbolKind::Function:
        return "function";
    case SymbolKind::Class:
        return "class";
    case SymbolKind::Method:
        return "method";
    case SymbolKind::Field:
        return "field";
    case SymbolKind::EnumCtor:
        return "enum constructor";
    case SymbolKind::LoopVar:
        return "loop variable";
    case SymbolKind::MatchBinding:
        return "match binding";
    }
    return "name";
}

CompletionItemKind toCompletionKind(SymbolKind k) {
    switch (k) {
    case SymbolKind::Function:
        return CompletionItemKind::Function;
    case SymbolKind::Class:
        return CompletionItemKind::Class;
    case SymbolKind::Method:
        return CompletionItemKind::Method;
    case SymbolKind::EnumCtor:
        return CompletionItemKind::EnumMember;
    default:
        return CompletionItemKind::Variable;
    }
}

std::vector<std::string> functionParams(const Symbol& sym) {
    std::vector<std::pair<std::size_t, std::string>> found;
    if (sym.innerScope != nullptr) {
        for (const auto& s : sym.innerScope->symbols) {
            if (s->kind == SymbolKind::Param && !s->implicit) {
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

const Scope* innermostScope(const Scope* scope, std::size_t offset) {
    for (const auto& child : scope->children) {
        const Span s = child->span;
        if (s.offset <= offset && offset < s.offset + s.length) {
            return innermostScope(child.get(), offset);
        }
    }
    return scope;
}

// User symbols visible as a bare name at `offset`: the global scope (globals
// are late-bound, so all of them) plus every enclosing local scope's symbols
// that are declared before `offset`.
std::vector<const Symbol*> visibleSymbols(const DocumentModel& model,
                                          std::size_t offset) {
    const Scope* global = model.symbols().global();
    const Scope* start = innermostScope(global, offset);

    std::vector<const Symbol*> out;
    std::unordered_set<std::string> seen;
    for (const Scope* sc = start; sc != nullptr; sc = sc->parent) {
        const bool isGlobal = sc->kind == ScopeKind::Global;
        for (const auto& sym : sc->symbols) {
            if (sym->implicit || sym->name == "_") {
                continue;
            }
            if (!isGlobal && sym->declaration.offset >= offset) {
                continue;
            }
            if (seen.insert(sym->name).second) {
                out.push_back(sym.get());
            }
        }
    }
    return out;
}

std::string renderUserSymbol(const DocumentModel& model, const Symbol& sym) {
    std::string out = symbolKindWord(kindOf(sym)) + " `" + sym.name + "`";
    if (sym.kind == SymbolKind::Function || sym.kind == SymbolKind::Method) {
        const std::vector<std::string> params = functionParams(sym);
        out += "\n\n```lox\nfun " + sym.name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            out += params[i];
            if (i + 1 < params.size()) {
                out += ", ";
            }
        }
        out += ")\n```";
    }
    const std::string decl = lineTextAt(model.text(), sym.declaration.offset);
    if (!decl.empty()) {
        out += "\n\nDeclared: `" + decl + "`";
    }
    return out;
}

CompletionItem stdlibItem(const StdlibEntry& e, bool member) {
    CompletionItem c;
    c.label = std::string(e.name);
    if (e.arity == kArityConstant) {
        c.kind = CompletionItemKind::Constant;
    } else {
        c.kind =
            member ? CompletionItemKind::Method : CompletionItemKind::Function;
    }
    c.detail = std::string(e.signature);
    c.documentation = std::string(e.description);
    return c;
}

struct Snippet {
    std::string_view label;
    std::string_view body;
    std::string_view detail;
};

constexpr std::array<Snippet, 5> kSnippets = {{
    {"if", "if (${1:condition}) {\n\t$0\n}", "if statement"},
    {"for",
     "for (var ${1:i} = 0; ${1:i} < ${2:n}; ${1:i} = ${1:i} + 1) {\n\t$0\n}",
     "for loop"},
    {"fun", "fun ${1:name}(${2:params}) {\n\t$0\n}", "function declaration"},
    {"match", "match ${1:subject} {\n\tcase ${2:pattern} => ${0}\n}",
     "match expression"},
    {"class", "class ${1:Name} {\n\t$0\n}", "class declaration"},
}};

DocumentSymbol convertSymbol(const DocumentModel& model,
                             const tooling::DocumentSymbolNode& node) {
    DocumentSymbol d;
    d.name = node.name.empty() ? std::string("?") : node.name;
    d.kind = toLspSymbolKind(node.kind);

    Span range = node.range.length != 0 ? node.range : node.selectionRange;
    Span selection =
        node.selectionRange.length != 0 ? node.selectionRange : range;
    const bool inside =
        selection.offset >= range.offset &&
        selection.offset + selection.length <= range.offset + range.length;
    if (!inside) {
        selection = range;
    }
    d.range = spanToRange(model, range);
    d.selectionRange = spanToRange(model, selection);

    for (const auto& child : node.children) {
        d.children.push_back(convertSymbol(model, child));
    }
    return d;
}

tooling::Position toToolingPos(const Position& p) {
    return tooling::Position{p.line, p.character};
}

void appendMemberCompletions(json& items, const std::string& receiver) {
    // `math` is the only receiver the resolver can identify by name. For any
    // other `x.` the receiver type is unknown, so offer nothing rather than a
    // guess that lists unrelated Map/File methods.
    if (receiver != "math") {
        return;
    }
    for (const StdlibEntry& e : allMathMemberDocs()) {
        items.push_back(stdlibItem(e, true));
    }
}

// Keywords, stdlib globals, in-scope user symbols, and the statement snippets.
void appendGeneralCompletions(json& items, const DocumentModel& model,
                              std::size_t at) {
    for (const auto& [name, doc] : kKeywordDocs) {
        if (name == "_") {
            continue;
        }
        CompletionItem c;
        c.label = std::string(name);
        c.kind = CompletionItemKind::Keyword;
        c.documentation = std::string(doc);
        items.push_back(c);
    }
    for (const StdlibEntry& e : allGlobalDocs()) {
        items.push_back(stdlibItem(e, false));
    }
    for (const Symbol* sym : visibleSymbols(model, at)) {
        CompletionItem c;
        c.label = sym->name;
        c.kind = toCompletionKind(sym->kind);
        c.detail = symbolKindWord(sym->kind);
        items.push_back(c);
    }
    for (const Snippet& s : kSnippets) {
        CompletionItem c;
        c.label = std::string(s.label);
        c.kind = CompletionItemKind::Snippet;
        c.detail = std::string(s.detail);
        c.insertText = std::string(s.body);
        c.insertTextFormat = InsertTextFormat::Snippet;
        items.push_back(c);
    }
}

} // namespace

Server::Server(JsonRpc& rpc)
    : m_rpc(rpc), m_store([this](const std::string& uri, std::int64_t version,
                                 const std::vector<Diagnostic>& diags) {
          publishDiagnostics(uri, version, diags);
      }) {}

void Server::requireReady() const {
    if (!m_gotInitialize) {
        throw RpcError{RpcErrorCode::ServerNotInitialized,
                       "server not initialized"};
    }
    if (m_shutdownReceived) {
        throw RpcError{RpcErrorCode::InvalidRequest, "server has shut down"};
    }
}

void Server::publishDiagnostics(const std::string& uri, std::int64_t version,
                                const std::vector<Diagnostic>& diags) {
    json params = {
        {"uri", uri},
        {"version", version},
        {"diagnostics", diags},
    };
    m_rpc.notify("textDocument/publishDiagnostics", std::move(params));
}

// -- lifecycle ----------------------------------------------------------

json Server::onInitialize(const json& /*params*/) {
    m_gotInitialize = true;
    return json{
        {"capabilities",
         {
             {"positionEncoding", "utf-16"},
             {"textDocumentSync",
              {{"openClose", true},
               {"change", 1}, // Full
               {"save", {{"includeText", false}}}}},
             {"hoverProvider", true},
             {"definitionProvider", true},
             {"referencesProvider", true},
             {"documentHighlightProvider", true},
             {"documentSymbolProvider", true},
             {"completionProvider",
              {{"triggerCharacters", json::array({"."})}}},
         }},
        {"serverInfo", {{"name", "loxpp-lsp"}, {"version", "0.1.0"}}},
    };
}

void Server::onInitialized(const json& /*params*/) {
    std::cerr << "loxpp-lsp: initialized\n";
}

json Server::onShutdown(const json& /*params*/) {
    m_shutdownReceived = true;
    return nullptr;
}

void Server::onExit(const json& /*params*/) {
    m_rpc.requestStop(m_shutdownReceived ? 0 : 1);
}

// -- sync -------------------------------------------------------------

void Server::onDidOpen(const json& params) {
    const auto& doc = params.at("textDocument");
    m_store.didOpen(doc.at("uri").get<std::string>(),
                    doc.value("version", std::int64_t{0}),
                    doc.at("text").get<std::string>());
}

void Server::onDidChange(const json& params) {
    const std::string uri =
        params.at("textDocument").at("uri").get<std::string>();
    const std::int64_t version =
        params.at("textDocument").value("version", std::int64_t{0});
    const auto& changes = params.at("contentChanges");
    if (!changes.is_array() || changes.empty()) {
        return;
    }
    // Full sync: the last entry carries the whole document.
    m_store.didChange(uri, version,
                      changes.back().at("text").get<std::string>());
}

void Server::onDidClose(const json& params) {
    const std::string uri =
        params.at("textDocument").at("uri").get<std::string>();
    m_store.didClose(uri);
    // Clear the client's view for this file.
    m_rpc.notify("textDocument/publishDiagnostics",
                 json{{"uri", uri}, {"diagnostics", json::array()}});
}

void Server::onDidSave(const json& params) {
    m_store.didSave(params.at("textDocument").at("uri").get<std::string>());
}

// -- language features ------------------------------------------------

json Server::onDocumentSymbol(const json& params) {
    requireReady();
    const std::string uri =
        params.at("textDocument").at("uri").get<std::string>();
    json result = json::array();
    (void)m_store.read(uri, [&](const DocumentModel& model) {
        for (const auto& node : model.documentSymbols()) {
            result.push_back(convertSymbol(model, node));
        }
    });
    return result;
}

json Server::onHover(const json& params) {
    requireReady();
    const auto p = params.get<TextDocumentPositionParams>();
    json result = nullptr;
    (void)m_store.read(p.uri, [&](const DocumentModel& model) {
        const std::string& text = model.text();
        const std::size_t off =
            model.positionToOffset(toToolingPos(p.position));
        const auto [ws, wl] = wordAround(text, off);
        if (wl == 0) {
            return;
        }
        const std::string word = text.substr(ws, wl);
        const Range wr = spanToRange(model, Span{ws, wl});

        if (const auto recv = receiverBeforeDot(text, ws)) {
            const StdlibEntry* e =
                (*recv == "math") ? mathMemberDoc(word) : nullptr;
            if (e == nullptr) {
                e = methodDoc(word); // Map / File method (names are unique)
            }
            if (e != nullptr) {
                result = Hover{MarkupContent{"markdown", renderHover(*e)}, wr};
            }
            return;
        }

        if (const auto kd = keywordDoc(word); !kd.empty()) {
            result = Hover{MarkupContent{"markdown", "**" + word + "** — " +
                                                         std::string(kd)},
                           wr};
            return;
        }

        if (const Symbol* sym = model.symbolAt(off)) {
            result = Hover{
                MarkupContent{"markdown", renderUserSymbol(model, *sym)}, wr};
            return;
        }

        if (const auto g = model.knownGlobalAt(off); !g.empty()) {
            if (const StdlibEntry* e = stdlibGlobalDoc(g)) {
                result = Hover{MarkupContent{"markdown", renderHover(*e)}, wr};
            }
        }
    });
    return result;
}

json Server::onCompletion(const json& params) {
    requireReady();
    const auto p = params.get<TextDocumentPositionParams>();
    json items = json::array();
    (void)m_store.read(p.uri, [&](const DocumentModel& model) {
        const std::string& text = model.text();
        std::size_t off = model.positionToOffset(toToolingPos(p.position));
        off = std::min(off, text.size());
        std::size_t prefixStart = off;
        while (prefixStart > 0 && isWordChar(text[prefixStart - 1])) {
            --prefixStart;
        }

        if (const auto recv = receiverBeforeDot(text, prefixStart)) {
            appendMemberCompletions(items, *recv);
        } else {
            appendGeneralCompletions(items, model, prefixStart);
        }
    });
    return items;
}

json Server::onDefinition(const json& params) {
    requireReady();
    const auto p = params.get<TextDocumentPositionParams>();
    json result = nullptr;
    (void)m_store.read(p.uri, [&](const DocumentModel& model) {
        const std::size_t off =
            model.positionToOffset(toToolingPos(p.position));
        if (const auto span = model.definitionAt(off)) {
            result = Location{p.uri, spanToRange(model, *span)};
        }
    });
    return result;
}

json Server::onReferences(const json& params) {
    requireReady();
    const auto p = params.get<TextDocumentPositionParams>();
    bool includeDeclaration = true;
    if (params.contains("context")) {
        includeDeclaration =
            params.at("context").value("includeDeclaration", true);
    }
    json result = json::array();
    (void)m_store.read(p.uri, [&](const DocumentModel& model) {
        const std::size_t off =
            model.positionToOffset(toToolingPos(p.position));
        for (const Span& span : model.referencesAt(off, includeDeclaration)) {
            result.push_back(Location{p.uri, spanToRange(model, span)});
        }
    });
    return result;
}

json Server::onDocumentHighlight(const json& params) {
    requireReady();
    const auto p = params.get<TextDocumentPositionParams>();
    json result = json::array();
    (void)m_store.read(p.uri, [&](const DocumentModel& model) {
        const std::size_t off =
            model.positionToOffset(toToolingPos(p.position));
        for (const Span& span : model.referencesAt(off, true)) {
            result.push_back(
                json{{"range", spanToRange(model, span)}, {"kind", 1}});
        }
    });
    return result;
}

// -- registration -----------------------------------------------------

void Server::registerHandlers() {
    m_rpc.onRequest("initialize",
                    [this](const json& p) { return onInitialize(p); });
    m_rpc.onNotification("initialized",
                         [](const json& p) { Server::onInitialized(p); });
    m_rpc.onRequest("shutdown",
                    [this](const json& p) { return onShutdown(p); });
    m_rpc.onNotification("exit", [this](const json& p) { onExit(p); });

    m_rpc.onNotification("textDocument/didOpen",
                         [this](const json& p) { onDidOpen(p); });
    m_rpc.onNotification("textDocument/didChange",
                         [this](const json& p) { onDidChange(p); });
    m_rpc.onNotification("textDocument/didClose",
                         [this](const json& p) { onDidClose(p); });
    m_rpc.onNotification("textDocument/didSave",
                         [this](const json& p) { onDidSave(p); });

    m_rpc.onRequest("textDocument/documentSymbol",
                    [this](const json& p) { return onDocumentSymbol(p); });
    m_rpc.onRequest("textDocument/hover",
                    [this](const json& p) { return onHover(p); });
    m_rpc.onRequest("textDocument/completion",
                    [this](const json& p) { return onCompletion(p); });
    m_rpc.onRequest("textDocument/definition",
                    [this](const json& p) { return onDefinition(p); });
    m_rpc.onRequest("textDocument/references",
                    [this](const json& p) { return onReferences(p); });
    m_rpc.onRequest("textDocument/documentHighlight",
                    [this](const json& p) { return onDocumentHighlight(p); });
}

} // namespace loxpp::lsp
