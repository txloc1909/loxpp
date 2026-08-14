#include "capture_analysis.h"
#include "cfg.h"
#include "exec_objects.h"

#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace {

// Slot -> the offset of the CLOSURE that opened the slot's currently-live
// incarnation, at one point in the CFG. Absent means closed at that point.
// A std::map so the highest-numbered open slot is reachable from rbegin(),
// which is what resolving a CLOSE_UPVALUE needs (see advance).
using OpenOrigins = std::map<int, int>;

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

// Folds one CLOSURE instruction's local captures into `state`/`record`. See
// `advance` for what `record == nullptr` (the dataflow probe) versus set
// (the commit walk) means.
void handleClosure(const DecodedInstruction& in, OpenOrigins& state,
                   FunctionCaptureInfo* record,
                   std::unordered_map<int, int>* rangeIndexByOrigin) {
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
        // sharing a range that is not there yet. The dataflow probe
        // (record == nullptr) never builds range objects, so any open
        // entry is shareable there — only the identity of the origin
        // offset matters for the probe, not whether it has "a range" yet.
        bool sharesExistingRange =
            it != state.end() &&
            (record == nullptr || rangeIndexByOrigin->count(it->second) > 0);
        if (sharesExistingRange) {
            if (record != nullptr) {
                int idx = rangeIndexByOrigin->at(it->second);
                record->liveRangesBySlot[slot][idx]
                    .capturingClosureOffsets.push_back(in.offset);
            }
            continue;
        }
        state[slot] = in.offset;
        if (record != nullptr) {
            CaptureLiveRange range;
            range.slot = slot;
            range.firstCaptureOffset = in.offset;
            range.capturingClosureOffsets.push_back(in.offset);
            std::vector<CaptureLiveRange>& vec = record->liveRangesBySlot[slot];
            vec.push_back(range);
            (*rangeIndexByOrigin)[in.offset] = static_cast<int>(vec.size()) - 1;
        }
    }
}

// Resolves one CLOSE_UPVALUE against `state` and closes it. See `advance`
// for what `record == nullptr` (the dataflow probe) versus set (the commit
// walk) means.
//
// Referee decision (PR #101, round-3): CLOSE_UPVALUE closes the HIGHEST
// open captured slot in the state. Compiler::endScope and
// Compiler::emitLoopCleanup both reclaim locals in descending slot order,
// one instruction per local, CLOSE_UPVALUE for each captured one. On one
// real path, an earlier close in the same reclaim run already closed every
// captured slot above this one, so the highest still-open slot is always
// this instruction's target. This needs no group boundaries and no local
// count — only the open set this pass already tracks.
void handleCloseUpvalue(const DecodedInstruction& in, OpenOrigins& state,
                        const std::string& functionId,
                        FunctionCaptureInfo* record,
                        std::unordered_map<int, int>* rangeIndexByOrigin) {
    if (state.empty()) {
        // The probe runs on entry states that are not yet at their fixed
        // point: a block past a LOOP header can be probed before the
        // back-edge has contributed that header's true entry state (the
        // back-edge's own source sits later in the instruction stream than
        // its target), so a close can transiently appear before this pass
        // has learned its slot is open. That self-corrects on a later
        // worklist iteration. The commit walk runs on the fully converged
        // result of a REACHABLE block, so an empty state here can only mean
        // decoder or compiler drift.
        if (record == nullptr) {
            return;
        }
        throw std::runtime_error("capture_analysis: CLOSE_UPVALUE at offset " +
                                 std::to_string(in.offset) +
                                 " in function id=" + functionId +
                                 " has no open captured slot to close");
    }
    auto highest = std::prev(state.end());
    int slot = highest->first;
    int origin = highest->second;
    if (record != nullptr) {
        int idx = rangeIndexByOrigin->at(origin);
        CaptureLiveRange& range = record->liveRangesBySlot[slot][idx];
        range.end = in.offset;
        range.allCloseOffsets.push_back(in.offset);
    }
    state.erase(highest);
}

