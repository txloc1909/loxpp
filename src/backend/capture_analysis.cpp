#include "capture_analysis.h"
#include "exec_objects.h"

#include <stdexcept>
#include <unordered_map>

namespace {

// Slot -> whether its live range is currently open, during the linear walk
// of one chunk.
using OpenSlots = std::unordered_map<int, bool>;

// One forward jump's (JUMP or JUMP_IF_FALSE) span: the address range whose
// code some OTHER path skips past. See SpanTracker for why this exists and
// how it is maintained.
struct PendingSpan {
    int target;
    std::vector<int> reopenOnExit;
};

// Tracks the JUMP_IF_FALSE spans this walk is currently "inside": one entry
// per jump whose target this walk has not reached yet. A CLOSE_UPVALUE seen
// while a span is active is provisional: its slot is recorded on the
// innermost active span (reopenOnExit) and reopened once that span's target
// is reached, instead of staying closed for good — that is what lets a
// later close on the fall-through path, or a later CLOSURE re-capturing the
// same slot, resolve correctly instead of falling through to the next slot
// up (R9/R10).
//
// Only JUMP_IF_FALSE opens a span: it is the one instruction that means
// "some OTHER path skips this code and rejoins at my target," which is
// exactly the ambiguity a CLOSE_UPVALUE inside it needs protecting against.
//
// A loop's own condition check is a JUMP_IF_FALSE too, but its target means
// "the loop is exhausted," not "an alternate path rejoins here" — reopening
// at it would incorrectly undo a per-iteration close that never had any
// alternate. isLoopGuard excludes exactly this one shape: a JUMP_IF_FALSE
// whose span contains the LOOP instruction that repeats it. A for-loop
// compiles its condition check and its increment clause as two separate
// regions, each closed by its OWN LOOP back-edge (one jumping past the
// increment straight to the condition, one at the body's own bottom
// jumping to the increment) — so more than one LOOP instruction can share
// "being inside this same loop" with one JUMP_IF_FALSE, and a naive "is
// this offset between some LOOP's target and its own offset" test matches
// nested conditionals inside the body too (their own target sits between
// two of the loop's OWN back-edges, not outside all of them). The one
// relationship that isolates the loop's own condition-check JUMP_IF_FALSE:
// among every LOOP back-edge whose target is at-or-before this
// JUMP_IF_FALSE (candidates for "some loop already started by here"), take
// the one with the HIGHEST own offset — the most recent back-edge this walk
// has actually passed. A loop-guard's own skip-target lands at-or-after
// that back-edge (it exits past the whole loop); a nested conditional's
// target lands strictly before it (still inside the body the back-edge
// just closed).
//
// A plain, unconditional JUMP never opens its own span (break's own exit
// jump, or a match arm's own exit jump, needs none: with no span active
// over it, its close simply commits, which is already correct — see
// recordClose). The one JUMP that matters is the "skip `else`" jump
// Compiler::ifStatement ALWAYS emits as `then`'s own last instruction —
// even with no `else` clause in the source, since ifStatement emits it
// unconditionally and only then compiles an optional `else` before patching
// it. That JUMP's END (offset + length, not its own offset — the enclosing
// JUMP_IF_FALSE's target is patched right after it) always lands EXACTLY on
// the enclosing JUMP_IF_FALSE's target, and it EXTENDS that span to cover
// `else` (or, with none, the one-instruction gap ifStatement still leaves
// before its own target) instead of ending it, so a slot closed in `then`
// stays unreachable through that whole region instead of being wrongly
// reopened partway through it.
class SpanTracker {
  public:
    explicit SpanTracker(const DecodedFunction& node) {
        for (const DecodedInstruction& ins : node.instructions) {
            if (ins.op == Op::LOOP) {
                m_loopBackEdges.push_back({ins.offset, ins.jumpTarget});
            }
        }
    }

    // Reopens every span this walk has passed, then pushes a new span or
    // extends the innermost active one if `ins` calls for it. Call this
    // before acting on `ins` itself.
    void advance(const DecodedInstruction& ins, OpenSlots& openSlots) {
        popSpansEndingAt(ins.offset - 1, openSlots); // never ambiguous
        if (extendsElseSkip(ins)) {
            m_stack.back().target = ins.jumpTarget;
            return;
        }
        popSpansEndingAt(ins.offset, openSlots);
        if (ins.op == Op::JUMP_IF_FALSE && ins.jumpTarget > ins.offset &&
            !isLoopGuard(ins)) {
            m_stack.push_back({ins.jumpTarget, {}});
        }
    }

