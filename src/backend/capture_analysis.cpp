#include "capture_analysis.h"
#include "cfg.h"
#include "exec_objects.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

// Slot -> the offset of the CLOSURE that opened the slot's currently-live
// incarnation, at one point in the CFG. Absent means closed at that point.
// A std::map so the highest-numbered open slot is reachable from rbegin(),
// which is what resolving a CLOSE_UPVALUE needs (see advanceCommit).
using OpenOrigins = std::map<int, int>;

// (slot, opening CLOSURE offset) -> that range's index in
// FunctionCaptureInfo::liveRangesBySlot[slot]. Keyed by the PAIR, not the
// offset alone (R16): one CLOSURE can capture more than one parent local in
// a single instruction (one `isLocal=1` upvalue per captured slot), so two
// DIFFERENT slots can share the same origin offset. Keying by offset alone
// let the second slot's insertion silently overwrite the first's index,
// naming the wrong range for one slot and, once the vector it pointed at
// held fewer elements than the stale index, reading out of bounds.
using RangeIndex = std::map<std::pair<int, int>, int>;

// Canonicalizes (slot, CLOSURE offset) origins that two mutually exclusive
// CFG paths prove are one live instance (referee amendment 2, rule 4 — see
// joinInto). A classic union-find keyed by the pair, so a chain of merges
// (an if/else nested inside another if/else, all capturing one outer local)
// still resolves to one root. Slot is part of the key only so two different
// slots' origins never compare equal by coincidence; every union unites two
// origins of the SAME slot.
class OriginUnionFind {
  public:
    int find(int slot, int origin) {
        auto it = parent_.find({slot, origin});
        if (it == parent_.end() || it->second == origin) {
            return origin;
        }
        int root = find(slot, it->second);
        it->second = root; // path compression
        return root;
    }

    // Records that `a` and `b` name one incarnation of `slot`. The smaller
    // offset wins, so the canonical origin is always the one CLOSURE, of the
    // pair, that program order reaches first — matching
    // CaptureLiveRange::firstCaptureOffset's own contract.
    int unite(int slot, int a, int b) {
        int ra = find(slot, a);
        int rb = find(slot, b);
        if (ra == rb) {
            return ra;
        }
        int lo = std::min(ra, rb);
        int hi = std::max(ra, rb);
        parent_[{slot, hi}] = lo;
        return lo;
    }

  private:
    std::map<std::pair<int, int>, int> parent_;
};

// Joins one predecessor's exit state into a block's accumulated entry state.
// A slot present in only one of the two stays open — the other predecessor
// never touched it, so it cannot have closed it (V3_loopvar's loop head: the
// pre-loop predecessor shows `i` closed, the back-edge predecessor shows it
// open, and open wins).
//
// Referee amendment 2, rule 4: two predecessors CAN both have the slot open
// with different origins, and that is not drift. An ordinary if/else (or
// match) where both arms capture the same outer, never-redeclared local
// opens it with two different CLOSURE offsets, one per arm, and both are
// live where the arms rejoin. Whichever arm actually ran, `vm.cpp`'s
// `captureUpvalue` finds the slot's cell already open and reuses it, so the
// runtime holds ONE cell at the merge — never two. Unite the two origins
// instead of throwing (amendment 1's decision 1, item 5, is superseded: it
// called this exact case an error), and adopt the union's canonical origin
// going forward, so a later lookup under either arm's own offset agrees.
void joinInto(OpenOrigins& acc, const OpenOrigins& incoming,
              OriginUnionFind& aliases) {
    for (const auto& [slot, origin] : incoming) {
        auto [it, inserted] = acc.emplace(slot, origin);
        if (!inserted && it->second != origin) {
            it->second = aliases.unite(slot, it->second, origin);
        }
    }
}

