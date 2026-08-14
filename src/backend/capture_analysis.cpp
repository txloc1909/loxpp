#include "capture_analysis.h"
#include "exec_objects.h"

#include <stdexcept>
#include <unordered_map>

namespace {

// Slot -> whether its live range is currently open, during the linear walk
// of one chunk.
using OpenSlots = std::unordered_map<int, bool>;

// Resolves one CLOSE_UPVALUE to the slot it closes. Compiler::endScope and
// Compiler::emitLoopCleanup (compiler.cpp) both reclaim locals in strictly
// descending slot order, and a local's scope depth is monotonic in its slot
// index, so among currently-open captured slots the highest index is always
// the one that closes next. That holds globally, not just within one
// scope-exit run, because block scopes nest strictly: an inner scope always
// closes before the outer scope that contains it, so a lower slot cannot
// still be waiting to close while a higher one, declared later, remains
// open. This invariant depends on every early-exit path (break, continue,
// match-arm exit) also closing a captured local with CLOSE_UPVALUE rather
// than a plain POP; a compiler that reclaimed a captured slot with POP on
// one of those paths would leave it open here, and a later, unrelated
// CLOSE_UPVALUE for a different slot could then be misattributed to it. The
// throw in recordClose (no open slot at all) does not catch that
// misattribution — it catches only a CLOSE_UPVALUE with no open slot to
// name, which is a different failure. recordClose's lastClosedSlot
// tolerance (see below) does not widen the misattribution risk here: it
// never picks an open slot at all, so it cannot pick the WRONG one — it
// only decides whether to throw or stay silent when NO slot is open.
// Guarding fully against a compiler regression here would need per-offset
// stack tracking (N2), which this node deliberately does not depend on; the
// actual guard against the scenario above is Compiler::emitLoopCleanup
// itself matching endScope.
int resolveCloseTarget(const OpenSlots& openSlots) {
    int target = -1;
    for (const auto& [slot, open] : openSlots) {
        if (open && slot > target) {
            target = slot;
        }
    }
    return target;
}

// Folds one CLOSURE instruction's local captures into `info`/`openSlots`.
//
// A live range opens only here, never at a plain GET_LOCAL/SET_LOCAL. A slot
// number can be reused, in a later sibling scope, by a variable nothing
// captures; opening on every reference would wrongly fold that later,
// uncaptured incarnation into the capture map. Opening only on capture
// guarantees every tracked range is genuinely captured, so it is guaranteed
// to close via CLOSE_UPVALUE, never a plain POP.
void recordCapture(const DecodedInstruction& ins, FunctionCaptureInfo& info,
                   OpenSlots& openSlots, int& lastClosedSlot) {
    for (const ClosureUpvalue& up : ins.upvalues) {
        if (!up.isLocal) {
            continue; // names a slot in a grandparent's frame, not this chunk's
        }
        int slot = up.index;
        std::vector<CaptureLiveRange>& ranges = info.liveRangesBySlot[slot];
        if (!openSlots[slot]) {
            CaptureLiveRange range;
            range.slot = slot;
            range.firstCaptureOffset = ins.offset;
            ranges.push_back(range);
            openSlots[slot] = true;
            // A new range starting anywhere invalidates any pending
            // "alternate early exit" tolerance for a slot closed earlier —
            // see recordClose. Past this point a stray orphan CLOSE_UPVALUE
            // is drift again, not a leftover exit path of the old range.
            lastClosedSlot = -1;
        }
        ranges.back().capturingClosureOffsets.push_back(ins.offset);
    }
}

// Resolves one CLOSE_UPVALUE and closes the range it targets.
//
// One captured local can have more than one CLOSE_UPVALUE in the chunk when
// its scope has more than one exit path: Compiler::emitLoopCleanup emits one
// on the break path, one on the continue path, and one on a match arm's own
// exit, in addition to the normal fall-through's endScope. Those paths are
// mutually exclusive at runtime — only one fires per actual execution — but
// this linear scan sees all of them in program order, and an unrelated
// closed-and-reopened capture can sit between two of them (an inner block's
// own capture, fully opened and closed, in between a continue and a break
// that both exit the same outer loop). The first CLOSE_UPVALUE for a slot
// closes its range as usual, through the open-slot match below.
//
// A later CLOSE_UPVALUE that matches no open slot is tolerated, not thrown,
// as long as `lastClosedSlot` shows some slot has closed through a genuine
// open-slot match since the most recent new capture. This pass cannot verify
// that this specific instruction is that same slot's alternate exit — doing
// that needs per-offset stack or control-flow tracking (N1/N2), which this
// node deliberately does not depend on — so it does not attribute the close
// to any range or touch `end` again. That is safe: every range's `end` was
// already set by its own first, genuine close, so an untouched later close
// changes no reported value. It only narrows, never removes, the original
// safety net: a CLOSE_UPVALUE with no open slot AND no slot closed yet at
// all in this chunk is still unexplained, and still throws.
void recordClose(const DecodedInstruction& ins, const std::string& functionId,
                 FunctionCaptureInfo& info, OpenSlots& openSlots,
                 int& lastClosedSlot) {
    int slot = resolveCloseTarget(openSlots);
    if (slot != -1) {
        info.liveRangesBySlot.at(slot).back().end = ins.offset;
        openSlots[slot] = false;
        lastClosedSlot = slot;
        return;
    }
    if (lastClosedSlot != -1) {
        return; // an alternate early-exit path re-closing the same range
    }
    throw std::runtime_error("capture_analysis: CLOSE_UPVALUE at offset " +
                             std::to_string(ins.offset) +
                             " in function id=" + functionId +
                             " has no open captured live range to close");
}

// A function's own top-level scope closes with the frame, not with an
// explicit CLOSE_UPVALUE (06_shared_upvalue's `outer` never emits one). Every
// slot still open once the chunk ends closes here instead.
void closeImplicitRanges(int chunkEnd, const OpenSlots& openSlots,
                         FunctionCaptureInfo& info) {
    for (const auto& [slot, open] : openSlots) {
        if (open) {
            CaptureLiveRange& range = info.liveRangesBySlot.at(slot).back();
            range.end = chunkEnd;
            range.closedImplicitly = true;
        }
    }
}

// A live range wholly inside some LOOP's back-edge span re-executes its
// declaration every iteration, so it needs a fresh cell each time
// (V1_fresh_cell), unlike a range no LOOP instruction wraps (V3_loopvar, and
// any capture outside a loop).
//
// A closedImplicitly range never qualifies: its `end` is the chunk length,
// which is always greater than any real instruction offset, so the
// `range.end <= ins.offset` half of the test below already excludes it. That
// makes it correct (if scoped to the whole call, it is by definition never
// wholly inside one loop's back edge) without a separate early check.
void markPerIterationRanges(const DecodedFunction& node,
                            FunctionCaptureInfo& info) {
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::LOOP) {
            continue;
        }
        for (auto& [slot, ranges] : info.liveRangesBySlot) {
            for (CaptureLiveRange& range : ranges) {
                if (range.firstCaptureOffset >= ins.jumpTarget &&
                    range.end <= ins.offset) {
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
    FunctionCaptureInfo info;
    info.id = node.id;

    OpenSlots openSlots;
    int lastClosedSlot = -1;
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSURE) {
            recordCapture(ins, info, openSlots, lastClosedSlot);
        } else if (ins.op == Op::CLOSE_UPVALUE) {
            recordClose(ins, node.id, info, openSlots, lastClosedSlot);
        }
    }

    int chunkEnd = static_cast<int>(node.function->chunk.size());
    closeImplicitRanges(chunkEnd, openSlots, info);
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
