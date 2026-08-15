#include "capture_analysis.h"
#include "cfg.h"
#include "exec_objects.h"

#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// Pass 0 — frame stack heights (referee amendment 3).
//
// The net effect one instruction has on the frame's stack height. Mirrors
// vm.cpp exactly, opcode by opcode — this is the same fact
// src/backend/abstract_stack.cpp (N2) computes as pushCount - popCount, but
// this pass tracks only the scalar net delta, not the locals/temporaries
// split N2 needs, so it is reimplemented here rather than shared: N3 and N2
// are independent analyses (notes/backend-implementation-dag.md), and the DAG
// note records that N2 may fold into this walk later, not the other way
// round.
// ---------------------------------------------------------------------------

int frameHeightEffect(const DecodedInstruction& ins) {
    switch (ins.op) {
    // Pure pushes.
    case Op::CONSTANT:
    case Op::NIL:
    case Op::TRUE:
    case Op::FALSE:
    case Op::GET_LOCAL:
    case Op::GET_GLOBAL:
    case Op::GET_UPVALUE:
    case Op::CLASS:
    case Op::CLOSURE:
        return 1;

    // No net effect: a pop-then-push-in-place (NEGATE, NOT, GET_PROPERTY,
    // GET_ITER, ITER_HAS_NEXT, ITER_NEXT, GET_TAG, INSTANCEOF, IS_SEQ), the
    // peek family that leaves its operand where it found it (SET_LOCAL,
    // SET_GLOBAL, SET_UPVALUE, JUMP_IF_FALSE — P2), or a pure control
    // transfer with no stack effect of its own (JUMP, LOOP).
    case Op::NEGATE:
    case Op::NOT:
    case Op::SET_LOCAL:
    case Op::SET_GLOBAL:
    case Op::SET_UPVALUE:
    case Op::JUMP:
    case Op::JUMP_IF_FALSE:
    case Op::LOOP:
    case Op::GET_PROPERTY:
    case Op::GET_ITER:
    case Op::ITER_HAS_NEXT:
    case Op::ITER_NEXT:
    case Op::GET_TAG:
    case Op::INSTANCEOF:
    case Op::IS_SEQ:
        return 0;

    // Pop 2 (or 1), push 1: net -1. JUMP_TABLE pops only the tag integer,
    // whichever edge is taken (an arm, or the out-of-range fall-through).
    case Op::EQUAL:
    case Op::GREATER:
    case Op::LESS:
    case Op::ADD:
    case Op::SUBTRACT:
    case Op::MULTIPLY:
    case Op::DIVIDE:
    case Op::MODULO:
    case Op::IN:
    case Op::PRINT:
    case Op::POP:
    case Op::DEFINE_GLOBAL:
    case Op::CLOSE_UPVALUE:
    case Op::SET_PROPERTY:
    case Op::DEFINE_METHOD:
    case Op::INHERIT:
    case Op::GET_INDEX:
    case Op::GET_SUPER:
    case Op::JUMP_TABLE:
        return -1;

    // Pop 3, push 1: net -2.
    case Op::SET_INDEX:
    case Op::SLICE:
        return -2;

    // Pop argCount (+1 for SUPER_INVOKE's superclass), push 1 result.
    case Op::CALL:
    case Op::INVOKE:
        return -ins.byteOperand;
    case Op::SUPER_INVOKE:
        return -(ins.byteOperand + 1);

    // Pop N (BUILD_LIST) or 2N (BUILD_MAP) elements, push 1 aggregate.
    case Op::BUILD_LIST:
        return 1 - ins.byteOperand;
    case Op::BUILD_MAP:
        return 1 - (2 * ins.byteOperand);

    // Terminal: no successor edge ever reads a height past these, so their
    // own net effect is never propagated anywhere.
    case Op::RETURN:
    case Op::MATCH_ERROR:
        return 0;
    }
    throw std::runtime_error(
        "capture_analysis: no frame-height effect for opcode " +
        std::to_string(static_cast<int>(ins.op)) + " at offset " +
        std::to_string(ins.offset));
}