// Advances `state` across one block's own instructions for the DATAFLOW
// PROBE only (see runDataflow) — a cheap advance whose sole job is to
// converge the per-block ENTRY states that seed the commit walk
// (analyzeOneChunk); it never builds a CaptureLiveRange and is not itself
// the source of truth for range identity — that happens once in the commit
// walk, on each block's own fully-converged entry state.
//
// R19 (referee amendment 1, item 3): an earlier version of this function
// always popped `state`'s own highest slot at a CLOSE_UPVALUE. That is
// wrong exactly when this close is a STATIC, overlay-only one (R17's
// shape) and an unrelated, still-open ENCLOSING capture happens to sit at
// `state`'s current top: the old rule popped the enclosing capture instead
// of leaving `state` untouched, corrupting every later block's entry state.
//
// `staticCloseTargets` gives the correct target directly — a no-op erase
// (`state` may not hold it: the static case) instead of a wrong one. It is
// NOT a flat, whole-chunk simulation of its own (an earlier attempt at that
// broke on R9's own regression: two SIBLING alternate closes of one
// captured local, with an unrelated ENCLOSING capture also open, are not
// safely resolvable by any simulation that treats them as sequential,
// because the first sibling's own resolution would wrongly appear to
// "use up" the target before the second sibling is ever reached — see
// analyzeOneChunk for where this map actually comes from: the COMMIT walk's
// own target resolution, from the previous iteration of a fixpoint between
// the two, exactly as the amendment specifies). Only when
// `staticCloseTargets` has nothing recorded for this exact offset (this
// pass has not yet iterated far enough to know) does this function fall
// back to `state`'s own highest slot, matching the pre-R19 rule.
//
// Referee amendment 2, section 4 (ruling on R19): "use ONE close rule in
// every walk... The naive pop exists only to bootstrap the first iteration,
// before a table exists. Iterate until the table is stable." This is that
// rule: `staticCloseTargets` IS the resolution table from the previous
// iteration (empty on the first), and the pop-`state`'s-own-top fallback is
// only ever the bootstrap the ruling names, never the final word for an
// offset the table already answers.
void advanceProbe(const BasicBlock& block, OpenOrigins& state,
                  const std::unordered_map<int, int>& staticCloseTargets) {
    for (const DecodedInstruction& in : block.instructions) {
        if (in.op == Op::CLOSURE) {
            for (const ClosureUpvalue& up : in.upvalues) {
                if (!up.isLocal) {
                    continue; // a grandparent's slot, not this chunk's
                }
                if (!state.contains(up.index)) {
                    state[up.index] = in.offset;
                }
            }
        } else if (in.op == Op::CLOSE_UPVALUE) {
            auto targetIt = staticCloseTargets.find(in.offset);
            int target = -1;
            if (targetIt != staticCloseTargets.end()) {
                target = targetIt->second;
            } else if (!state.empty()) {
                target = std::prev(state.end())->first;
            } else {
                // Entry states are not yet at their fixed point during the
                // probe: a block past a LOOP header can be probed before the
                // back-edge has contributed that header's true entry state,
                // so a close can transiently appear before this pass has
                // learned its slot is open. That self-corrects on a later
                // worklist iteration.
                continue;
            }
            state.erase(target); // a no-op if `state` does not hold `target`
        }
        // POP and everything else: no captured-slot effect.
    }
}

// True when `range` sits wholly inside one LOOP instruction's back-edge span
// [loopIns.jumpTarget, loopIns.offset]: its declaration AND every one of its
// explicit closes (V1_fresh_cell) — not merely the last one recorded,
// because break/continue/fall-through can each close the same per-iteration
// range on their own mutually exclusive path, and ALL of them must sit
// inside the loop for the range to be per-iteration. A range with no
// explicit close at all (06_shared_upvalue: closedImplicitly only) never
// qualifies — it is scoped to the whole call, never re-declared.
bool rangeIsWhollyInsideLoop(const CaptureLiveRange& range,
                             const DecodedInstruction& loopIns) {
    if (range.allCloseOffsets.empty()) {
        return false;
    }
    if (range.firstCaptureOffset < loopIns.jumpTarget) {
        return false;
    }
    return std::ranges::all_of(range.allCloseOffsets, [&loopIns](int end) {
        return end <= loopIns.offset;
    });
}

