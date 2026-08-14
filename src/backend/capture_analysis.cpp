#include "capture_analysis.h"
#include "exec_objects.h"

#include <stdexcept>

namespace {

// Slot -> whether its live range is currently open, during the linear walk
// of one chunk.
using OpenSlots = std::unordered_map<int, bool>;

// Resolves one CLOSE_UPVALUE to the slot it closes. Compiler::endScope
// (compiler.cpp) always reclaims locals in strictly descending slot order,
// and a local's scope depth is monotonic in its slot index, so among
// currently-open captured slots the highest index is always the one that
// closes next. That holds globally, not just within one scope-exit run,
// because block scopes nest strictly: an inner scope always closes before
// the outer scope that contains it, so a lower slot cannot still be waiting
// to close while a higher one, declared later, remains open.
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
                   OpenSlots& openSlots) {
    for (const ClosureUpvalue& up : ins.upvalues) {
        if (!up.isLocal) {
            continue; // names a slot in a grandparent's frame, not this chunk's
        }
        int slot = up.index;
        std::vector<CaptureLiveRange>& ranges = info.liveRangesBySlot[slot];
        if (!openSlots[slot]) {
            CaptureLiveRange range;
            range.slot = slot;
            range.start = ins.offset;
            ranges.push_back(range);
            openSlots[slot] = true;
        }
        ranges.back().capturingClosureOffsets.push_back(ins.offset);
    }
}

// Resolves one CLOSE_UPVALUE and closes the range it targets.
void recordClose(const DecodedInstruction& ins, const std::string& functionId,
                 FunctionCaptureInfo& info, OpenSlots& openSlots) {
    int slot = resolveCloseTarget(openSlots);
    if (slot == -1) {
        throw std::runtime_error("capture_analysis: CLOSE_UPVALUE at offset " +
                                 std::to_string(ins.offset) +
                                 " in function id=" + functionId +
                                 " has no open captured live range to close");
    }
    info.liveRangesBySlot.at(slot).back().end = ins.offset;
    openSlots[slot] = false;
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
void markPerIterationRanges(const DecodedFunction& node,
                            FunctionCaptureInfo& info) {
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op != Op::LOOP) {
            continue;
        }
        for (auto& [slot, ranges] : info.liveRangesBySlot) {
            for (CaptureLiveRange& range : ranges) {
                if (range.closedImplicitly) {
                    continue; // scoped to the whole call, never re-entered by a
                              // back-edge
                }
                if (range.start >= ins.jumpTarget && range.end <= ins.offset) {
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
    for (const DecodedInstruction& ins : node.instructions) {
        if (ins.op == Op::CLOSURE) {
            recordCapture(ins, info, openSlots);
        } else if (ins.op == Op::CLOSE_UPVALUE) {
            recordClose(ins, node.id, info, openSlots);
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
