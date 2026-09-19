#pragma once

// Signature help for call arguments.
//
// enclosingCall() finds the innermost unmatched `(` before `offset` with a
// forward scan, so strings and `//` comments never confuse the paren match.
// signatureHelpFor() resolves that call against the stdlib doc tables and the
// in-scope user functions and returns the LSP SignatureHelp JSON value, or
// null when the cursor is not in a known call.

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "tooling/document_model.h"

namespace loxpp::lsp {

using json = nlohmann::json;

struct SignatureCall {
    std::string callee;
    std::optional<std::string> receiver;
    std::size_t openParen = 0;
    int argIndex = 0;
};

// The call whose argument list holds `offset`, or nullopt for plain grouping
// parens, a cursor in a string or comment, or no open paren at all.
[[nodiscard]] std::optional<SignatureCall>
enclosingCall(const std::string& text, std::size_t offset);

// Null JSON when no known signature applies; otherwise the SignatureHelp
// object with one signature and a clamped activeParameter.
[[nodiscard]] json signatureHelpFor(const tooling::DocumentModel& model,
                                    std::size_t offset);

} // namespace loxpp::lsp