// A live range needs a fresh cell every iteration exactly when some LOOP's
// back-edge span wholly contains it — see rangeIsWhollyInsideLoop.
void markPerIterationRanges(const DecodedFunction& node,
                            FunctionCaptureInfo& info) {
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::LOOP) {
            continue;
        }
        for (auto& [slot, ranges] : info.liveRangesBySlot) {
            for (CaptureLiveRange& range : ranges) {
                if (rangeIsWhollyInsideLoop(range, ins)) {
                    range.perIteration = true;
                }
            }
        }
    }
}

void analyzeOneChunk(const DecodedFunction& node, CaptureAnalysis& out);

// Every nested function is created by exactly one CLOSURE instruction in its
// parent's own stream (funDeclaration/method in compiler.cpp always emit
// one); recurse through that instruction so the child's ownUpvalues come
// from the same place its identity does.
void recurseIntoChildren(const DecodedFunction& node, CaptureAnalysis& out) {
    std::unordered_map<int, const DecodedInstruction*> creatorByNestedIndex;
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSURE) {
            creatorByNestedIndex[ins.nestedIndex] = &ins;
        }
    }

    for (size_t i = 0; i < node.nested.size(); i++) {
        const DecodedFunction& child = node.nested[i];
        analyzeOneChunk(child, out);
        auto it = creatorByNestedIndex.find(static_cast<int>(i));
        if (it != creatorByNestedIndex.end()) {
            out.functions.at(child.id).ownUpvalues = it->second->upvalues;
        }
    }
}

// Runs the dataflow to a fixed point: for every block reachable from block
// 0, the set of captured slots open at its entry, and with which origin
// CLOSURE offset. `reachable[b]` is set the first time block b is ever
// merged into, which happens only by following a real CFG edge from block 0
// — a block this pass never visits is unreachable (for example, scope-exit
// code after an unconditional `return`), and the commit walk must treat it
// specially, not invent a real per-path entry state for it.
//
// A slot's openness at a block can depend on a LOOP back-edge that this pass
// has not reached yet on a first forward pass (the back-edge's source sits
// later in the instruction stream than its own target — V3_loopvar), so this
// is a worklist fixpoint, not a single pass: a block whose computed exit
// state changes re-queues every successor, and iteration stops only once no
// block's exit state changes anymore.
struct DataflowResult {
    std::vector<OpenOrigins> entryState;
    // uint8_t, not bool: std::vector<bool>'s proxy reference makes `!v[i]`
    // ambiguous against this codebase's `operator!(Value)` (Value has an
    // implicit bool constructor) when the std::variant Value build
    // (LOXPP_NAN_TAGGING=OFF) is active. Plain bytes sidestep the proxy
    // entirely.
    std::vector<uint8_t> reachable;
};

