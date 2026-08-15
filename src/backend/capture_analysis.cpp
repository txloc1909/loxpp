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

// Joins one predecessor's exit state into a block's accumulated entry state.
// A slot present in only one of the two stays open — the other predecessor
// never touched it, so it cannot have closed it (V3_loopvar's loop head: the
// pre-loop predecessor shows `i` closed, the back-edge predecessor shows it
// open, and open wins). Two predecessors that both have the slot open must
// agree on WHICH incarnation (the same origin offset): the compiler's own
// scoping never lets two live incarnations of one slot reach a common point
// without one first closing, so disagreement here means the decoder or this
// pass has drifted from the compiler's real output, not a normal program.
void joinInto(OpenOrigins& acc, const OpenOrigins& incoming,
              const std::string& functionId, int blockLeaderOffset) {
    for (const auto& [slot, origin] : incoming) {
        auto [it, inserted] = acc.emplace(slot, origin);
        if (!inserted && it->second != origin) {
            throw std::runtime_error(
                "capture_analysis: block at offset " +
                std::to_string(blockLeaderOffset) +
                " in function id=" + functionId + " is reachable with slot " +
                std::to_string(slot) + " open from two different captures (" +
                std::to_string(it->second) + " and " + std::to_string(origin) +
                ")");
        }
    }
}

// Advances `state` across one block's own instructions for the DATAFLOW
// PROBE only (see runDataflow) — a cheap, order-only advance that ignores
// the static overlay entirely. It exists solely to converge the per-block
// ENTRY states that seed the commit walk (analyzeOneChunk); it never builds
// a CaptureLiveRange and is not itself the source of truth for which slot a
// CLOSE_UPVALUE names — that resolution, including the overlay, happens once
// in the commit walk, on each block's own fully-converged entry state.
//
// This blind "always pop the current highest" rule can pop the WRONG slot at
// a close that the commit walk will later resolve as a static, overlay-only
// no-op (referee amendment 1) — but only within one scope-exit group, whose
// N instructions the compiler always emits one per statically captured local
// of that scope, in descending order. The group's real dynamically-open
// slots are a SUBSET of that N; walking the group and blindly popping the
// current top, N times, empties exactly that subset (each real pop removes
// one of its own members, in the same descending order it would have been
// removed in correctly) and lets every remaining pop no-op on empty state —
// so the group's NET effect on `state` converges to the same result either
// way. Only the per-instruction ATTRIBUTION can differ, and the probe never
// attributes anything.
void advanceProbe(const BasicBlock& block, OpenOrigins& state) {
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
            if (state.empty()) {
                // Entry states are not yet at their fixed point during the
                // probe: a block past a LOOP header can be probed before the
                // back-edge has contributed that header's true entry state,
                // so a close can transiently appear before this pass has
                // learned its slot is open. That self-corrects on a later
                // worklist iteration.
                continue;
            }
            state.erase(std::prev(state.end()));
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

DataflowResult runDataflow(const Cfg& cfg, const std::string& functionId) {
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
        advanceProbe(cfg.blocks[static_cast<size_t>(b)], next);

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
            joinInto(merged, next, functionId,
                     cfg.blocks[static_cast<size_t>(t)].leaderOffset);
            if (result.reachable[static_cast<size_t>(t)] == 0 ||
                merged != result.entryState[static_cast<size_t>(t)]) {
                result.reachable[static_cast<size_t>(t)] = 1;
                result.entryState[static_cast<size_t>(t)] = std::move(merged);
                worklist.push_back(t);
            }
        }
    }

    return result;
}

// Folds one CLOSURE instruction's local captures into `state` and `overlay`
// together, and (if `reachable`) into `info`. See advanceCommit for what the
// two maps are and why a CLOSURE always updates both.
void handleClosureCommit(const DecodedInstruction& in, OpenOrigins& state,
                         OpenOrigins& overlay, bool reachable,
                         FunctionCaptureInfo& info,
                         RangeIndex& rangeIndexByOrigin) {
    for (const ClosureUpvalue& up : in.upvalues) {
        if (!up.isLocal) {
            continue; // a grandparent's slot, not this chunk's
        }
        int slot = up.index;
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
                in.offset);
            overlay[slot] = it->second;
            continue;
        }
        state[slot] = in.offset;
        if (reachable) {
            CaptureLiveRange range;
            range.slot = slot;
            range.firstCaptureOffset = in.offset;
            range.capturingClosureOffsets.push_back(in.offset);
            std::vector<CaptureLiveRange>& vec = info.liveRangesBySlot[slot];
            vec.push_back(range);
            rangeIndexByOrigin[{slot, in.offset}] =
                static_cast<int>(vec.size()) - 1;
        }
        overlay[slot] = in.offset;
    }
}

// Resolves one CLOSE_UPVALUE against `state` and `overlay` together, and (if
// `reachable`) records the outcome into `info`. See advanceCommit for the
// two-layer resolution rule this implements (referee amendment 1).
void handleCloseUpvalueCommit(const DecodedInstruction& in, OpenOrigins& state,
                              OpenOrigins& overlay, bool reachable,
                              const std::string& functionId,
                              FunctionCaptureInfo& info,
                              RangeIndex& rangeIndexByOrigin) {
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
                   RangeIndex& rangeIndexByOrigin) {
    for (const DecodedInstruction& in : block.instructions) {
        if (in.op == Op::CLOSURE) {
            handleClosureCommit(in, state, overlay, reachable, info,
                                rangeIndexByOrigin);
        } else if (in.op == Op::CLOSE_UPVALUE) {
            handleCloseUpvalueCommit(in, state, overlay, reachable, functionId,
                                     info, rangeIndexByOrigin);
        }
        // POP and everything else: no captured-slot effect.
    }
}

void analyzeOneChunk(const DecodedFunction& node, CaptureAnalysis& out) {
    FunctionCaptureInfo info;
    info.id = node.id;

    Cfg cfg = buildCfg(node.instructions);
    DataflowResult dataflow = runDataflow(cfg, node.id);

    // Commit walk: cfg.blocks is in byte order (see cfg.h), so this loop
    // visits every instruction of the chunk exactly once, in program order
    // — required both for ranges of one slot to land in liveRangesBySlot in
    // offset order (the determinism the mission brief's codegen-naming rule
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
                      rangeIndexByOrigin);

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
