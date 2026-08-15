#pragma once

// JVM code generator (node N4 of the JVM/CLR backend DAG, discharges P2 — the
// "peek, don't pop" family; node N5 adds P3b, control flow and verifier
// legality; node N6 adds P5, functions and calls;
// notes/bytecode-translation-problems.md). Lowers one function's chunk to
// Jasmin (.j) text, consuming N0's decoder, N1's CFG/labels, and N2's
// abstract-stack analysis. Upvalue wiring (N7), classes (N8), aggregates and
// for-in (N9), and match/enum (N10) are still out of scope: this node aborts
// loudly on any opcode or CLOSURE shape none of N4/N5/N6 lowers, so a later
// node's gap fails loudly too.
//
// JVM local-variable layout — one fixed mapping for both entry shapes
// (nodes/N6.md: "choose a fixed slot mapping ... and use it everywhere"),
// differing only in how many JVM-only slots come before the Lox frame's own:
//
//   script (`main`):   slot 0 = String[] args, slot 1 = LoxGlobals.
//   function (`invoke`): slot 0 = `this` (the LoxFn$<n> instance), slot 1 =
//     `self` (the callee/receiver the Lox chunk's own frame slot 0 wants),
//     slot 2 = Object[] args, slot 3 = LoxGlobals (from
//     `LoxRuntime.current()`; a generated class has no field of its own for
//     it; see jvm_emitter.cpp). A prologue copies `self` into the Lox
//     frame's own slot-0 mirror and unpacks `args[i]` into slot i+1's.
//
// Either way, the Lox frame's own slots [0, maxLocalCount) mirror 1:1 into
// the JVM locals right after those fixed slots, then one scratch slot holds
// a peeked SET_GLOBAL's value across the LoxGlobals.set() call (P2), and —
// only in a chunk that contains a CALL with at least one argument — one more
// scratch slot for the callee plus one per argument the chunk's widest CALL
// needs (P7: the args are already on the operand stack below where a fresh
// array reference would land, so building the Object[] needs every value
// spilled to a local first).

#include "abstract_stack.h"
#include "chunk_decoder.h"

#include <string>
#include <vector>

namespace jvm {

// Escapes one raw Lox++ string (an arbitrary byte sequence — spec/03-types.md
// calls it ASCII, but the scanner does not enforce that) into a Jasmin
// `ldc "..."` literal body, not including the surrounding quotes. Every byte
// outside printable, quote-safe ASCII becomes a `\NNN` octal escape, so the
// emitted .j file never carries a raw control or high byte that a text-mode
// read could reinterpret.
std::string escapeJasminString(const std::string& raw);

// Formats a double's raw IEEE-754 bit pattern as a Jasmin `ldc2_w` operand: a
// bare decimal `long` literal, paired at the call site with
// `invokestatic java/lang/Double/longBitsToDouble(J)D` to turn it back into
// the exact original double.
//
// A decimal/exponent literal cannot do this job (PR #107 R6, R7): jasmin 2.4
// parses an `ldc2_w` operand that has a '.' or an 'e' at 32-bit float
// precision, then widens it — silently rounding every value a float cannot
// hold exactly — and it rejects some valid `%g`-style exponent text outright
// (`1e+17`, no decimal point) with "Badly formatted number". A bare integer
// literal has neither defect: jasmin reads it as a `long`, verbatim.
std::string formatDoubleBitsLiteral(double value);

// One generated compilation unit: the Jasmin source for one class, and the
// class name its own `.class` directive declares. tools/jvm_run.sh derives
// the expected `.class` file from that same directive and checks it was
// really written, so `source`'s directive and `className` must agree — the
// CLI (main.cpp) writes each one out as "<className>.j".
struct EmittedClass {
    std::string className;
    std::string source;
};

// Emits the whole program reachable from `root`: the top-level script as
// `scriptClassName`, plus one `LoxFn$<n>` class per function or method any
// chunk in the tree constructs with a zero-upvalue CLOSURE (node N6 — a
// CLOSURE that captures anything throws "not implemented", upvalue wiring is
// N7). `root`/`tree` must come from the same compiled tree
// (decodeFunctionTree / analyzeStackTree on the same ObjFunction).
//
// Class names are assigned by one fixed pre-order walk of the decoded tree,
// so two runs on the same compiled program always agree (brief.md section 9,
// "deterministic naming") — this is the driver a real `loxpp --target jvm`
// run uses; emitScript below is single-chunk and test-facing only.
std::vector<EmittedClass> emitProgram(const DecodedFunction& root,
                                      const StackAnalysisTree& tree,
                                      const std::string& scriptClassName);

// Emits complete Jasmin source for one chunk only, as the top-level script
// class `className`. Equivalent to the one `EmittedClass` emitProgram would
// produce for `root`, for a program that declares no nested function —
// kept so the pre-N6 test suite (and any test that only needs one script
// chunk's own text) does not have to unpack emitProgram's vector. A CLOSURE
// throws "not implemented" here, because a lone chunk has no class name to
// give the target; drive emitProgram instead for a program that has one.
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className);

} // namespace jvm
