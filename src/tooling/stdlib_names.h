#pragma once

// The stdlib global name set, and the members of the `math` object.
//
// Single source of truth for both the resolver (N7 -- decides whether an
// unresolved reference is a known global or a genuine unknown) and the LSP
// server (N8 -- hover and completion). Cross-checked against src/stdlib/*.cpp
// and src/math.cpp; see the PR body for the grep.

#include <span>
#include <string_view>

namespace loxpp::tooling {

// Global functions and objects the stdlib installs before a program runs.
[[nodiscard]] std::span<const std::string_view> stdlibGlobals();

// Members reachable as `math.<name>` (functions and constants).
[[nodiscard]] std::span<const std::string_view> mathMembers();

[[nodiscard]] bool isStdlibGlobal(std::string_view name);
[[nodiscard]] bool isMathMember(std::string_view name);

} // namespace loxpp::tooling
