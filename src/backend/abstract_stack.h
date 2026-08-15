#pragma once

// Abstract-stack reconstruction (node N2 of the JVM/CLR backend DAG). clox
// fuses named locals and expression temporaries into one operand stack; the
// JVM and CLR keep them apart (a local-variable array vs. an operand stack).
// This pass symbolically executes a chunk to recover, at every offset, which
// stack positions are locals and which are temporaries — the fact the
// compiler discards (notes/bytecode-translation-problems.md, P1).
//
// Target-independent: no JVM or CLR knowledge. Both backends need the same
// numbers, and the CLR backend must hand-compute `.maxstack` from this where
// jasmin would otherwise do it for the JVM.

#include "chunk_decoder.h"

#include <cstdint>
#include <string>
#include <vector>

// What a POP instruction discards. Two byte-identical POPs can mean opposite
// things (P1): TEMP ends an expression-statement result and must become a
// real `pop`; LOCAL_RECLAIM ends a named local's scope and must be dropped —
// a JVM/CLR local slot needs no pop to go out of scope.
enum class PopKind : std::uint8_t { TEMP, LOCAL_RECLAIM };

// The abstract stack immediately before or after an instruction runs.
// `height` counts every live cell (locals and temporaries together).
// `localCount` is how many of the bottom cells are currently bound to a
// declared variable; positions [localCount, height) are temporaries. clox's
// scoping is strictly LIFO, so locals are always exactly the bottom cells —
// no per-cell tag is needed, only these two numbers.
struct StackState {
    int height{0};
    int localCount{0};

    // What the JVM/CLR operand stack actually holds here, once locals move
    // to slots and stop occupying stack cells.
    [[nodiscard]] int operandDepth() const { return height - localCount; }

    bool operator==(const StackState&) const = default;
};

// An offset where a value already on the stack becomes a named local with no
// store instruction (P1's "invisible var" — `var a = 1;` compiles to just
// CONSTANT; the compiler privately notes "slot 1 is now a" and emits no
// store). `slot` is the frame-relative local index. The emitter must insert
// a real store (`astore`/`stloc`) exactly at this offset.
struct InvisibleVarSite {
    int offset{0};
    int slot{0};
};

// One POP instruction, classified.
struct PopClassification {
    int offset{0};
    PopKind kind{};
};

// The full per-instruction analysis of one function's own chunk.
struct FunctionStackAnalysis {
    std::string functionId;

    // Aligned 1:1 with the source DecodedFunction::instructions.
    std::vector<StackState> before;
    std::vector<StackState> after;

    // Aligned 1:1 with instructions too. False for an instruction no path
    // from function entry reaches — `before`/`after` are meaningless there.
    // `compiler.cpp`'s endCompiler() unconditionally appends a trailing
    // NIL;RETURN even when every path already returned explicitly, so this
    // does happen in real chunks; it is not a decoder or analysis bug.
    std::vector<bool> reached;

    // In offset order.
    std::vector<PopClassification> pops;

    // In offset order, deduplicated: the same declaration can be reachable
    // from more than one predecessor (e.g. a captured local initialised by
    // a short-circuit `and`/`or` expression lands via either branch).
    std::vector<InvisibleVarSite> invisibleVars;

    // High-water mark of operandDepth() over the whole chunk — what a JVM
    // `.limit stack` / CIL `.maxstack` must be at least as large as. Excludes
    // the arity+1 bottom slots: those move to the local array, not the
    // operand stack. Counts an invisible-var's declaring push itself (the
    // emitter's store has not run yet at that instruction), so it is a safe
    // lower bound, not the final number: an emission strategy that needs its
    // own transient cell beyond what this pass sees must add it on top of
    // this number before using it as `.limit stack`/`.maxstack`. That is
    // N4's responsibility, not this node's. Two known cases, neither
    // raising the bound on any of the 603 corpus chunks measured so far:
    // an emission-strategy choice (e.g. the `dup` a P2 shuffle lowering
    // adds), and a load this pass's own labels force — when an instruction
    // pops a cell this pass calls a named local (RETURN does, at 33 sites
    // across examples/ and bootstrap/loxpp_interpreter.lox;
    // bytecode-translation-problems.md records the count), the emitter must
    // load that local first, and that load needs a real operand cell this
    // pass does not count.
    int maxOperandDepth{0};
};

