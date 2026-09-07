#pragma once

#include <string>

#include "tooling/ast.h"

namespace loxpp::tooling {

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