// Computes the height immediately before every reachable instruction of one
// already-built CFG, given the chunk's entry height (1 + arity). One forward
// worklist walk: a block's entry height is fixed the first time some edge
// reaches it, and every later edge into that same block must agree, or the
// compiler/decoder have drifted from each other (see capture_analysis.h).
// LOOP is always a back edge (P3a), so a loop header's height is always
// fixed by a FORWARD edge before its own back edge is ever walked — no
// widening or repeated re-visits are needed, unlike N2's abstract stack,
// which also tracks a weaker (locals-vs-temporaries) invariant this pass
// does not need.
std::unordered_map<int, int>
computeFrameHeightsForCfg(const Cfg& cfg, int entryHeight,
                          const std::string& functionId) {
    std::unordered_map<int, int> heightBefore;
    size_t n = cfg.blocks.size();
    std::vector<int> blockEntryHeight(n, -1);
    std::vector<uint8_t> known(n, 0);
    std::deque<int> worklist;

    if (n > 0) {
        blockEntryHeight[0] = entryHeight;
        known[0] = 1;
        worklist.push_back(0);
    }

    while (!worklist.empty()) {
        int b = worklist.front();
        worklist.pop_front();

        int h = blockEntryHeight[static_cast<size_t>(b)];
        for (const DecodedInstruction& ins :
             cfg.blocks[static_cast<size_t>(b)].instructions) {
            heightBefore[ins.offset] = h;
            h += frameHeightEffect(ins);
        }

        for (const CfgEdge& edge :
             cfg.blocks[static_cast<size_t>(b)].successors) {
            int t = edge.targetBlock;
            if (known[static_cast<size_t>(t)] == 0) {
                known[static_cast<size_t>(t)] = 1;
                blockEntryHeight[static_cast<size_t>(t)] = h;
                worklist.push_back(t);
            } else if (blockEntryHeight[static_cast<size_t>(t)] != h) {
                throw std::runtime_error(
                    "capture_analysis: frame height mismatch entering block " +
                    std::to_string(t) + " in function id=" + functionId + " (" +
                    std::to_string(blockEntryHeight[static_cast<size_t>(t)]) +
                    " vs " + std::to_string(h) + ")");
            }
        }
    }

    return heightBefore;
}

// ---------------------------------------------------------------------------
// Pass 1 — which instance of a captured slot is open at each point.
//
// Unchanged from referee amendment 2: a slot is open at a block's entry
// exactly when some predecessor that reaches it still has it open
// (mirroring the VM's own open-upvalue list), and a merge where two
// predecessors bring the same slot open under different origins unites them
// into one instance (mirroring captureUpvalue's reuse of an already-open
// upvalue at one stack location). What amendment 3 removes is the static
// overlay this fixpoint used to need to resolve a CLOSE_UPVALUE's target:
// with the height rule, that target is now a compile-time constant
// (closeSlotFor below), so a close is either found open on this path (erase
// it) or not (a genuine no-op here, left for Pass 2's program-order
// attribution) — no second layer, no iteration to a fixed point.
// ---------------------------------------------------------------------------

// Slot -> the offset of the CLOSURE that opened the slot's currently-live
// incarnation, at one point in the CFG. Absent means closed at that point.
using OpenOrigins = std::map<int, int>;

// (slot, opening CLOSURE offset) -> that range's index in
// FunctionCaptureInfo::liveRangesBySlot[slot]. Keyed by the PAIR, not the
// offset alone (R16): one CLOSURE can capture more than one parent local in
// a single instruction (one `isLocal=1` upvalue per captured slot), so two
// DIFFERENT slots can share the same origin offset.
using RangeIndex = std::map<std::pair<int, int>, int>;

