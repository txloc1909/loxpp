#pragma once

// JVM code generator (node N4 of the JVM/CLR backend DAG, discharges P2 — the
// "peek, don't pop" family, notes/bytecode-translation-problems.md). Lowers
// one function's straight-line chunk to Jasmin (.j) text, consuming N0's
// decoder and N2's abstract-stack analysis. No control flow (N5), no calls or
// closures (N6/N7): this node handles only the opcodes named in N4.md, and
// aborts loudly on any other opcode so a later node's gap fails loudly too.
//
// JVM local-variable layout (script form): slot 0 is the `main` method's
// `String[] args` parameter, slot 1 holds the LoxGlobals instance, slots
// [2, 2+maxLocalCount) mirror the Lox++ frame's own slots 1:1 (frame slot 0 —
// the never-named callee/receiver — gets a JVM slot too, just an unused one),
// and one final scratch slot holds a peeked SET_GLOBAL's value across the
// LoxGlobals.set() call (java.lang.String/Object have no "leave a value"
// overload to call instead).

#include "abstract_stack.h"
#include "chunk_decoder.h"

#include <string>

namespace jvm {

// Escapes one raw Lox++ string (an arbitrary byte sequence — spec/03-types.md
// calls it ASCII, but the scanner does not enforce that) into a Jasmin
// `ldc "..."` literal body, not including the surrounding quotes. Every byte
// outside printable, quote-safe ASCII becomes a `\NNN` octal escape, so the
// emitted .j file never carries a raw control or high byte that a text-mode
// read could reinterpret.
std::string escapeJasminString(const std::string& raw);

// Formats a double as a Jasmin `ldc2_w` operand that reads back to the exact
// same bit pattern. 17 significant decimal digits round-trip any IEEE-754
// double (Steele & White); a literal with no '.' or 'e' reads as an integer
// to some assemblers, so one is added when the digits alone would omit it.
std::string formatJasminDouble(double value);

// Emits complete Jasmin source for the top-level script, as class
// `className` (the CLI always passes "LoxMain"). `fn` and `analysis` must
// come from the same compiled tree (decodeFunctionTree / analyzeStackTree on
// the same ObjFunction).
//
// Throws std::runtime_error, with a message prefixed "not implemented in N4:
// ", on any opcode this node does not lower — includes CLOSURE, so a program
// with a nested `fun`/method aborts here rather than silently dropping it
// (function bodies are N6's responsibility, not this node's).
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className);

} // namespace jvm
