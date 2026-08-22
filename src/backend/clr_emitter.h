#pragma once

// CLR code generator. Covers CONSTANT (number, string, and an enum
// constructor materialised as a fresh `LoxEnumCtor`), NIL/TRUE/FALSE, the
// arithmetic and comparison family, NEGATE, NOT, PRINT, POP, GET_LOCAL,
// SET_LOCAL, DEFINE_GLOBAL, GET_GLOBAL, SET_GLOBAL, JUMP/JUMP_IF_FALSE/LOOP
// (P3b), CALL, CLOSURE (including a captured upvalue), GET_UPVALUE,
// SET_UPVALUE, CLOSE_UPVALUE, RETURN in both of its roles (P5),
// BUILD_LIST/BUILD_MAP/GET_INDEX/SET_INDEX (pulled forward from the
// aggregates scope — see emitBuildList's and emitBuildMap's own notes),
// CLASS/INHERIT/DEFINE_METHOD/GET_PROPERTY/SET_PROPERTY/INVOKE/GET_SUPER/
// SUPER_INVOKE/INSTANCEOF (P5+P4 — `init` returns `this` at the bytecode
// level already, per compiler.cpp's own emitReturn, so this pass needs no
// separate initializer case), MATCH_ERROR, SLICE, IN, IS_SEQ (a match
// sequence pattern's own type check), the for-in iterator protocol
// GET_ITER/ITER_HAS_NEXT/ITER_NEXT (P8 — see emitGetIter's own note for the
// operand-stack hazard specific to GET_ITER), and GET_TAG together with
// JUMP_TABLE (P8's own match/enum dispatch — see emitFusedGetTagJumpTable's
// own note) — see notes/bytecode-translation-problems.md for what each
// P-number means.
//
// A folded operand's repair is not one code path. `native_pops.h`'s own
// `nativePops` table states how many operand-stack cells a consumer reads;
// `normalizeFoldedOperands` (clr_emitter.cpp) repairs any deficit for every
// row that states a plain count, sharing that one table with
// `jvm_emitter.cpp`'s own mechanism of the same name. A row marked CUSTOM
// instead (a value that survives past its own instruction rather than
// being net-popped, or an instruction that already threads its own
// zero-depth load unconditionally) is not `normalizeFoldedOperands`'s
// concern; whichever opcode needs to consume a possibly-folded value that
// way routes through `isFoldedAtZeroDepth`/`loadNamedLocalAtZeroDepth`
// (clr_emitter.cpp) instead — checkable directly against each such
// function's own callers, not against this comment. GET_TAG immediately
// followed by JUMP_TABLE (`fusableJumpTable`, clr_emitter.cpp) lowers to a
// CIL `switch` with an explicit base subtraction ilasm needs and jasmin's
// own `tableswitch` does not; a JUMP_TABLE that reaches the dispatch switch
// unfused falls to the same failure mode as any opcode with neither a case
// in that switch nor a `nativePops` row: `notImplemented` (clr_emitter.cpp)
// throws rather than silently emitting nothing.
//
// A captured local lowers to a one-element `object[]` ref cell (P4). The
// cell allocation is idempotent, not a static declaration-point seed: an
// `isinst object[]` type test runs at every CLOSURE, GET_LOCAL, and
// SET_LOCAL of a captured slot, because a `for` loop's condition or
// increment clause can revisit the slot through the back edge, on a later
// trip, before the slot's own re-declaration runs — program order alone
// cannot fix raw-vs-cell at code-generation time. See ensureCapturedCell's
// own note (clr_emitter.cpp) for the standing counter-example. No Lox value
// is ever itself a bare `object[]` (every aggregate the runtime exposes
// wraps its storage in a named class — LoxOps.GetIndex's own BINDING
// INVARIANT comment, runtime/clr/src), so the type test can never mistake a
// real Lox value for a cell or the reverse.
//
// A function becomes its own CIL class, `extends [LoxRuntime]Lox.LoxClosure`
// (P6's one `Call(object[])` interface — every callable kind implements it,
// so generated code never branches on the callee's kind). Its constructor
// takes the upvalue array (each entry either a freshly-seeded cell or a
// parent's own cell/upvalue, wired by emitClosure) and forwards the
// function's compile-time name/arity to the base class; `Invoke(object
// self, object[] args)` overrides the base class's own abstract slot with
// this chunk's lowered body. `self` (P5: "the callee itself — or, in a
// method, the receiver") and each unpacked `args[i]` copy into the same
// Lox-frame-slot mapping GET_LOCAL/SET_LOCAL already use for the script
// chunk — CIL's own argument slots (`ldarg.1`, `ldarg.2`) hold them only
// long enough for the prologue to copy them in, so (unlike the JVM backend,
// whose local-variable array has no separate argument space) a CLR function
// chunk's local layout is byte-for-byte the SAME as the script chunk's:
// local 0 is always LoxGlobals, and the Lox frame's own slots start at
// local 1 either way.
//
// `emitProgram` assembles the whole reachable tree into ONE ilasm source:
// one shared `.assembly`/`.module` header, then one `.class` block per
// function (the script's own plus one per nested `fun`/method), in one
// fixed pre-order walk — ilasm assembles more than one class from a single
// module without any per-class assembly manifest of its own.
//
// JUMP/LOOP lower to CIL `br` at a label `src/backend/cfg.{h,cpp}` already
// recovered; JUMP_IF_FALSE lowers to `dup; call LoxOps::IsFalsy; brtrue` so
// the peeked condition survives on both outgoing edges (P2/P3). Every value
// is already `object` (CONSTANT's own boxing), which is what lets a merge
// between two different code paths carry a value at all.
//
// This opcode set can still fold a peek-family value's result into a named
// local ahead of a genuine consumer — P2's "eager invisible-var
// materialization" (abstract_stack.h): `{ var a = 1; var b = (a = 2); }`
// folds `b`'s value before the SET_LOCAL that assigns `a` even runs (probes
// 18/19), and `x and (y = 1);` or a local initializer whose value is itself
// a short-circuit expression folds JUMP_IF_FALSE's own condition the same
// way (probes 22/23). Once this pass places its own CFG labels, resolving
// that fold at operandDepth() == 0 needs the same cross-check the JVM
// backend needed the moment ITS jumps could create a merge:
// `before[i].localCount - 1` is only an upper bound AT a CFG merge
// (abstract_stack.h), not an exact slot. `resolveZeroDepthLocalSlot`
// (zero_depth_local.h) is the one, target-independent authority for this
// cross-check: one shared authority, not two. This pass calls into it
// rather than re-deriving its own copy, the same way jvm_emitter.cpp does.