    [[nodiscard]] bool active() const { return !m_stack.empty(); }

    // Records that `slot`, just closed by the current instruction, should
    // reopen once the innermost active span's target is reached. Call only
    // when active().
    void reopenOnExit(int slot) { m_stack.back().reopenOnExit.push_back(slot); }

  private:
    struct LoopBackEdge {
        int offset;
        int target;
    };

    // The enclosing JUMP_IF_FALSE's target is patched to right AFTER this
    // JUMP (Compiler::ifStatement patches thenJump once elseJump and the
    // optional `else` are both compiled), so the match is on where `ins`
    // ENDS, not where it starts.
    [[nodiscard]] bool extendsElseSkip(const DecodedInstruction& ins) const {
        return !m_stack.empty() &&
               m_stack.back().target == ins.offset + ins.length &&
               ins.op == Op::JUMP && ins.jumpTarget > ins.offset;
    }

    [[nodiscard]] bool
    isLoopGuard(const DecodedInstruction& jumpIfFalse) const {
        const LoopBackEdge* mostRecent = nullptr;
        for (const LoopBackEdge& edge : m_loopBackEdges) {
            if (edge.target <= jumpIfFalse.offset &&
                (mostRecent == nullptr || edge.offset > mostRecent->offset)) {
                mostRecent = &edge;
            }
        }
        return mostRecent != nullptr &&
               jumpIfFalse.jumpTarget >= mostRecent->offset;
    }

    void popSpansEndingAt(int offset, OpenSlots& openSlots) {
        while (!m_stack.empty() && m_stack.back().target <= offset) {
            for (int slot : m_stack.back().reopenOnExit) {
                openSlots[slot] = true;
            }
            m_stack.pop_back();
        }
    }

    std::vector<LoopBackEdge> m_loopBackEdges;
    std::vector<PendingSpan> m_stack;
};

// Resolves one CLOSE_UPVALUE to the slot it closes. Compiler::endScope and
// Compiler::emitLoopCleanup (compiler.cpp) both reclaim locals in strictly
// descending slot order, and a local's scope depth is monotonic in its slot
// index, so among currently-open captured slots the highest index is always
// the one that closes next. That holds within one straight-line run of
// reclaim instructions. It does NOT hold across two runs that close the same
// slot on mutually exclusive paths (break/continue/match-arm exit vs. the
// normal fall-through) — the first such close, taken alone, looks
// permanent, and a later, unrelated CLOSE_UPVALUE for a different slot can
// then be misattributed to it (R9/R10). SpanTracker exists to reopen a slot
// once its alternate-exit window has genuinely closed, so that by the time
// `resolveCloseTarget` runs, "currently open" again means what it says.
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
// this linear scan sees all of them in program order.
//
// When this instruction sits inside an active span (see SpanTracker), the
// slot it resolves to is recorded in that span so it is reopened, not left
// permanently closed, once the span's target proves every alternate path
// has reconverged. That is what lets a later close on a different
// mutually-exclusive path (or a later CLOSURE re-capturing the same slot,
// R10) resolve correctly instead of falling through to the next slot up
// (R9).
//
// A CLOSE_UPVALUE that matches no open slot at all is tolerated, not thrown,
// as long as `lastClosedSlot` shows some slot has closed through a genuine
// open-slot match since the most recent new capture. SpanTracker handles
// every alternate-exit shape this pass has a concrete counter-example for;
// this tolerance remains as a narrower fallback for a shape it does not
// model. It never attributes the close to any range, so it changes no
// reported value. A CLOSE_UPVALUE with no open slot AND no slot closed yet
// at all in this chunk is still unexplained, and still throws.
void recordClose(const DecodedInstruction& ins, const std::string& functionId,
                 FunctionCaptureInfo& info, OpenSlots& openSlots,
                 int& lastClosedSlot, SpanTracker& spans) {
    int slot = resolveCloseTarget(openSlots);
    if (slot != -1) {
        info.liveRangesBySlot.at(slot).back().end = ins.offset;
        openSlots[slot] = false;
        lastClosedSlot = slot;
        if (spans.active()) {
            spans.reopenOnExit(slot);
        }
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
    SpanTracker spans(node);

    for (const DecodedInstruction& ins : node.instructions) {
        spans.advance(ins, openSlots);

        if (ins.op == Op::CLOSURE) {
            recordCapture(ins, info, openSlots, lastClosedSlot);
        } else if (ins.op == Op::CLOSE_UPVALUE) {
            recordClose(ins, node.id, info, openSlots, lastClosedSlot, spans);
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
