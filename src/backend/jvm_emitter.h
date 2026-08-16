#pragma once

// JVM code generator (node N4 of the JVM/CLR backend DAG, discharges P2 — the
// "peek, don't pop" family; node N5 adds P3b, control flow and verifier
// legality; node N6 adds P5, functions and calls; node N7 adds P4b, closures
// and upvalues; node N8 adds P5+P4, classes/methods/super;
// notes/bytecode-translation-problems.md). Lowers one function's chunk to
// Jasmin (.j) text, consuming N0's decoder, N1's CFG/labels, N2's
// abstract-stack analysis, and N3's capture analysis. Match/enum dispatch
// proper (N10: GET_TAG, JUMP_TABLE) is still out of scope: this node aborts
// loudly on any opcode none of N4/N5/N6/N7/N8 lowers, so a later node's gap
// fails loudly too.
//
// N7 also lowers BUILD_LIST/GET_INDEX/SET_INDEX — three opcodes
// notes/backend-implementation-dag.md assigns to N9 (aggregates and
// for-in), pulled forward here because N7's own checkpoint
// (notes/translation-probes/V1_fresh_cell.lox, V3_loopvar.lox) cannot run
// to completion without them: both build a list of the closures under test
// and read it back by index. N9 adds the rest of its own scope on top:
// BUILD_MAP, SLICE, IN, IS_SEQ, and the for-in iterator protocol
// (GET_ITER/ITER_HAS_NEXT/ITER_NEXT) — see emitBuildList's and
// emitBuildMap's own notes.
//
// N8 also lowers MATCH_ERROR, an N10 opcode pulled forward for the same
// reason: a `match` whose arms are all class patterns (no literal wildcard
// or plain binding) compiles a real, reachable MATCH_ERROR, because the
// compiler never proves a class pattern exhaustive over its own subclasses
// — examples/class_dispatch.lox is exactly this shape, and this node's own
// checkpoint cannot run to completion without it. N10 still owns GET_TAG
// and JUMP_TABLE, the enum-tag dispatch fast path — see emitMatchError's
// own note.
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
// only in a chunk that contains a CALL with at least one argument, a
// BUILD_LIST with at least one element, or a BUILD_MAP with at least one
// pair — one more scratch slot for the callee plus one per spilled value
// the chunk's widest such instruction needs (P7: the values are already on
// the operand stack below where a fresh array reference would land, so
// building the Object[] needs every value spilled to a local first;
// BUILD_MAP's own width is twice its pair count, one slot each for key and
// value).

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
// chunk in the tree constructs with a CLOSURE, with its upvalues wired
// (node N7). `root`/`tree` must come from the same compiled tree
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
