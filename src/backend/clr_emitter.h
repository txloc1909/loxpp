#pragma once

// CLR code generator. Covers CONSTANT (number, string), NIL/TRUE/FALSE, the
// arithmetic and comparison family, NEGATE, NOT, PRINT, POP, GET_LOCAL,
// SET_LOCAL, DEFINE_GLOBAL, GET_GLOBAL, SET_GLOBAL, RETURN in its script
// form, and JUMP/JUMP_IF_FALSE/LOOP (P3b) — see
// notes/bytecode-translation-problems.md for what each P-number means.
//
// Scope: no calls, no closures, no classes, no aggregates, no match. Every
// opcode outside that set throws std::runtime_error, naming the opcode,
// instead of falling through silently — a later CLR emission node lowers
// it for real.
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
// cross-check (brief.md section 3: one shared authority, not two) — this
// pass calls into it rather than re-deriving its own copy, the same way
// jvm_emitter.cpp does.

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
// `className` with a static, parameterless `Main` entry point.
// `analysis` must come from `analyzeStack` on the same `fn`. Does not
// recurse into nested functions — CLOSURE is not lowered by this pass (a
// lone chunk has no class name to give a nested function's target
// anyway); a chunk that contains one throws when the dispatch loop reaches
// it, the same as any other opcode outside this file's scope.
std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className);

} // namespace clr
