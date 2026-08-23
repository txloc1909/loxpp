#pragma once

// How many operand-stack cells `src/vm.cpp` pops for one instruction —
// target-independent (P1/P2, notes/bytecode-translation-problems.md): a fact
// about the native VM's own bytecode semantics, not about any backend's own
// lowering. Every backend that reconstructs a folded operand (the shared
// abstract-stack analysis, abstract_stack.h, can fold a value into a named
// local instead of leaving it as a genuine operand-stack temporary) needs
// this same count, so it lives here once rather than once per backend.
//
// Exhaustive `switch` over `Op`, no `default`: a missing row throws below,
// at run time, the first time emission reaches it, rather than silently
// treating an unlisted opcode as needing no repair.
//
// `std::nullopt` marks a CUSTOM row: the peek/locals-model family, where a
// value survives past the instruction instead of being net-popped (P2's own
// family — SET_LOCAL/SET_GLOBAL/SET_UPVALUE/JUMP_IF_FALSE), a reclaim that
// never touches the operand stack at all (POP/CLOSE_UPVALUE), an instruction
// that already threads its own zero-depth load unconditionally with no depth
// check to duplicate (GET_ITER/INHERIT), or pure control transfer that never
// consumes a value (JUMP/LOOP/JUMP_TABLE — JUMP_TABLE is only ever reached
// fused onto a preceding GET_TAG). A caller with its own repair mechanism for
// a CUSTOM row's own opcode keeps working exactly as it already does; this
// function never computes a repair for one.
//
// Every other row states ONE number, and it is always the same measure: how
// many operand-stack cells this instruction READS from the stack, whatever
// it pushes back afterward — not always the instruction's net stack change.
// SET_PROPERTY reads the instance AND the value (2 cells), pops both, then
// pushes the value back — net change -1, row states 2. DEFINE_METHOD reads
// the class AND the closure (2 cells), but pops only the closure and leaves
// the class in place for the next DEFINE_METHOD to find — net change -1, row
// states 2 as well: the class is a real cell this instruction needs
// physically present, even though the instruction itself never removes it.
// GET_SUPER is the same shape again: it pops the superclass itself, then
// binds `this` from underneath it — 2 cells read, 1 pushed back. Read
// straight off `chunk.h`'s own per-opcode comments and confirmed against
// every `vm.cpp` case body, including any helper the case calls.
#include "chunk_decoder.h"

#include <optional>
#include <string>

std::optional<int> nativePops(Op op, const DecodedInstruction& in);

// Every Op enumerator's own spelling, for a diagnostic message only — never
// the disassembly oracle (test_chunk_decoder.cpp owns that). An enumerator
// missing from the switch still returns a name, "UNKNOWN_OP", rather than
// leaving a caller building an error string with nothing to print.
std::string opName(Op op);
