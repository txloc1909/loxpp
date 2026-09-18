#include "lsp/signature_help.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "lsp/protocol.h"
#include "lsp/stdlib_docs.h"
#include "tooling/symbol_table.h"

namespace loxpp::lsp {

namespace {

using tooling::DocumentModel;
using tooling::Scope;
using tooling::ScopeKind;
using tooling::Symbol;
using tooling::SymbolKind;

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Parameter names from a signature of the form "name(a, b) -> T". Returns an
// empty list for "name()". The stdlib table owns the format, so a missing
// paren pair yields no names rather than a guess.
std::vector<std::string> paramNamesFromSignature(std::string_view sig) {
    const std::size_t open = sig.find('(');
    const std::size_t close =
        sig.find(')', open == std::string_view::npos ? 0 : open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos ||
        close <= open + 1) {
        return {};
    }
    std::vector<std::string> out;
    std::size_t start = open + 1;
    while (start < close) {
        while (start < close && isSpace(sig[start])) {
            ++start;
        }
        std::size_t end = start;
        while (end < close && sig[end] != ',') {
            ++end;
        }
        std::size_t trim = end;
        while (trim > start && isSpace(sig[trim - 1])) {
            --trim;
        }
        if (trim > start) {
            out.emplace_back(sig.substr(start, trim - start));
        }
        start = end + 1;
    }
    return out;
}

std::string trimSpaces(const std::string& s) {
    std::size_t a = 0;
    while (a < s.size() && isSpace(s[a])) {
        ++a;
    }
    std::size_t b = s.size();
    while (b > a && isSpace(s[b - 1])) {
        --b;
    }
    return s.substr(a, b - a);
}

// innermostScope + visible-function lookup mirror the completion path: globals
// are late-bound, locals must be declared before the call's open paren.
const Scope* innermostScope(const Scope* scope, std::size_t offset) {
    for (const auto& child : scope->children) {
        const tooling::Span s = child->span;
        if (s.offset <= offset && offset < s.offset + s.length) {
            return innermostScope(child.get(), offset);
        }
    }
    return scope;
}

const Symbol* findUserFunction(const DocumentModel& model,
                               std::string_view name, std::size_t offset) {
    const Scope* global = model.symbols().global();
    const Scope* start = innermostScope(global, offset);
    const Symbol* globalMatch = nullptr;
    for (const Scope* sc = start; sc != nullptr; sc = sc->parent) {
        const bool isGlobal = sc->kind == ScopeKind::Global;
        for (const auto& sym : sc->symbols) {
            if (sym->name != name || sym->implicit) {
                continue;
            }
            if (sym->kind != SymbolKind::Function &&
                sym->kind != SymbolKind::Method) {
                continue;
            }
            if (!isGlobal && sym->declaration.offset >= offset) {
                continue;
            }
            if (isGlobal) {
                globalMatch = sym.get();
            } else {
                return sym.get();
            }
        }
    }
    return globalMatch;
}

std::vector<std::string> userParamNames(const Symbol& sym) {
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

json toSignatureJson(const std::string& label, const std::string& documentation,
                     const std::vector<std::string>& params, int argIndex) {
    SignatureHelp help;
    SignatureInformation sig;
    sig.label = label;
    if (!documentation.empty()) {
        sig.documentation = documentation;
    }
    for (const auto& p : params) {
        sig.parameters.push_back(ParameterInformation{p, std::nullopt});
    }
    help.signatures.push_back(std::move(sig));
    help.activeSignature = 0;
    int active =
        params.empty() ? 0 : std::min(argIndex, (int)params.size() - 1);
    help.activeParameter = active;
    return help;
}

} // namespace

std::optional<SignatureCall> enclosingCall(const std::string& text,
                                           std::size_t offset) {
    offset = std::min(offset, text.size());

    // Forward scan to the cursor: the paren stack top is the enclosing call.
    // Strings hold escapes and span lines; only `//` comments exist.
    std::vector<std::size_t> stack;
    bool inString = false;
    bool inComment = false;
    for (std::size_t i = 0; i < offset;) {
        const char c = text[i];
        if (inComment) {
            if (c == '\n') {
                inComment = false;
            }
            ++i;
            continue;
        }
        if (inString) {
            if (c == '\\' && i + 1 < offset) {
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            inString = true;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < offset && text[i + 1] == '/') {
            inComment = true;
            i += 2;
            continue;
        }
        if (c == '(') {
            stack.push_back(i);
        } else if (c == ')') {
            if (!stack.empty()) {
                stack.pop_back();
            }
        }
        ++i;
    }
    if (inString || inComment || stack.empty()) {
        return std::nullopt;
    }
    const std::size_t open = stack.back();

    std::size_t j = open;
    while (j > 0 && isSpace(text[j - 1])) {
        --j;
    }
    const std::size_t nameEnd = j;
    while (j > 0 && isWordChar(text[j - 1])) {
        --j;
    }
    if (j == nameEnd) {
        return std::nullopt;
    }
    const std::string callee = text.substr(j, nameEnd - j);

    std::optional<std::string> receiver;
    std::size_t k = j;
    while (k > 0 && isSpace(text[k - 1])) {
        --k;
    }
    if (k > 0 && text[k - 1] == '.') {
        --k;
        while (k > 0 && isSpace(text[k - 1])) {
            --k;
        }
        const std::size_t recvEnd = k;
        while (k > 0 && isWordChar(text[k - 1])) {
            --k;
        }
        if (k == recvEnd) {
            return std::nullopt;
        }
        receiver = text.substr(k, recvEnd - k);
    }

    // Top-level commas between the paren and the cursor select the argument.
    int commas = 0;
    int depth = 0;
    bool sStr = false;
    bool sComment = false;
    for (std::size_t i = open + 1; i < offset;) {
        const char c = text[i];
        if (sComment) {
            if (c == '\n') {
                sComment = false;
            }
            ++i;
            continue;
        }
        if (sStr) {
            if (c == '\\' && i + 1 < offset) {
                i += 2;
                continue;
            }
            if (c == '"') {
                sStr = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            sStr = true;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < offset && text[i + 1] == '/') {
            sComment = true;
            i += 2;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) {
                --depth;
            }
        } else if (c == ',' && depth == 0) {
            ++commas;
        }
        ++i;
    }
    if (sStr || sComment) {
        return std::nullopt;
    }
    return SignatureCall{callee, receiver, open, commas};
}

json signatureHelpFor(const DocumentModel& model, std::size_t offset) {
    const std::string& text = model.text();
    const auto call = enclosingCall(text, offset);
    if (!call) {
        return nullptr;
    }

    if (call->receiver) {
        const StdlibEntry* e = nullptr;
        if (*call->receiver == "math") {
            e = mathMemberDoc(call->callee);
        } else {
            e = methodDoc(call->callee);
        }
        if (e == nullptr || e->arity == kArityConstant) {
            return nullptr;
        }
        return toSignatureJson(
            std::string(e->signature), std::string(e->description),
            paramNamesFromSignature(e->signature), call->argIndex);
    }

    if (const StdlibEntry* e = stdlibGlobalDoc(call->callee)) {
        if (e->arity == kArityConstant) {
            return nullptr;
        }
        return toSignatureJson(
            std::string(e->signature), std::string(e->description),
            paramNamesFromSignature(e->signature), call->argIndex);
    }

    if (const Symbol* sym =
            findUserFunction(model, call->callee, call->openParen)) {
        const std::vector<std::string> params = userParamNames(*sym);
        std::string label = "fun " + sym->name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            label += params[i];
            if (i + 1 < params.size()) {
                label += ", ";
            }
        }
        label += ")";
        std::string doc;
        const std::string& full = model.text();
        std::size_t ls = full.rfind('\n', sym->declaration.offset == 0
                                              ? 0
                                              : sym->declaration.offset - 1);
        ls = (ls == std::string::npos) ? 0 : ls + 1;
        std::size_t le = full.find('\n', sym->declaration.offset);
        le = (le == std::string::npos) ? full.size() : le;
        doc = trimSpaces(full.substr(ls, le - ls));
        return toSignatureJson(label, doc, params, call->argIndex);
    }

    return nullptr;
}

} // namespace loxpp::lsp
