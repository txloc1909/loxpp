#pragma once

// Global slots for the QBE backend (#592): the one ordered list of global
// names a compiled program uses. The emitter gives each name an index, and
// the runtime predeclares the same list in the same order, so a compiled
// GET_GLOBAL/SET_GLOBAL/DEFINE_GLOBAL is an index into the Runtime's own
// slot array instead of a name lookup. Sorting is what keeps the two sides
// in agreement without a shared protocol: both derive the list from the
// same recompiled function tree.

#include <string>
#include <vector>

struct DecodedFunction;

namespace qbe {

// Every global name named by a GET_GLOBAL, SET_GLOBAL, or DEFINE_GLOBAL in
// `root` or any nested function, sorted and deduplicated. Index = slot.
// Throws std::runtime_error if such an instruction's constant is not a
// string (decoder/compiler drift).
std::vector<std::string> collectGlobalNames(const DecodedFunction& root);

} // namespace qbe
