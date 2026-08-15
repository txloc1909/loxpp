#pragma once

// Target-independent capture analysis (P4a in
// notes/bytecode-translation-problems.md). It consumes the decoded
// instruction tree from chunk_decoder.h (N0) and answers, per chunk: which
// local slots a nested closure captures, the live range of each captured
// slot, and which captured slots several closures share one cell for.
//
// This pass does not emit code and carries no JVM or CLR knowledge — node N7
// is the only consumer, on whichever target it runs. N7 reads only the SLOT
// SET this pass reports (FunctionCaptureInfo::liveRangesBySlot's keys, via
// jvm_emitter.cpp's capturedSlots) — a runtime `instanceof` check at every
// CLOSURE/GET_LOCAL/SET_LOCAL of a captured slot replaces the per-range,
// per-close, and per-path detail below (PR #111 R7). That detail stays here
// because it is still the honest model of what the VM does, and a future
// node — or a future N7 revision that trades the runtime check for a static
// one — needs it. Each field below says, on its own line, whether today's
// N7 reads it.
//
// The whole hazard this pass exists to catch: CLOSE_UPVALUE is not a no-op.
// It ends a captured slot's live range, and the next time that slot's
// declaration runs, it needs a fresh cell. See P4 and the V1/V3 probes.
//
// THE FINAL DESIGN (referee amendment 3, PR #101, 2026-08-15). Three review
// rounds (amendments 1 and 2) tried to INFER which slot a CLOSE_UPVALUE
// closes, from tracked dataflow state and a static overlay. That inference
// was never needed, and the overlay it required had a real gap (R22): the
// compiler emits one CLOSE_UPVALUE per exit path that crosses a captured
// local's scope (one for each of `break`, `continue`, and the fall-through),
// so a slot can close more than once, and an overlay that keeps one entry
// per slot loses every close after the first.
//
// The fix is a fact, not a heuristic: `vm.cpp` runs
// `closeUpvalues(stackTop - 1); pop();` for CLOSE_UPVALUE. The closed slot
// *is* the frame stack height, right before the instruction runs, minus one.
// This pass computes that height exactly, over the CFG (N1), with a fixed
// per-opcode effect table and a throw on any control-flow merge that
// disagrees — see computeFrameHeights below. `closeSlot(offset) =
// heightBefore(offset) - 1` then names every CLOSE_UPVALUE's target
// unambiguously, with no reference to what this pass's own dataflow thinks
// is open. That deleted the static overlay, the offset-resolution table, and
// the iteration-to-a-fixed-point that amendments 1 and 2 needed to make the
// overlay's own two-layer resolution converge.
//
// What survives from amendments 1 and 2: the CFG dataflow (mirrors the VM's
// open-upvalue list: a slot is open at a block's entry exactly when some
// predecessor that reaches it still has it open) and the union-find instance
// identity (mirrors `captureUpvalue`'s reuse of an already-open upvalue at
// one stack location — two closures that capture the same live incarnation
// share one cell, however many CFG paths bring them together). Both restate
// a real VM mechanism; the overlay did not, and it is gone.
//
// This pass still carries no full stack simulation (N2): it computes only
// the raw height needed to resolve CLOSE_UPVALUE, not the local/temporary
// split N2 needs for JVM locals vs. the operand stack. Every offset this pass
// reports beyond a resolved close is still a bound on a slot's capture, not a
// per-execution-path fact — see FunctionCaptureInfo::firstCaptureOffset below
// for what that means for N7.
//
// Height is a per-node-3 concept (N2 and N3 are independent analyses; see
// notes/backend-implementation-dag.md), but computeFrameHeights is exposed
// publicly, not kept file-private, for two reasons: N9 (and any later node
// that needs the live local set at an offset) can reuse it instead of
// re-deriving the same per-opcode effect table, and it lets a test verify,
// independently of this pass's own bookkeeping, that every resolved close's
// slot really is height-before minus one — the definitional fact the whole
// design rests on.

#include "cfg.h"
#include "chunk_decoder.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// One continuous live range of one captured local slot: the span during
// which one heap ref-cell backs that slot.
struct CaptureLiveRange {
    int slot{-1};

    // The offset of the FIRST CLOSURE instruction, in program order, that
    // captures this incarnation of the slot. This is a bound, not the
    // declaration point: the real `var` (or loop-header) declaration that
    // creates this incarnation always runs at or before this offset, but not
    // always immediately before it, and not always on every path that
    // reaches this offset.
    //
    // Concretely, `firstCaptureOffset` is NOT guaranteed to dominate every
    // entry in `allCloseOffsets`: a jump can land inside
    // (firstCaptureOffset, someEnd] without ever having run
    // firstCaptureOffset. V3_loopvar.lox is the standing counter-example — a
    // zero-trip loop reaches this range's CLOSE_UPVALUE without ever running
    // the CLOSURE that opened it, because the loop variable's real
    // declaration (the init clause) runs once, before the loop, while the
    // capturing CLOSURE sits inside the conditionally-skipped body. This
    // pass uses the CFG (N1) and frame heights to resolve WHICH slot a
    // CLOSE_UPVALUE closes and which closures share one cell, but that is a
    // different question from WHERE a slot's declaration runs on a given
    // path — that needs the abstract stack (N2), which this node still does
    // not depend on, so it reports the bound it can prove and no more.
    //
    // When several CLOSURE offsets share this one live instance (see
    // capturingClosureOffsets below — an ordinary if/else where every arm
    // captures the same outer local), this field is the MINIMUM of them,
    // which is also always the one program order reaches first.
    int firstCaptureOffset{-1};