#include "abstract_stack.h"
#include "chunk_decoder.h"

#include <string>

namespace clr {

// Encodes one Lox++ byte string (spec/03-types.md calls it ASCII, but the
// scanner does not enforce that) as a complete ilasm `ldstr` operand:
// `bytearray (b0 00 b1 00 ...)`, one UTF-16LE code unit per byte. Latin-1
// code point N is byte N for every N in [0, 255] (LoxRuntime.Charset), so
// this is exact for the whole byte range with no per-byte case split.
//
// ilasm's own quoted-string escapes cannot represent every byte: a literal
// NUL byte in the source text ends ilasm's own string token early (proven:
// `"a\0b"` prints only `"a"`), and its `\NNN` octal escape reads the
// resulting byte back through its UTF-8 source reader, so an escape for a
// byte at or above 0200 octal comes back as U+FFFD, not that byte's own
// Latin-1 code point (measured against ilasm 8.0.0 in this project's
// dev-managed image). The bytearray form has no such case, so it is used
// for every string, not only the bytes escaping would mishandle.
std::string ilasmStringLiteral(const std::string& raw);

// Formats a double's raw IEEE-754 bit pattern as a complete ilasm `ldc.r8`
// operand: `(b0 b1 ... b7)`, little-endian per ECMA-335's fixed byte order
// for a `ldc.r8` byte-array operand in the IL stream — a rule of the format,
// not a fact about the host. Each byte is produced by shifting the integer
// bit pattern, not by reading host memory, so the output is the same on
// every host regardless of its native byte order. Measured against ilasm
// 8.0.0: its own decimal/exponent `ldc.r8` lexer round-tripped every value
// this pass tried, including
// 16777217.0 (the float-precision trap that forces the JVM/jasmin backend
// to a similar bits-based literal, jvm_emitter.h) — but the byte-array form
// is exact by construction for every double, including subnormals and a
// NaN's own payload bits, so this pass does not depend on ilasm's decimal
// lexer at all.
std::string ilasmDoubleLiteral(double value);

// Emits complete ilasm source for one script chunk, as the top-level class
// `className` with a static `Main(string[] args)` entry point.
// `analysis` must come from `analyzeStack` on the same `fn`. Does not
// recurse into nested functions: a CLOSURE reaching this throws "has no
// assigned class name", because a lone chunk has no name to give a nested
// function's target — kept so the single-chunk test suite does not have
// to build a whole `StackAnalysisTree` for a program with no nested
// function; drive `emitProgram` instead for one that has one.
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className);

// Emits the whole program reachable from `root` as one complete ilasm
// source: the top-level script as `scriptClassName`, plus one `LoxFn$<n>`
// class per function or method any chunk in the tree constructs with a
// CLOSURE, in one fixed pre-order walk of the decoded tree (so two runs on
// the same compiled program always agree — "deterministic naming",
// clr_emitter.h's own hazard). `root`/`tree` must come from the same
// compiled tree (`decodeFunctionTree` / `analyzeStackTree` on the same
// `ObjFunction`). This is the driver a real `loxpp --target clr` run uses;
// `emitScript` above is single-chunk and test-facing only.
std::string emitProgram(const DecodedFunction& root,
                        const StackAnalysisTree& tree,
                        const std::string& scriptClassName);

} // namespace clr
