#pragma once

// JVM code generator. Covers P2 (the "peek, don't pop" family), P3b (control
// flow and verifier legality), P5 (functions and calls), P4b (closures and
// upvalues), P4 (classes/methods/super), aggregates and for-in, and P8
// (match/enum dispatch proper: GET_TAG, JUMP_TABLE, enum-ctor CONSTANT) — see
// notes/bytecode-translation-problems.md for what each P-number means. Match/
// enum dispatch carries its own residue: a `match` expression's result can
// reach ANY consumer opcode as a value the native VM treats as already on the
// operand stack, when the abstract-stack analysis (abstract_stack.cpp)
// instead folded it into a named local (compileMatchBody's own "fused local/
// operand-stack model").
//
// The fix is stated as a STRUCTURAL claim, not a per-opcode enumeration:
// earlier per-opcode-shape enumerations of "which consumers need this fixed
// up" kept being disproved by a three-line program, because nothing forced
// such a list to cover every opcode. `native_pops.h`'s `nativePops`,
// target-independent and shared with the CLR backend, is an exhaustive table
// over `Op`, no `default` (clang's `-Wswitch` warns on a missing
// enumerator; this project builds with neither `-Werror` nor `-Wall`, so a
// missing row still compiles and throws only at run time) stating how many
// operand-stack cells `src/vm.cpp` pops for each one;
// `normalizeFoldedOperands`, one pre-dispatch step every instruction gets
// alike (see its own note, above `emitBody`), compares that count against
// abstract_stack.cpp's own `operandDepth()` and repairs exactly the deficit.
// A folded operand can only ever be the BOTTOM-most of an instruction's own
// operands — `compileMatchBody` folds a `match` expression's result into its
// own named local before any later sibling operand is even parsed — so a
// deficit of 1 (the bottom operand missing, every genuine sibling still on
// the real stack) is repaired for every `nativePops`-covered opcode alike,
// not site by site — EXCEPT when the consumer sits on a CFG merge where
// `loadNamedLocalAtZeroDepth`'s own two slot estimates disagree, where it
// stops loudly instead (see the third GAP residue below).
//
// A deficit of 2 or more throws rather than repairing: a program that puts
// a live sibling operand BELOW a match's own subject/result once collided
// with `compileMatchBody`'s own slot allocation on `build/loxpp` itself
// (`compiler.cpp`); that allocation now reserves one phantom local per live
// sibling operand first, so native answers this shape correctly, but this
// file's own repair was not extended to match it — `normalizeFoldedOperands`
// throws a named error citing this file for that case instead of guessing.
// `notes/bytecode-translation-problems.md`'s own GAP entry records the
// measurement.
//
// A separate, REACHABLE gap exists outside this file: `and`/`or` over a
// folded `match` operand fails at analysis time, in abstract_stack.cpp,
// before this pass ever runs — a deliberately deferred gap (see the GAP
// entry in notes/bytecode-translation-problems.md for the full account).
//
// A third, REACHABLE gap exists inside this file. When a folded match result
// is one operand of a consumer that itself sits at an `and`/`or` join label,
// and a LATER sibling operand of the same consumer is the other side of that
// `and`/`or`, `loadNamedLocalAtZeroDepth` computes two disagreeing slot
// estimates for the join and refuses instead of guessing — even though
// `deficit == 1` and `build/loxpp` answers the program correctly. `print
// (match A() {case A => 1 case B => 2}) + (true and 5);` is one repro; see
// the GAP entry in notes/bytecode-translation-problems.md for the rest. The
// failure is loud, not silent, and no required gate reaches it. The real fix
// is per-edge merge verification, a known residue from an earlier design
// pass, still outside this file's scope.
//
// This file also lowers BUILD_LIST/GET_INDEX/SET_INDEX ahead of the rest of
// the aggregates/for-in scope they conceptually belong to
// (notes/backend-implementation-dag.md), because the closures/upvalues
// checkpoint (test/translation-probes/V1_fresh_cell.lox, V3_loopvar.lox)
// cannot run to completion without them: both build a list of the closures
// under test and read it back by index. The rest of that scope layers on
// top: BUILD_MAP, SLICE, IN, IS_SEQ, and the for-in iterator protocol
// (GET_ITER/ITER_HAS_NEXT/ITER_NEXT) — see emitBuildList's and
// emitBuildMap's own notes.
//
// This file also lowers MATCH_ERROR ahead of the rest of the match/enum
// dispatch scope it conceptually belongs to, for the same reason: a `match`
// whose arms are all class patterns (no literal wildcard or plain binding)
// compiles a real, reachable MATCH_ERROR, because the compiler never proves
// a class pattern exhaustive over its own subclasses — examples/
// class_dispatch.lox is exactly this shape, and the classes/methods/super
// checkpoint cannot run to completion without it. GET_TAG and JUMP_TABLE,
// the enum-tag dispatch fast path, lower straight to this same MATCH_ERROR
// call as the `tableswitch`'s own `default` target — see
// emitFusedGetTagJumpTable's own note.
//
// JVM local-variable layout — one fixed mapping for both entry shapes,
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
// A decimal/exponent literal cannot do this job: jasmin 2.4 parses an
// `ldc2_w` operand that has a '.' or an 'e' at 32-bit float
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
// chunk in the tree constructs with a CLOSURE, with its upvalues wired.
// `root`/`tree` must come from the same compiled tree (decodeFunctionTree /
// analyzeStackTree on the same ObjFunction).
//
// Class names are assigned by one fixed pre-order walk of the decoded tree,
// so two runs on the same compiled program always agree ("deterministic
// naming") — this is the driver a real `loxpp --target jvm` run uses;
// emitScript below is single-chunk and test-facing only.
std::vector<EmittedClass> emitProgram(const DecodedFunction& root,
                                      const StackAnalysisTree& tree,
                                      const std::string& scriptClassName);

// Emits complete Jasmin source for one chunk only, as the top-level script
// class `className`. Equivalent to the one `EmittedClass` emitProgram would
// produce for `root`, for a program that declares no nested function —
// kept so the single-chunk test suite (and any test that only needs one script
// chunk's own text) does not have to unpack emitProgram's vector. A CLOSURE
// throws "not implemented" here, because a lone chunk has no class name to
// give the target; drive emitProgram instead for a program that has one.
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className);

} // namespace jvm