    // Every real CLOSE_UPVALUE offset that resolves to this exact range, one
    // per path that closes it. A range genuinely gets more than one when its
    // scope has more than one mutually exclusive exit — break, continue, a
    // match arm's own exit, and the normal fall-through can each emit their
    // OWN CLOSE_UPVALUE for the same captured local, on their own path — and
    // exactly one of them fires per real execution, never more than one and
    // never zero (unless the range is closedImplicitly, in which case this
    // is empty: see that field). One captured slot's incarnation can also
    // rack up more than one entry here from CLOSE_UPVALUE instructions that
    // never dynamically reach this range at all (R22): the compiler emits
    // one close per exit path regardless of which branch a real run takes,
    // so a close that this pass's own dataflow finds closed on every
    // reachable path still names this range, by height, as a genuine no-op
    // instruction — real bytecode, attributed correctly, just never
    // executed as a live close on that particular path.
    //
    // This field exists so a consumer (or a test — see
    // checkNoOrphanCloseUpvalues in test_backend_capture.cpp) can verify
    // every alternate is accounted for, not only the last one.
    std::vector<int> allCloseOffsets;

    // True when this range is still open (per the per-path dataflow) at
    // SOME reachable RETURN or MATCH_ERROR: that path returns before any
    // code closes it, so the frame's own teardown
    // (closeUpvalues(frame->slots)) is what ends it on that path, not an
    // explicit CLOSE_UPVALUE. This can COEXIST with a non-empty
    // `allCloseOffsets`: one path can return with the range still open while
    // a DIFFERENT path closes the very same range explicitly — both are
    // real, mutually exclusive ways this one range ends, exactly like two
    // entries in `allCloseOffsets` are. The common case with no explicit
    // close at all is the function's own top-level scope, which the
    // compiler never wraps in an explicit close on any path
    // (06_shared_upvalue's `outer`).
    bool closedImplicitly{false};

    // True when some LOOP's back-edge span [target, offset] contains
    // `firstCaptureOffset` AND every entry in `allCloseOffsets` (there must
    // be at least one): this exact bytecode span re-executes every
    // iteration, so the declaration re-runs and needs a fresh cell each time
    // (V1_fresh_cell). False when `allCloseOffsets` is empty (a
    // closedImplicitly-only range is scoped to the whole call, never
    // re-declared) or when no single loop's span covers every explicit end
    // (V3_loopvar, and any capture outside a loop) — one cell serves the
    // whole range in both of those cases, however many times it runs.
    bool perIteration{false};

    // Offsets of every CLOSURE instruction, in this chunk, that captures
    // this exact range. More than one entry means those closures share ONE
    // cell — do not allocate one per closure. Two closures in the SAME
    // block, capturing an already-open cell sequentially, is one way to get
    // here (06_shared_upvalue, V2_shared). Two closures on MUTUALLY
    // EXCLUSIVE if/else (or match) arms, each capturing the same outer,
    // never-redeclared local, is another (referee amendment 2, rule 4) —
    // whichever arm runs, the runtime holds one cell, so both offsets name
    // the same instance here too.
    std::vector<int> capturingClosureOffsets;
};

// Everything this pass knows about one chunk (one ObjFunction's body).
struct FunctionCaptureInfo {
    // Matches DecodedFunction::id.
    std::string id;

    // Captured slot -> its live ranges, in offset order. A slot absent from
    // this map is never captured in this chunk and needs no ref-cell.
    // std::map (not unordered_map): N7's codegen must walk this in a stable
    // order so generated class/method names stay stable across runs (see the
    // mission brief's determinism rule).
    //
    // N7 reads only this map's KEYS (jvm_emitter.cpp's capturedSlots) — which
    // slots are ever captured, not which range is open at which offset. The
    // runtime `instanceof` check (see the file header above) settles raw-vs-
    // cell at each site directly, so N7 does not walk `CaptureLiveRange`
    // itself today.
    std::map<int, std::vector<CaptureLiveRange>> liveRangesBySlot;

    // How this function's OWN upvalue array is wired: entry i names either
    // a parent local (isLocal true) or a parent upvalue (isLocal false).
    // Copied from the CLOSURE instruction, in the PARENT chunk, that creates
    // this function. Empty for the root script, which no CLOSURE creates.
    std::vector<ClosureUpvalue> ownUpvalues;