DataflowResult
runDataflow(const Cfg& cfg,
            const std::unordered_map<int, int>& staticCloseTargets,
            OriginUnionFind& aliases) {
    size_t n = cfg.blocks.size();
    DataflowResult result;
    result.entryState.resize(n);
    result.reachable.assign(n, 0);
    std::vector<OpenOrigins> exitState(n);
    std::vector<uint8_t> exitComputed(n, 0);
    std::deque<int> worklist;

    if (n > 0) {
        result.reachable[0] = 1; // block 0 is the chunk's entry
        worklist.push_back(0);
    }

    while (!worklist.empty()) {
        int b = worklist.front();
        worklist.pop_front();

        OpenOrigins next = result.entryState[static_cast<size_t>(b)];
        advanceProbe(cfg.blocks[static_cast<size_t>(b)], next,
                     staticCloseTargets);

        // A block's very first computation must propagate even when `next`
        // happens to equal a default-constructed (empty) exitState — that
        // equality is coincidence, not evidence the successors already
        // heard about it.
        bool changed = exitComputed[static_cast<size_t>(b)] == 0 ||
                       next != exitState[static_cast<size_t>(b)];
        exitState[static_cast<size_t>(b)] = next;
        exitComputed[static_cast<size_t>(b)] = 1;
        if (!changed) {
            continue;
        }

        for (const CfgEdge& edge :
             cfg.blocks[static_cast<size_t>(b)].successors) {
            int t = edge.targetBlock;
            OpenOrigins merged = result.entryState[static_cast<size_t>(t)];
            joinInto(merged, next, aliases);
            if (result.reachable[static_cast<size_t>(t)] == 0 ||
                merged != result.entryState[static_cast<size_t>(t)]) {
                result.reachable[static_cast<size_t>(t)] = 1;
                result.entryState[static_cast<size_t>(t)] = std::move(merged);
                worklist.push_back(t);
            }
        }
    }

    // R20's alias table is not final until the fixpoint above stops finding
    // new unions, and a slot's entry state can still hold a pre-union raw
    // origin from a block whose only edge in never revisited joinInto after
    // the union that would have canonicalized it (a CLOSURE reached by
    // exactly one predecessor never merges at all). Canonicalize every
    // entry state once, here, after the fixpoint is done — the single point
    // the commit walk (handleClosureCommit, handleCloseUpvalueCommit, and
    // analyzeOneChunk's own closedImplicitly loop) can then trust `origin`
    // values are already final, with no scattered re-lookup of its own.
    for (OpenOrigins& entry : result.entryState) {
        for (auto& [slot, origin] : entry) {
            origin = aliases.find(slot, origin);
        }
    }

    return result;
}

// Folds one CLOSURE instruction's local captures into `state` and `overlay`
// together, and (if `reachable`) into `info`. See advanceCommit for what the
// two maps are and why a CLOSURE always updates both.
//
// Referee amendment 2 corrects range identity: a capture token (slot,
// CLOSURE offset) is only a range's INITIAL NAME, not its identity (rules 1
// and 6). Three cases, matching `vm.cpp`'s own `captureUpvalue` (reuse an
// open upvalue, or create one):
//   - joinsExistingRange (rule 2): this exact PATH already has the slot
//     open -- add to that instance.
//   - joinsSiblingRange (rule 4): this path does not, but a MUTUALLY
//     EXCLUSIVE sibling path does, under a different token that joinInto's
//     union-find already proved is the SAME instance -- add to it too, and
//     canonicalize this path's own state/overlay to match, so the instance
//     reads as one range regardless of which arm a later reader follows.
//   - neither (rule 3): every path reaching here has the slot closed --
//     start a genuinely new instance.
void handleClosureCommit(const DecodedInstruction& in, OpenOrigins& state,
                         OpenOrigins& overlay, bool reachable,
                         FunctionCaptureInfo& info,
                         RangeIndex& rangeIndexByOrigin,
                         OriginUnionFind& aliases) {
    for (const ClosureUpvalue& up : in.upvalues) {
        if (!up.isLocal) {
            continue; // a grandparent's slot, not this chunk's
        }
        int slot = up.index;
        int rawOrigin = in.offset;
        int canonicalOrigin = aliases.find(slot, rawOrigin);

        auto it = state.find(slot);
        // During commit, "already open" is only trustworthy when that
        // origin's range object actually exists yet. A loop header's
        // stabilized entry state can show a slot open with THIS VERY
        // CLOSURE's own offset as origin — the back-edge's echo of "still
        // open on a later iteration" — before this, the walk's only visit
        // to that offset, has run at all. Treat that as opening fresh, not
        // sharing a range that is not there yet.
        bool joinsExistingRange =
            reachable && it != state.end() &&
            rangeIndexByOrigin.contains({slot, it->second});
        if (joinsExistingRange) {
            int idx = rangeIndexByOrigin.at({slot, it->second});
            info.liveRangesBySlot[slot][idx].capturingClosureOffsets.push_back(
                rawOrigin);
            overlay[slot] = it->second;
            continue;
        }
        bool joinsSiblingRange =
            reachable && rangeIndexByOrigin.contains({slot, canonicalOrigin});
        if (joinsSiblingRange) {
            int idx = rangeIndexByOrigin.at({slot, canonicalOrigin});
            info.liveRangesBySlot[slot][idx].capturingClosureOffsets.push_back(
                rawOrigin);
            state[slot] = canonicalOrigin;
            overlay[slot] = canonicalOrigin;
            continue;
        }
        state[slot] = canonicalOrigin;
        if (reachable) {
            CaptureLiveRange range;
            range.slot = slot;
            range.firstCaptureOffset = canonicalOrigin;
            range.capturingClosureOffsets.push_back(rawOrigin);
            std::vector<CaptureLiveRange>& vec = info.liveRangesBySlot[slot];
            vec.push_back(range);
            rangeIndexByOrigin[{slot, canonicalOrigin}] =
                static_cast<int>(vec.size()) - 1;
        }
        overlay[slot] = canonicalOrigin;
    }
}