// Canonicalizes (slot, CLOSURE offset) origins that two mutually exclusive
// CFG paths prove are one live instance (referee amendment 2, rule 4). A
// classic union-find keyed by the pair, so a chain of merges (an if/else
// nested inside another if/else, all capturing one outer local) still
// resolves to one root. Slot is part of the key only so two different
// slots' origins never compare equal by coincidence.
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
// with different origins, and that is not drift. Unite the two origins
// instead of throwing.
void joinInto(OpenOrigins& acc, const OpenOrigins& incoming,
              OriginUnionFind& aliases) {
    for (const auto& [slot, origin] : incoming) {
        auto [it, inserted] = acc.emplace(slot, origin);
        if (!inserted && it->second != origin) {
            it->second = aliases.unite(slot, it->second, origin);
        }
    }
}

// Advances `state` across one block's own instructions, for the DATAFLOW
// FIXPOINT only (see runDataflow) — a cheap advance whose sole job is to
// converge the per-block ENTRY states that seed the attribution walk
// (analyzeOneChunk). It never builds a CaptureLiveRange.
//
// CLOSE_UPVALUE's target slot is now a compile-time constant
// (`heightBefore.at(offset) - 1`, referee amendment 3), so resolving it here
// needs no history, no fallback, and no later reconciliation: erase that
// exact slot if `state` has it open, otherwise leave `state` untouched — a
// dynamic no-op on this path, exactly matching what `vm.cpp`'s
// `closeUpvalues` does when nothing is open at that stack location.
void advanceDataflow(const BasicBlock& block, OpenOrigins& state,
                     const std::unordered_map<int, int>& heightBefore) {
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
            int slot = heightBefore.at(in.offset) - 1;
            state.erase(slot); // a no-op if `state` does not hold `slot`
        }
        // POP and everything else: no captured-slot effect.
    }
}

// Runs the dataflow to a fixed point: for every block reachable from block
// 0, the set of captured slots open at its entry, and with which origin
// CLOSURE offset. `reachable[b]` is set the first time block b is ever
// merged into — a block this pass never visits is unreachable (for example,
// scope-exit code after an unconditional `return`).
//
// A slot's openness at a block can depend on a LOOP back-edge that this pass
// has not reached yet on a first forward pass (V3_loopvar), so this is a
// worklist fixpoint, not a single pass.
struct DataflowResult {
    std::vector<OpenOrigins> entryState;
    // uint8_t, not bool: std::vector<bool>'s proxy reference makes `!v[i]`
    // ambiguous against this codebase's `operator!(Value)` (Value has an
    // implicit bool constructor) when the std::variant Value build
    // (LOXPP_NAN_TAGGING=OFF) is active. Plain bytes sidestep the proxy
    // entirely.
    std::vector<uint8_t> reachable;
};