// Analyzes one function's own chunk. Does not recurse into nested functions —
// each has its own frame, analyzed independently, starting from
// height = arity+1 (slot 0 = callee/receiver, slots 1..arity = parameters;
// bytecode-translation-problems.md P5). A method's receiver and a plain
// function's closure occupy slot 0 the same way, so no special-casing is
// needed between them.
//
// Throws std::runtime_error if a control-flow merge disagrees on operand
// depth (height minus local count) — the invariant the JVM/CLR verifier
// enforces at every merge. Raw height and local count may legitimately
// differ across incoming edges (e.g. two `match` arms that destructure a
// different number of pattern fields); operand depth may not. A
// disagreement there means the decoder or this analysis has drifted from
// the compiler; the input program is trusted to be compiler-correct.
//
// At such a merge, `before[i].height` and `before[i].localCount` are each
// the maximum over incoming edges, taken independently — an upper bound
// that need not match any single incoming edge's own raw pair. Two measured
// cases: `bootstrap/loxpp_interpreter.lox`'s `resolveStmt` (offset 705) has
// edges (h=3,L=3) and (h=4,L=4); `execStmt` (offset 630) has two edges both
// (h=4,L=4) yet `before[630]` reports (5,5), so a third, unlisted edge
// supplies the max on both dimensions there. Read `before[i].operandDepth()`
// as exact on every edge (the property just above proves it); do not read
// `before[i].height` or `before[i].localCount` alone as one edge's exact
// state at a merge.
//
// N1 (CFG/label recovery) has landed (src/backend/cfg.h, cfg.cpp, PR #100).
// This analysis still computes its own private local leaders/edges (see
// abstract_stack.cpp) instead of depending on N1's builder, per N2.md's
// hazards section: N5 unifies the two builders later, once both exist.
FunctionStackAnalysis analyzeStack(const DecodedFunction& fn);

// One node of the whole-tree analysis, mirroring DecodedFunction's shape.
struct StackAnalysisTree {
    FunctionStackAnalysis self;
    std::vector<StackAnalysisTree> nested;
};

StackAnalysisTree analyzeStackTree(const DecodedFunction& root);

// True for opcodes where the assigned/tested value survives as the
// instruction's own result — "assignment is an expression" (vm.cpp) plus
// JUMP_IF_FALSE's condition peek. This is *not* "the stack shape is
// unchanged": SET_PROPERTY and SET_INDEX consume more than they leave
// behind (stackEffect: pop 2 push 1, pop 3 push 1 respectively) — they
// belong to this family because the *value being assigned*, not the whole
// input, reappears as the result. Only SET_LOCAL, SET_GLOBAL, SET_UPVALUE,
// and JUMP_IF_FALSE leave the stack shape itself unchanged (pop 0, push 0).
// The short-circuit `and`/`or` idiom is not a separate opcode; it is
// exactly a JUMP_IF_FALSE peek paired with a POP on the truthy path
// (bytecode-translation-problems.md P2).
bool peeksInsteadOfPops(Op op);

// Internal to analyzeStack's own implementation (R11's safety-net guard,
// abstract_stack.cpp). Declared here, not in the anonymous namespace it once
// sat in, only so test_backend_abstract_stack.cpp can drive it directly
// with a hand-built `declaredSlotsAt` (R15) — analyzeStack's own discovery
// never produces the gap this checks for, so no real chunk can reach this
// path through analyzeStack alone. Not part of the public analysis API:
// N4/N5/N6 must not call this.
void validateNoInvisibleVarGaps(
    const std::vector<DecodedInstruction>& ins,
    const std::vector<StackState>& before, const std::vector<bool>& reached,
    const std::vector<std::vector<int>>& declaredSlotsAt,
    const std::string& functionId);