// Resolves one CLOSE_UPVALUE against `state` and `overlay` together, and (if
// `reachable`) records the outcome into `info`. See advanceCommit for the
// two-layer resolution rule this implements (referee amendment 1).
//
// R19: also records the resolved target slot into `resolvedTargets`, keyed
// by this instruction's own offset — this walk's OWN resolution (state AND
// the program-order overlay together) is authoritative, so analyzeOneChunk
// feeds it back as the NEXT iteration's `staticCloseTargets` for
// advanceProbe (referee amendment 1, item 3: iterate the two to a fixed
// point). A close this walk cannot yet resolve (neither layer holds
// anything, reachable) throws, same as before this map existed;
// analyzeOneChunk catches that on every iteration but the last, because an
// earlier iteration's `state` can still be wrong in exactly the way R19
// describes, and a later iteration, seeded with whatever this one DID
// resolve before the throw, corrects it.
void handleCloseUpvalueCommit(const DecodedInstruction& in, OpenOrigins& state,
                              OpenOrigins& overlay, bool reachable,
                              const std::string& functionId,
                              FunctionCaptureInfo& info,
                              RangeIndex& rangeIndexByOrigin,
                              std::unordered_map<int, int>& resolvedTargets) {
    bool hasState = !state.empty();
    bool hasOverlay = !overlay.empty();
    if (!hasState && !hasOverlay) {
        if (!reachable) {
            info.unreachableCloseOffsets.push_back(in.offset);
            return;
        }
        throw std::runtime_error(
            "capture_analysis: CLOSE_UPVALUE at offset " +
            std::to_string(in.offset) + " in function id=" + functionId +
            " has no open captured slot to close in either layer");
    }
    int stateTop = hasState ? std::prev(state.end())->first : -1;
    int overlayTop = hasOverlay ? std::prev(overlay.end())->first : -1;
    bool dynamic = hasState && stateTop >= overlayTop;
    int slot = dynamic ? stateTop : overlayTop;
    int origin = dynamic ? state.at(slot) : overlay.at(slot);
    resolvedTargets[in.offset] = slot;
    if (reachable) {
        auto idxIt = rangeIndexByOrigin.find({slot, origin});
        if (idxIt != rangeIndexByOrigin.end()) {
            info.liveRangesBySlot[slot][idxIt->second]
                .allCloseOffsets.push_back(in.offset);
        }
    } else {
        info.unreachableCloseOffsets.push_back(in.offset);
    }
    if (dynamic) {
        state.erase(slot);
    }
    overlay.erase(slot);
}