DataflowResult runDataflow(const Cfg& cfg,
                           const std::unordered_map<int, int>& heightBefore,
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
        advanceDataflow(cfg.blocks[static_cast<size_t>(b)], next, heightBefore);

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

    // A slot's entry state can still hold a pre-union raw origin from a
    // block whose only edge in never revisited joinInto after the union
    // that would have canonicalized it (a CLOSURE reached by exactly one
    // predecessor never merges at all). Canonicalize every entry state
    // once, here, after the fixpoint is done — the single point the
    // attribution walk can then trust `origin` values are already final.
    for (OpenOrigins& entry : result.entryState) {
        for (auto& [slot, origin] : entry) {
            origin = aliases.find(slot, origin);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Pass 2 — program-order attribution.
// ---------------------------------------------------------------------------

// Folds one CLOSURE instruction's local captures into `state` together
// (this walk's own path-local view) and, if `reachable`, into `info`. Also
// updates `latestInstanceBySlot` unconditionally — see the header comment on
// that map, just below.
//
// Referee amendment 2 corrects range identity: a capture token (slot,
// CLOSURE offset) is only a range's INITIAL NAME, not its identity. Three
// cases, matching `vm.cpp`'s own `captureUpvalue` (reuse an open upvalue, or
// create one):
//   - joinsExistingRange (rule 2): this exact PATH already has the slot
//     open -- add to that instance.
//   - joinsSiblingRange (rule 4): this path does not, but a MUTUALLY
//     EXCLUSIVE sibling path does, under a different token that the
//     dataflow's own union-find already proved is the SAME instance -- add
//     to it too.
//   - neither (rule 3): every path reaching here has the slot closed --
//     start a genuinely new instance.
void handleClosureCommit(const DecodedInstruction& in, OpenOrigins& state,
                         bool reachable, FunctionCaptureInfo& info,
                         RangeIndex& rangeIndexByOrigin,
                         OriginUnionFind& aliases,
                         std::unordered_map<int, int>& latestInstanceBySlot) {
    for (const ClosureUpvalue& up : in.upvalues) {
        if (!up.isLocal) {
            continue; // a grandparent's slot, not this chunk's
        }
        int slot = up.index;
        int rawOrigin = in.offset;
        int canonicalOrigin = aliases.find(slot, rawOrigin);

        auto it = state.find(slot);
        // During attribution, "already open" is only trustworthy when that
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
            latestInstanceBySlot[slot] = it->second;
            continue;
        }
        bool joinsSiblingRange =
            reachable && rangeIndexByOrigin.contains({slot, canonicalOrigin});
        if (joinsSiblingRange) {
            int idx = rangeIndexByOrigin.at({slot, canonicalOrigin});
            info.liveRangesBySlot[slot][idx].capturingClosureOffsets.push_back(
                rawOrigin);
            state[slot] = canonicalOrigin;
            latestInstanceBySlot[slot] = canonicalOrigin;
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
        latestInstanceBySlot[slot] = canonicalOrigin;
    }
}

// Resolves one CLOSE_UPVALUE and, if `reachable`, records the outcome into
// `info`. `slot` is `heightBefore.at(in.offset) - 1` — a compile-time
// constant, not inferred (referee amendment 3).
//
// A close whose slot `state` shows open on this exact path is a DYNAMIC
// close: attribute it to that instance and erase it from `state`, exactly
// like amendments 1 and 2 did.
//
// A close whose slot `state` does NOT show open is a STATIC one (R22): the
// compiler emits one CLOSE_UPVALUE per exit path that crosses a captured
// local's scope (break, continue, the fall-through), and every path but the
// one that dynamically captured it reaches its own copy of that instruction
// with nothing open there — a real, dynamic no-op on this path, exactly
// matching what `vm.cpp`'s `closeUpvalues` does. It still needs a real
// instance to record against: `latestInstanceBySlot` names the most recent
// CLOSURE this whole program-order walk has seen for this EXACT slot,
// updated by every CLOSURE (handleClosureCommit) and never cleared by a
// close — an earlier design (amendments 1 and 2's static overlay) erased
// this on the FIRST close and broke on a slot's second, sibling close
// (R22). Exact-slot matching (via height) makes stealing impossible: a
// close for slot 7 can never consult, or touch, slot 3's entry.
//
// A REACHABLE close whose slot has no entry in `latestInstanceBySlot` at
// all — no CLOSURE, anywhere in program order before it, ever captured this
// slot — is compiler or decoder drift, not a normal program: throws.
void handleCloseUpvalueCommit(
    const DecodedInstruction& in, int slot, OpenOrigins& state, bool reachable,
    const std::string& functionId, FunctionCaptureInfo& info,
    RangeIndex& rangeIndexByOrigin,
    const std::unordered_map<int, int>& latestInstanceBySlot) {
    if (!reachable) {
        info.unreachableCloseOffsets.push_back(in.offset);
        return;
    }

    auto pathIt = state.find(slot);
    int origin{};
    if (pathIt != state.end()) {
        origin = pathIt->second;
        state.erase(pathIt);
    } else {
        auto latestIt = latestInstanceBySlot.find(slot);
        if (latestIt == latestInstanceBySlot.end()) {
            throw std::runtime_error(
                "capture_analysis: CLOSE_UPVALUE at offset " +
                std::to_string(in.offset) + " in function id=" + functionId +
                " closes slot " + std::to_string(slot) +
                ", which no CLOSURE ever captures");
        }
        origin = latestIt->second;
    }

    auto idxIt = rangeIndexByOrigin.find({slot, origin});
    if (idxIt == rangeIndexByOrigin.end()) {
        throw std::runtime_error(
            "capture_analysis: CLOSE_UPVALUE at offset " +
            std::to_string(in.offset) + " in function id=" + functionId +
            " resolved to slot " + std::to_string(slot) + " origin " +
            std::to_string(origin) + ", which has no recorded range");
    }
    info.liveRangesBySlot[slot][idxIt->second].allCloseOffsets.push_back(
        in.offset);
}

// Advances `state` (this block's own fully-converged per-path entry state,
// from Pass 1) across one block's instructions, building the real
// CaptureLiveRange records as it goes. Called once per block, in ascending
// block-index (i.e. ascending offset, see Cfg::blocks) order, for every
// block of the chunk — reachable or not: an unreachable CLOSE_UPVALUE still
// needs to be reported (FunctionCaptureInfo::unreachableCloseOffsets), and
// `latestInstanceBySlot` still advances for unreachable CLOSUREs, mirroring
// the compiler's own single-pass bookkeeping, which does not know or care
// whether the code it just emitted ever runs.
void advanceCommit(const BasicBlock& block, OpenOrigins& state, bool reachable,
                   const std::string& functionId, FunctionCaptureInfo& info,
                   RangeIndex& rangeIndexByOrigin, OriginUnionFind& aliases,
                   const std::unordered_map<int, int>& heightBefore,
                   std::unordered_map<int, int>& latestInstanceBySlot) {
    for (const DecodedInstruction& in : block.instructions) {
        if (in.op == Op::CLOSURE) {
            handleClosureCommit(in, state, reachable, info, rangeIndexByOrigin,
                                aliases, latestInstanceBySlot);
        } else if (in.op == Op::CLOSE_UPVALUE) {
            int slot = heightBefore.at(in.offset) - 1;
            handleCloseUpvalueCommit(in, slot, state, reachable, functionId,
                                     info, rangeIndexByOrigin,
                                     latestInstanceBySlot);
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

void analyzeOneChunk(const DecodedFunction& node, CaptureAnalysis& out) {
    Cfg cfg = buildCfg(node.instructions);
    int entryHeight = 1 + node.function->arity;
    std::unordered_map<int, int> heightBefore =
        computeFrameHeightsForCfg(cfg, entryHeight, node.id);

    OriginUnionFind aliases;
    DataflowResult dataflow = runDataflow(cfg, heightBefore, aliases);

    FunctionCaptureInfo info;
    info.id = node.id;

    RangeIndex rangeIndexByOrigin;
    std::unordered_map<int, int> latestInstanceBySlot;
    // cfg.blocks is in byte order (see cfg.h), so this loop visits every
    // instruction of the chunk exactly once, in program order — required
    // both for ranges of one slot to land in liveRangesBySlot in offset
    // order (the determinism the mission brief's codegen-naming rule
    // depends on) and for `latestInstanceBySlot` to correctly mirror the
    // compiler's own single-pass view.
    for (size_t b = 0; b < cfg.blocks.size(); b++) {
        bool reachable = dataflow.reachable[b] != 0;
        OpenOrigins state = reachable ? dataflow.entryState[b] : OpenOrigins{};
        advanceCommit(cfg.blocks[b], state, reachable, node.id, info,
                      rangeIndexByOrigin, aliases, heightBefore,
                      latestInstanceBySlot);

        if (!reachable || !cfg.blocks[b].successors.empty()) {
            continue;
        }
        // A reachable terminal block (RETURN or MATCH_ERROR): whatever is
        // still open in `state` here closes with the frame, not with an
        // explicit CLOSE_UPVALUE (06_shared_upvalue's `outer` never emits
        // one on any path).
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

std::unordered_map<int, int> computeFrameHeights(const DecodedFunction& node) {
    Cfg cfg = buildCfg(node.instructions);
    return computeFrameHeightsForCfg(cfg, 1 + node.function->arity, node.id);
}

CaptureAnalysis analyzeCaptures(const DecodedFunction& root) {
    CaptureAnalysis result;
    analyzeOneChunk(root, result);
    return result;
}