// Advances `state` across one block's own instructions, in program order.
// Pure state advance when `record` is null (the dataflow probe, run to a
// fixed point before any range is recorded — see runDataflow); also builds
// the real CaptureLiveRange records when `record` is set (the commit walk,
// run once per reachable block on its stabilized entry state — see
// analyzeOneChunk). Same open/close logic either way, so the commit walk
// cannot disagree with the probe about which slot a CLOSE_UPVALUE names.
void advance(const BasicBlock& block, OpenOrigins& state,
             const std::string& functionId, FunctionCaptureInfo* record,
             std::unordered_map<int, int>* rangeIndexByOrigin) {
    for (const DecodedInstruction& in : block.instructions) {
        if (in.op == Op::CLOSURE) {
            handleClosure(in, state, record, rangeIndexByOrigin);
        } else if (in.op == Op::CLOSE_UPVALUE) {
            handleCloseUpvalue(in, state, functionId, record,
                               rangeIndexByOrigin);
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
// code after an unconditional `return`), and the commit walk must skip it,
// not invent an entry state for it.
//
// A slot's openness at a block can depend on a LOOP back-edge that this pass
// has not reached yet on a first forward pass (the back-edge's source sits
// later in the instruction stream than its own target — V3_loopvar), so this
// is a worklist fixpoint, not a single pass: a block whose computed exit
// state changes re-queues every successor, and iteration stops only once no
// block's exit state changes anymore.
struct DataflowResult {
    std::vector<OpenOrigins> entryState;
    std::vector<bool> reachable;
};

DataflowResult runDataflow(const Cfg& cfg, const std::string& functionId) {
    size_t n = cfg.blocks.size();
    DataflowResult result;
    result.entryState.resize(n);
    result.reachable.assign(n, false);
    std::vector<OpenOrigins> exitState(n);
    std::vector<bool> exitComputed(n, false);
    std::deque<int> worklist;

    if (n > 0) {
        result.reachable[0] = true; // block 0 is the chunk's entry
        worklist.push_back(0);
    }

    while (!worklist.empty()) {
        int b = worklist.front();
        worklist.pop_front();

        OpenOrigins next = result.entryState[static_cast<size_t>(b)];
        advance(cfg.blocks[static_cast<size_t>(b)], next, functionId, nullptr,
                nullptr);

        // A block's very first computation must propagate even when `next`
        // happens to equal a default-constructed (empty) exitState — that
        // equality is coincidence, not evidence the successors already
        // heard about it.
        bool changed = !exitComputed[static_cast<size_t>(b)] ||
                       next != exitState[static_cast<size_t>(b)];
        exitState[static_cast<size_t>(b)] = next;
        exitComputed[static_cast<size_t>(b)] = true;
        if (!changed) {
            continue;
        }

        for (const CfgEdge& edge :
             cfg.blocks[static_cast<size_t>(b)].successors) {
            int t = edge.targetBlock;
            OpenOrigins merged = result.entryState[static_cast<size_t>(t)];
            joinInto(merged, next, functionId,
                     cfg.blocks[static_cast<size_t>(t)].leaderOffset);
            if (!result.reachable[static_cast<size_t>(t)] ||
                merged != result.entryState[static_cast<size_t>(t)]) {
                result.reachable[static_cast<size_t>(t)] = true;
                result.entryState[static_cast<size_t>(t)] = std::move(merged);
                worklist.push_back(t);
            }
        }
    }

    return result;
}

void analyzeOneChunk(const DecodedFunction& node, CaptureAnalysis& out) {
    FunctionCaptureInfo info;
    info.id = node.id;

    Cfg cfg = buildCfg(node.instructions);
    DataflowResult dataflow = runDataflow(cfg, node.id);

    // Commit walk: cfg.blocks is in byte order (see cfg.h), so ranges for
    // one slot are appended to liveRangesBySlot in offset order here too —
    // required for the determinism the mission brief's codegen-naming rule
    // depends on, and for the non-overlap invariant the tests check.
    std::unordered_map<int, int> rangeIndexByOrigin;
    int chunkEnd = static_cast<int>(node.function->chunk.size());
    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        if (!dataflow.reachable[b]) {
            // Never executes on any path from the chunk's entry (dead code
            // after an unconditional return, for example). Attribute its
            // CLOSE_UPVALUE instructions to no range, but still report them,
            // so a cross-check can tell "unreachable" apart from "the pass
            // lost track of a real close."
            for (const DecodedInstruction& ins : cfg.blocks[b].instructions) {
                if (ins.op == Op::CLOSE_UPVALUE) {
                    info.unreachableCloseOffsets.push_back(ins.offset);
                }
            }
            continue;
        }

        OpenOrigins state = dataflow.entryState[b];
        advance(cfg.blocks[b], state, node.id, &info, &rangeIndexByOrigin);

        if (!cfg.blocks[b].successors.empty()) {
            continue;
        }
        // A reachable terminal block (RETURN or MATCH_ERROR): whatever is
        // still open here closes with the frame, not with an explicit
        // CLOSE_UPVALUE (06_shared_upvalue's `outer` never emits one on any
        // path). This can coexist with a real explicit close recorded on a
        // DIFFERENT path for the same range (one branch returns early while
        // it is still open, another branch closes it properly) — so only
        // fall back to the chunk's length when no explicit close exists yet;
        // never overwrite a real one.
        for (const auto& [slot, origin] : state) {
            int idx = rangeIndexByOrigin.at(origin);
            CaptureLiveRange& range = info.liveRangesBySlot[slot][idx];
            range.closedImplicitly = true;
            if (range.allCloseOffsets.empty()) {
                range.end = chunkEnd;
            }
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
