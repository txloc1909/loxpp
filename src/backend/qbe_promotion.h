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
// The emitter, not this pass, emits the reloads/spills; this pass only names
// the values and says where they change.

#include "abstract_stack.h"
#include "capture_analysis.h"
#include "cfg.h"
#include "chunk_decoder.h"

#include <map>
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