    // CLOSE_UPVALUE offsets found in a CFG block this pass never reaches
    // from block 0 — dead code, for example scope-exit cleanup after an
    // unconditional `return`. These never execute on any real run, so they
    // are attributed to no range; this field exists only so a cross-check
    // (checkNoOrphanCloseUpvalues in test_backend_capture.cpp) can tell
    // "unreachable" apart from "this pass lost track of a real close". N7
    // does not read this field.
    std::vector<int> unreachableCloseOffsets;

    // A REACHABLE CLOSE_UPVALUE, closed on every path that reaches it (a
    // static close, R22), whose most-recent same-slot CLOSURE is itself
    // unreachable, so it opened no range (R26 — referee amendment 4, PR
    // #101, round 10). Every capture of this incarnation is dead code, so
    // no cell can exist at this offset on any real run: the close's only
    // run-time effect is its own pop.
    //
    // N7 does not read this field either (PR #111 R7). Its CLOSE_UPVALUE
    // case in jvm_emitter.cpp emits NO bytecode at all, for every close on
    // every path — reachable, unreachable, or statically dead alike — so
    // the "pop alone, no cell operation" rule this comment used to hand N7
    // holds by construction, not because N7 consults this list. A future
    // lowering that treats CLOSE_UPVALUE cases differently by kind must
    // read this field to keep that rule true.
    std::vector<int> staticallyDeadCloseOffsets;
};

// Capture info for a whole ObjFunction tree (see decodeFunctionTree),
// keyed by DecodedFunction::id. std::map for the same determinism reason as
// FunctionCaptureInfo::liveRangesBySlot.
struct CaptureAnalysis {
    std::map<std::string, FunctionCaptureInfo> functions;
};

// Computes, for one function's own chunk (not recursing into nested
// functions — each has its own frame and its own call to this), the frame
// stack height immediately BEFORE every instruction reachable from offset 0,
// over the CFG (N1). Entry height is 1 + arity (slot 0 = callee/receiver,
// slots 1..arity = parameters; bytecode-translation-problems.md P5, and
// notes/backend-implementation-dag.md's abstract-stack node N2 documents the
// same convention independently). Each opcode has a fixed net stack effect —
// see frameHeightEffect in capture_analysis.cpp, mirroring vm.cpp exactly.
//
// Throws std::runtime_error if two edges into one block disagree on the
// height they bring — a real compiler/decoder stack-balance bug, not a
// modelling gap this pass tolerates (unlike N2's abstract stack, this pass
// tracks raw height only, with no locals/temporaries split, so it has no
// weaker invariant to fall back on). This check caught a real compiler
// defect during this pass's own development: a match arm body that ends in
// a statement, not an expression, left the frame one slot short on some
// paths (see the PR that fixed src/compiler.cpp's compileMatchArm).
//
// An offset no path from function entry reaches has no entry in the
// returned map; querying one is a caller bug, not a case this function
// handles softly.
std::unordered_map<int, int> computeFrameHeights(const DecodedFunction& node);

// Builds the CFG (N1) of every chunk in `root`'s tree and derives the
// capture map (referee amendment 3, PR #101, 2026-08-15 — the final design;
// see the header comment above): compute exact per-offset frame heights,
// resolve every CLOSE_UPVALUE's target slot directly from
// `computeFrameHeights(...) [offset] - 1`, then run one CFG dataflow
// fixpoint to find which slot is open at each point and which live
// instances the union-find proves are one runtime cell (amendment 2,
// unchanged), and one program-order pass to attribute every real
// CLOSE_UPVALUE to one of four total outcomes (referee amendment 4, PR #101,
// round 10 — legal dead code makes these four exhaustive, not three):
//   1. unreachable — the close is itself dead code
//      (FunctionCaptureInfo::unreachableCloseOffsets);
//   2. dynamic — the dataflow shows the slot open on the path reaching it
//      (attributed to a real range's allCloseOffsets);
//   3. static-attributed — the slot is closed on every path reaching it
//      (R22: a captured local's scope can emit more than one CLOSE_UPVALUE,
//      one per exit path, and every one of them is real bytecode that must
//      resolve to the same instance), and the most recent CLOSURE this pass
//      has seen for that exact slot, in program order, opened a real range
//      (also allCloseOffsets);
//   4. statically dead — the same as 3, except that most recent CLOSURE
//      opened no range, because it is unreachable too (R26:
//      FunctionCaptureInfo::staticallyDeadCloseOffsets).
//
// Throws std::runtime_error only for the two outcomes the emission contract
// (mission brief section 5c) makes impossible for compiler-correct input: a
// REACHABLE close whose height-derived slot has no CLOSURE anywhere in
// program order before it (no incarnation of that slot was ever named), or a
// DYNAMIC close (the slot was path-open) whose origin has no recorded range.
// Either signals decoder or compiler drift, not a normal program.
CaptureAnalysis analyzeCaptures(const DecodedFunction& root);