// Advances `state` (this block's own fully-converged per-path entry state)
// and `overlay` (one program-order layer, SHARED across every block this
// function is called for, in ascending block-index — i.e. ascending offset,
// see Cfg::blocks — order) across one block's instructions, building the
// real CaptureLiveRange records as it goes. Called once per block, in that
// order, for every block of the chunk — reachable or not (referee amendment
// 1, section on unreachable blocks: an unreachable CLOSE_UPVALUE still
// updates the overlay, because the compiler's own single-pass bookkeeping
// does not know or care whether the code it just emitted ever runs).
//
// `reachable` gates every OBSERVABLE effect on `info` (range creation, close
// attribution, the unreachable-close report) — `state` and `overlay` still
// advance for an unreachable block, purely so a LATER, reachable block's own
// overlay lookups stay correctly threaded, but nothing at all is recorded
// for the block itself.
//
// Referee amendment 1 (PR #101): the round-3 rule ("CLOSE_UPVALUE closes the
// highest open captured slot in the per-path state") assumed every
// statically captured local of a scope is dynamically open on the path that
// reaches its close. That is false when a CLOSURE sits on a branch that
// returns: a sibling path runs the scope's own CLOSE_UPVALUE for a slot its
// own execution never opened. `overlay` tracks exactly that: the compiler's
// single-pass, order-only view of "captured, not yet closed" (mirroring
// Local::isCaptured), independent of which branch actually ran. A CLOSURE
// updates BOTH layers together, to whatever origin `state` itself lands on
// (already open on this path — the shared-cell case — or freshly opened
// here). A CLOSE_UPVALUE resolves against whichever layer holds the HIGHER
// slot number: `state` wins when it holds the target (a real, dynamic close
// — the target is genuinely open on this exact path, so `state` loses it);
// `overlay` wins only when `state` does not hold it (the target is captured
// SOMEWHERE in this scope, per the compiler, but not on this path — a
// dynamic no-op here, matching vm.cpp's closeUpvalues, which does nothing to
// a slot with no open cell). The target is always removed from `overlay`
// either way, because the compiler's own bookkeeping has now accounted for
// it, on whichever path first reaches it in program order.
void advanceCommit(const BasicBlock& block, OpenOrigins& state,
                   OpenOrigins& overlay, bool reachable,
                   const std::string& functionId, FunctionCaptureInfo& info,
                   RangeIndex& rangeIndexByOrigin, OriginUnionFind& aliases,
                   std::unordered_map<int, int>& resolvedTargets) {
    for (const DecodedInstruction& in : block.instructions) {
        if (in.op == Op::CLOSURE) {
            handleClosureCommit(in, state, overlay, reachable, info,
                                rangeIndexByOrigin, aliases);
        } else if (in.op == Op::CLOSE_UPVALUE) {
            handleCloseUpvalueCommit(in, state, overlay, reachable, functionId,
                                     info, rangeIndexByOrigin, resolvedTargets);
        }
        // POP and everything else: no captured-slot effect.
    }
}

