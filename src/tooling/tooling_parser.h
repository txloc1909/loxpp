#pragma once

#include <cstddef>
#include <string>

#include "tooling/ast.h"

namespace loxpp::tooling {

// Upper bound on the depth of the returned tree along any root-to-leaf path.
// The parser counts both recursive-descent nesting and the length of loop-
// built left-leaning chains (operator ladders, the postfix call chain) on one
// counter; when the sum reaches this value it stops extending that path,
// records an error, recovers, and still returns a Program. The bound lets a
// consumer walk or destroy the tree recursively on a small stack -- an LSP
// worker thread near 512 KB -- without overflowing. The resolver and the
// LSP server rely on it; see the comment in tooling_parser.cpp for the
// rationale.
inline constexpr std::size_t kMaxTreeDepth = 500;

// Parse Lox++ source into an editor-tooling AST.
//
// This parser never throws and never writes to any stream. On malformed
// input it recovers -- it synchronises to the next `;`, `}`, or declaration-
// keyword start -- and returns a partial Program. Every identifier, `.name`,
// call, and scope boundary in the input is represented with a correct span,
// even when the surrounding structure is broken.
//
// The returned Program borrows `source`: its string_view members point into
// that buffer, so `source` must outlive the Program.
Program parse(const std::string& source);

} // namespace loxpp::tooling
