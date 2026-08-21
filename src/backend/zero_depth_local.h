#pragma once

// The depth-0 named-local resolution rule, target-independent and shared by
// every backend that lowers a P2 peek-family consumer (SET_LOCAL,
// SET_GLOBAL, SET_UPVALUE, JUMP_IF_FALSE — see
// notes/bytecode-translation-problems.md) once its own operand depth
// (abstract_stack.h) is zero: the value to consume is not a genuine
// operand-stack temporary, because the shared abstract-stack analysis
// already folded it into a named local (P2's eager invisible-var
// materialization).
//
// Away from a CFG merge, `exactLocalCountMinusOne`
// (analysis.before[i].localCount - 1) names that local exactly. AT a merge,
// `localCount` is only an UPPER BOUND (abstract_stack.h), so this
// cross-checks it against a second, independently derived estimate that
// every backend already tracks: the most recently DECLARED invisible-var
// slot, updated by a forward walk in offset order as each backend inserts
// its own invisible-var stores. Agreement confirms the bound was exact this
// time; disagreement means it was not — this throws instead of silently
// resolving to the wrong slot, which the JVM backend was proven to do
// silently before this cross-check existed (a plain, unnested `match`
// disagreeing with `build/loxpp` with exit code 0 and no diagnostic).
//
// One authority for this rule, not one private copy per backend
// (notes/... brief.md section 3): do not re-derive this cross-check
// independently inside a new emitter.
int resolveZeroDepthLocalSlot(int exactLocalCountMinusOne, bool atCfgMergeLabel,
                              int lastInvisibleVarSlot, int offset,
                              const char* backendTag);