// Runs one full fixpoint-plus-commit pass: the CFG dataflow (seeded with
// `staticCloseTargets`, the previous pass's own resolution — empty on the
// first call, matching the pre-R19 rule) and then the sequential commit
// walk over every block, in program order, building `info` and reading back
// every close's resolved target into `resolvedTargets` as it goes.
//
// A commit walk that reaches a close neither layer can yet resolve throws
// (handleCloseUpvalueCommit) with whatever `resolvedTargets` this SAME call
// had gathered before the throw left in place — the caller (analyzeOneChunk)
// uses that partial map to seed the next call, exactly the failure case R19
// describes (a target genuinely needs one more round to become visible).
FunctionCaptureInfo
runOnePass(const DecodedFunction& node, const Cfg& cfg,
           const std::unordered_map<int, int>& staticCloseTargets,
           std::unordered_map<int, int>& resolvedTargets) {
    FunctionCaptureInfo info;
    info.id = node.id;

    OriginUnionFind aliases;
    DataflowResult dataflow = runDataflow(cfg, staticCloseTargets, aliases);

    // cfg.blocks is in byte order (see cfg.h), so this loop visits every
    // instruction of the chunk exactly once, in program order — required
    // both for ranges of one slot to land in liveRangesBySlot in offset
    // order (the determinism the mission brief's codegen-naming rule
    // depends on, and the non-overlap invariant the tests check) and for
    // `overlay` below to correctly mirror the compiler's own single-pass
    // view (referee amendment 1).
    RangeIndex rangeIndexByOrigin;
    OpenOrigins overlay;
    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        bool reachable = dataflow.reachable[b] != 0;
        // An unreachable block never executes, so nothing is genuinely
        // dynamically open there; only `overlay` (updated by advanceCommit
        // regardless of `reachable`) can resolve a close inside it.
        OpenOrigins state = reachable ? dataflow.entryState[b] : OpenOrigins{};
        advanceCommit(cfg.blocks[b], state, overlay, reachable, node.id, info,
                      rangeIndexByOrigin, aliases, resolvedTargets);

        if (!reachable || !cfg.blocks[b].successors.empty()) {
            continue;
        }
        // A reachable terminal block (RETURN or MATCH_ERROR): whatever is
        // still open in `state` here closes with the frame, not with an
        // explicit CLOSE_UPVALUE (06_shared_upvalue's `outer` never emits
        // one on any path). This can coexist with a real explicit close
        // recorded on a DIFFERENT path for the same range (one branch
        // returns early while it is still open, another branch closes it
        // properly) — see FunctionCaptureInfo::closedImplicitly.
        for (const auto& [slot, origin] : state) {
            auto idxIt = rangeIndexByOrigin.find({slot, origin});
            if (idxIt == rangeIndexByOrigin.end()) {
                continue;
            }
            info.liveRangesBySlot[slot][idxIt->second].closedImplicitly = true;
        }
    }

    return info;
}

void analyzeOneChunk(const DecodedFunction& node, CaptureAnalysis& out) {
    Cfg cfg = buildCfg(node.instructions);

    // R19 (referee amendment 1, item 3): the close-resolution rule and the
    // per-path fixpoint depend on each other. advanceProbe needs to know
    // which slot each CLOSE_UPVALUE targets to avoid corrupting an
    // unrelated, lower slot's open state at a static close; the commit
    // walk's own resolution (state and the program-order overlay together)
    // is the authoritative source for that target, but it is only correct
    // once the fixpoint it reads its `state` from is itself correct. Break
    // the circularity by iterating: seed the fixpoint with the PREVIOUS
    // pass's resolved targets (empty on the first pass, matching the
    // pre-R19 rule exactly), and feed this pass's own resolution back in as
    // the next seed. The referee's own prototype needed two passes on every
    // program measured; this allows a few more before giving up on
    // convergence.
    std::unordered_map<int, int> staticCloseTargets;
    FunctionCaptureInfo info;
    constexpr int kMaxIterations = 6;
    bool converged = false;
    for (int iteration = 0; iteration < kMaxIterations && !converged;
         iteration++) {
        std::unordered_map<int, int> resolvedTargets;
        bool isLastIteration = iteration == kMaxIterations - 1;
        try {
            info = runOnePass(node, cfg, staticCloseTargets, resolvedTargets);
        } catch (const std::runtime_error&) {
            // A close this pass could not yet resolve: keep whatever this
            // SAME pass resolved before the throw (a superset of the seed
            // it started from) and try again, unless this was the last
            // allowed attempt, in which case the failure is real (R19's own
            // "has no open captured slot" case, unrelated to iteration).
            if (isLastIteration) {
                throw;
            }
            staticCloseTargets = std::move(resolvedTargets);
            continue;
        }
        converged = resolvedTargets == staticCloseTargets;
        staticCloseTargets = std::move(resolvedTargets);
    }
    if (!converged) {
        throw std::runtime_error(
            "capture_analysis: close-target resolution did not reach a "
            "fixed point for function id=" +
            node.id);
    }

    markPerIterationRanges(node, info);

    out.functions[node.id] = std::move(info);
    recurseIntoChildren(node, out);
}

} // namespace

CaptureAnalysis analyzeCaptures(const DecodedFunction& root) {
    CaptureAnalysis result;
    analyzeOneChunk(root, result);
    return result;
}
