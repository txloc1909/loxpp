#include "lsp/signature_help.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "lsp/model_util.h"
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
        while (start < close && isSpaceChar(sig[start])) {
            ++start;
        }
        std::size_t end = start;
        while (end < close && sig[end] != ',') {
            ++end;
        }
        std::size_t trim = end;
        while (trim > start && isSpaceChar(sig[trim - 1])) {
            --trim;
        }
        if (trim > start) {
            out.emplace_back(sig.substr(start, trim - start));
        }
        start = end + 1;
    }
    return out;
}

// True when an unescaped `"` appears at or after `from`. A string that is
// still open at the cursor with no closer ahead is being typed, not read.
bool hasClosingQuote(const std::string& text, std::size_t from) {
    for (std::size_t i = from; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            i += 2;
            continue;
        }
        if (text[i] == '"') {
            return true;
        }
        ++i;
    }
    return false;
}

// innermostScope + visible-function lookup mirror the completion path: globals
// are late-bound, locals must be declared before the call's open paren.
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
    // A string still open at the cursor with no closer anywhere ahead is
    // being typed, so the call stays visible. A trailing `//` comment never
    // hides the call either; comment text still counts as comment below.
    const bool danglingString = inString && !hasClosingQuote(text, offset);
    if ((inString && !danglingString) || stack.empty()) {
        return std::nullopt;
    }
    const std::size_t open = stack.back();

    std::size_t j = open;
    while (j > 0 && isSpaceChar(text[j - 1])) {
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
    while (k > 0 && isSpaceChar(text[k - 1])) {
        --k;
    }
    if (k > 0 && text[k - 1] == '.') {
        --k;
        while (k > 0 && isSpaceChar(text[k - 1])) {
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
    // A dangling string counts as plain text here; its commas may shift the
    // index by one, which the clamp below absorbs for short signatures.
    const bool trackStrings = !danglingString;
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
        if (trackStrings && c == '"') {
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
    if (sStr) {
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
            // Map and File method names are unique across the tables, so a
            // bare-name match applies whatever the receiver text is. This
            // guesses when the receiver is not a Map or File; hover makes
            // the same guess, and the receiver type is unknown here.
            // User-defined methods through a receiver never resolve.
            e = methodDoc(call->callee);
        }
        if (e == nullptr || e->arity == kArityConstant) {
            return nullptr;
        }
        return toSignatureJson(
            std::string(e->signature), std::string(e->description),
            paramNamesFromSignature(e->signature), call->argIndex);
    }

    // A user definition shadows a stdlib global of the same name.
    if (const Symbol* sym =
            findUserFunction(model, call->callee, call->openParen)) {
        const std::vector<std::string> params = functionParamNames(*sym);
        std::string label = "fun " + sym->name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            label += params[i];
            if (i + 1 < params.size()) {
                label += ", ";
            }
        }
        label += ")";
        // The declaration line up to the body brace, not the whole line.
        std::string doc = lineTextAt(text, sym->declaration.offset);
        const std::size_t brace = doc.find('{');
        if (brace != std::string::npos) {
            doc = trimmed(doc.substr(0, brace));
        }
        return toSignatureJson(label, doc, params, call->argIndex);
    }

    if (const StdlibEntry* e = stdlibGlobalDoc(call->callee)) {
        if (e->arity == kArityConstant) {
            return nullptr;
        }
        return toSignatureJson(
            std::string(e->signature), std::string(e->description),
            paramNamesFromSignature(e->signature), call->argIndex);
    }

    return nullptr;
}

} // namespace loxpp::lsp
