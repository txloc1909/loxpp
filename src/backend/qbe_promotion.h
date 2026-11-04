#pragma once

// S8 (#461): register promotion for the QBE backend. The QBE emitter keeps
// clox's fused stack: every value lives at base + 8*height, and the GC
// (Runtime::markRoots) scans stack..stackTop. A local kept only in a QBE
// SSA value is invisible to the GC, so it must be written back to its stack
// slot before every allocating call ("safe point").
//
// This pass decides which local slots may be promoted and builds the value
// flow the emitter needs to keep them in registers across basic blocks. A
// slot is a candidate when it is never captured (capture_analysis) and it is
// a parameter or a named local (abstract_stack's invisibleVars). The flow is
// a per-block entry map from slot to SSA value, with a QBE phi where
// predecessors disagree; a catch block starts from its PUSH_HANDLER's own
// local set and reloads those slots from memory, because every fallible call
// spills them before it can throw into that catch.
//
// The flow is a *must* analysis, so it starts only where a value exists
// without a predecessor: the prologue for block 0, the reload for a catch
// handler. Every other block is computed from a real predecessor. Starting a
// block from an empty value is not the same thing — it is indistinguishable
// from a genuine "no value on this path" — and it drops the slot in every
// block cycle (a nested loop, or a loop inside a try), where the dropped
// block then reads a stack cell the promoted write never updated (#1052).
// A phi whose predecessors all carry one value collapses onto that value, so
// such a cycle settles on the value it carries in from outside.
//
// A block that gives a slot up reads that slot from its stack cell, and a
// branch edge is not a safe point: every incoming edge must write the cell
// first. Those stores (tailSpill) come from the settled flow, never from a
// merge that is still running, or a predecessor reached later keeps a stale
// cell (#1052).
//
// The emitter, not this pass, emits the reloads/spills; this pass only names
// the values and says where they change.

#include "abstract_stack.h"
#include "capture_analysis.h"
#include "cfg.h"
#include "chunk_decoder.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qbe {

// Pseudo-predecessor index for the emitter's own prologue block
// (`@<symbol>_entry_ok`): it falls through to cfg block 0, so a phi there
// must include it. Its values are the promoted parameters, loaded in the
// prologue.
inline constexpr int kProloguePred = -1;

// One QBE phi to emit at a block's entry: `name = phi @p1 v1, @p2 v2, ...`.
struct PromoPhi {
    int slot{-1};
    std::string name;
    // One (predecessor block index, its exit value) pair per predecessor.
    std::vector<std::pair<int, std::string>> args;
};

// Everything the emitter needs to promote one function's local slots.
struct PromotionPlan {
    // Indexed by slot (the fused-stack height). True when the slot may be
    // kept in a register.
    std::vector<bool> candidate;

    // Per cfg block index: slot -> the SSA value holding it at block entry.
    // A slot absent here is not promoted in that block; the emitter falls
    // back to the stack slot.
    std::vector<std::unordered_map<int, std::string>> entryValue;

    // Per cfg block index: the phis to emit at its entry.
    std::vector<std::vector<PromoPhi>> phis;

    // Per cfg block index: catch-entry reloads, slot -> the SSA value name.
    // The emitter loads each from its stack slot at the block's entry, in
    // slot order so the emitted .ssa does not depend on a hash.
    std::vector<std::map<int, std::string>> catchReload;

    // Per cfg block index: promoted slots to write back to their stack cell
    // in the block's own tail, before its terminator. A merge that cannot
    // give its block a value for a slot (the phi would need a self-referencing
    // argument) leaves the block to reload that slot, and these stores make
    // the reload read this iteration's value: a branch edge is not a safe
    // point, so an unspilled register value would not be there yet.
    std::vector<std::set<int>> tailSpill;

    // A declaring push (abstract_stack's InvisibleVarSite offset) -> the
    // (slot, value name) the emitter defines right after it.
    std::unordered_map<int, std::pair<int, std::string>> declareAt;

    // A SET_LOCAL offset whose target slot is promoted -> (slot, value
    // name). The emitter loads the operand and updates the register instead
    // of storing to the slot.
    std::unordered_map<int, std::pair<int, std::string>> setLocalAt;

    // A POP offset that reclaims a promoted slot -> that slot. The emitter
    // drops the value (it is out of scope); no store is needed.
    std::unordered_map<int, int> reclaimAt;

    bool empty() const { return candidate.empty(); }
};

// Builds the plan. `analysis` and `cfg` must both come from `fn`'s own
// chunk, and `captures` must be the matching FunctionCaptureInfo (or an
// empty one, which simply promotes every candidate).
PromotionPlan planPromotion(const DecodedFunction& fn,
                            const FunctionStackAnalysis& analysis,
                            const FunctionCaptureInfo& captures,
                            const Cfg& cfg);

} // namespace qbe
