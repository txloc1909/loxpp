#pragma once

// Quick fixes behind the textDocument/codeAction handler (see lsp/server.h).
//
// Two fixes in v1, both built from data the server already holds:
// - A compiler exhaustiveness diagnostic names the missing enum constructors.
//   The fix inserts one `case <Name> => nil` arm per missing constructor just
//   before the match's closing brace. A bare `case Name` matches any payload,
//   so the arm compiles whatever the constructor's arity is.
// - A `+` binary expression at the request offset. Lox++ has no implicit type
//   coercion (spec/03-types.md), so mixing a String and a Number under `+` is
//   a runtime error; the fix wraps one operand in `str()`. This one is
//   cursor-driven rather than diagnostic-driven: the operand types are only
//   known at runtime, so no compiler diagnostic ever names them.
//
// All offsets are byte offsets into the document text. Source is ASCII
// (spec/01-lexical.md), so a byte offset equals an LSP character.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace loxpp::tooling {
class DocumentModel;
struct BinaryExpr;
} // namespace loxpp::tooling

namespace loxpp::lsp {

using json = nlohmann::json;

// One edit with offsets into the original document text. Most fixes are
// insertions (length == 0); the match-arm fix also folds the brace indent
// into a ranged edit. Applying several edits means applying them in
// descending offset order so earlier offsets stay valid.
struct SourceEdit {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string newText;
};

struct QuickFix {
    std::string title;
    std::vector<SourceEdit> edits;
    // Index into the `context.diagnostics` array this fix answers, or
    // nullopt for cursor-driven fixes (the str() wraps).
    std::optional<std::size_t> diagnosticIndex;
};

// The payload of "Non-exhaustive match on enum 'E': missing arms for: A, B".
struct MissingMatchArms {
    std::string enumName;
    std::vector<std::string> missing;
};

// Parses an exhaustiveness message. Returns nullopt for any other message,
// for an empty constructor list, or for a name that is not a plain
// identifier (the names are spliced into source, so they must be safe).
[[nodiscard]] std::optional<MissingMatchArms>
parseNonExhaustiveMatch(const std::string& message);

// One fix per exhaustiveness diagnostic in `contextDiagnostics` whose message
// parses and whose range sits inside a `match` expression. `requestOffset`
// is only the fallback anchor when a diagnostic range does not parse.
// The fix trusts the diagnostic: it finds the match by position and never
// checks the named enum against it, so a stale diagnostic can offer arms
// for the wrong enum. A repeated diagnostic yields one fix, not two.
[[nodiscard]] std::vector<QuickFix>
matchExhaustivenessFixes(const tooling::DocumentModel& model,
                         std::size_t requestOffset,
                         const json& contextDiagnostics);

// Up to two fixes (wrap the left / wrap the right operand) when the
// innermost `+` expression covers `requestOffset`. An operand that is already
// a `str(x)` call gets no wrap; when both are, or there is no `+` here, the
// result is empty.
[[nodiscard]] std::vector<QuickFix>
strWrapFixes(const tooling::DocumentModel& model, std::size_t requestOffset);

} // namespace loxpp::lsp
