#pragma once

// Hover and completion text for the Lox++ standard library.
//
// One table, keyed by name, covering every global in
// src/tooling/stdlib_names.h (22 globals + 25 `math` members) plus the
// built-in Map and File methods. The descriptions are short and follow
// spec/05-stdlib.md. src/tooling/stdlib_names.h stays the single source of
// truth for the name set; stdlibDocsMissingNames() proves this table has an
// entry for every name in it.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loxpp::lsp {

// arity sentinels for entries that are not a plain fixed-arity function.
inline constexpr int kArityVariadic = -1;
inline constexpr int kArityConstant = -2;

struct StdlibEntry {
    std::string_view name;
    std::string_view signature;   // for example "len(seq) -> Number"
    int arity = 0;                // >= 0, or a sentinel above
    std::string_view description; // one line, from spec/05-stdlib.md
};

// A global function or object (`clock`, `math`, ...).
[[nodiscard]] const StdlibEntry* stdlibGlobalDoc(std::string_view name);

// A `math.<name>` member (function or constant).
[[nodiscard]] const StdlibEntry* mathMemberDoc(std::string_view name);

// A `coroutine.<name>` member (`create`).
[[nodiscard]] const StdlibEntry* coroutineMemberDoc(std::string_view name);

// The members of a global object that a receiver name reaches (`math`,
// `coroutine`, `task`), or nullptr for any other receiver. Completion, hover,
// and signature help all ask this one lookup, so a global object has docs in
// all three or in none.
[[nodiscard]] const std::vector<StdlibEntry>*
receiverMemberDocs(std::string_view receiver);

// One member of such a global object, or nullptr.
[[nodiscard]] const StdlibEntry* receiverMemberDoc(std::string_view receiver,
                                                   std::string_view name);

// A built-in Map or File method (`keys`, `readline`, ...). Map and File method
// names do not overlap each other, the math members, or the globals. Includes
// the Coroutine methods `resume` and `status`.
[[nodiscard]] const StdlibEntry* methodDoc(std::string_view name);

// Every entry, for completion lists.
[[nodiscard]] const std::vector<StdlibEntry>& allGlobalDocs();
[[nodiscard]] const std::vector<StdlibEntry>& allMathMemberDocs();
[[nodiscard]] const std::vector<StdlibEntry>& allCoroutineMemberDocs();
[[nodiscard]] const std::vector<StdlibEntry>& allMethodDocs();

// Renders an entry as Markdown for a hover panel.
[[nodiscard]] std::string renderHover(const StdlibEntry& entry);

// Names in src/tooling/stdlib_names.h with no entry in this table. Empty when
// the table is complete; used by the LSP unit test and logged at startup.
[[nodiscard]] std::vector<std::string> stdlibDocsMissingNames();

} // namespace loxpp::lsp
