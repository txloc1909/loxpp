#pragma once

// Target-independent capture analysis (P4a in
// notes/bytecode-translation-problems.md). It consumes the decoded
// instruction tree from chunk_decoder.h (N0) and answers, per chunk: which
// local slots a nested closure captures, the live range of each captured
// slot, and which captured slots several closures share one cell for.
//
// This pass does not emit code and carries no JVM or CLR knowledge — node N7
// is the only consumer, on whichever target it runs.
//
// The whole hazard this pass exists to catch: CLOSE_UPVALUE is not a no-op.
// It ends a captured slot's live range, and the next time that slot's
// declaration runs, it needs a fresh cell. See P4 and the V1/V3 probes.
//
// Scope note: this pass walks the CFG (N1, buildCfg in cfg.h), not the flat
// instruction list, and tracks captured-slot openness per basic block. A
// CLOSE_UPVALUE carries no operand, so a flat, order-only walk cannot always
// tell two mutually exclusive alternate exits of ONE live range (break,
// continue, and the normal fall-through, each closing the same per-iteration
// capture on its own path) apart from a real scope exit followed by a
// genuinely different, later variable that reuses the same slot number. This
// pass still carries no stack simulation (N2), so every offset it reports is
// a bound on a slot's capture, not a per-execution-path fact. See
// FunctionCaptureInfo::firstCaptureOffset below for what that means for N7.
//
// Referee amendment 1 (PR #101, 2026-08-15): a per-path open set alone is not
// sound. The compiler emits one CLOSE_UPVALUE per STATICALLY captured local
// of a scope (decided at compile time from Local::isCaptured), while this
// pass only opens a slot in its per-path state when a CLOSURE actually RUNS
// on that path. A branch that captures a slot and then returns leaves a
// SIBLING path running that scope's CLOSE_UPVALUE for a slot it never
// dynamically opened. To resolve such a close soundly, the pass also
// maintains a STATIC OVERLAY: one program-order map, threaded through every
// block in offset order regardless of reachability, that mirrors the
// compiler's own single-pass view of "captured and not yet closed" (see
// analyzeOneChunk in capture_analysis.cpp). Each CLOSE_UPVALUE resolves
// against the HIGHER of the per-path state and the overlay: state wins when
// it holds the target (a real, dynamic close — vm.cpp's closeUpvalues
// actually ends a cell there); the overlay wins only when state does not (a
// static close — a genuine dynamic no-op on this path, because no cell ever
// opened here to close). The one resolution table this produces (offset ->
// target slot) must be found by iterating the path fixpoint and the
// resolution together to a stable point — see analyzeOneChunk's own
// iteration loop, and advanceProbe's doc comment for why a single pass is
// not enough.
//
// Referee amendment 2 (PR #101, 2026-08-15) — RANGE IDENTITY IS A LIVE
// INSTANCE, NOT A TOKEN. Amendment 1 keyed a range by the pair (slot,
// CLOSURE offset). That is only a range's INITIAL NAME. Two different
// CLOSURE offsets for the same slot can be the SAME live instance — one
// runtime cell — exactly when `vm.cpp`'s own `captureUpvalue` would reuse an
// already-open upvalue rather than create a new one: an ordinary if/else (or
// match) where every arm captures the same, never-redeclared outer local
// opens it with a DIFFERENT CLOSURE offset per arm, and all of them are the
// SAME cell where the arms rejoin (06_shared_upvalue's two-closures-one-cell
// fact, moved onto branches — see CaptureAnalysisTest.
// IfElseArmsCapturingOneOuterVariableShareOneCell). The corrected rule:
//   1. A capture token is (slot, CLOSURE offset) — a range's initial name,
//      not its identity.
//   2. A CLOSURE that finds its slot OPEN on the current path joins the
//      instance the path already carries: it adds its own offset to
//      capturingClosureOffsets: it does not open a new range.
//   3. A CLOSURE that finds its slot CLOSED on every path reaching it starts
//      a NEW instance.
//   4. At a CFG merge where predecessors bring the SAME slot open under
//      DIFFERENT tokens, those tokens name ONE instance (OriginUnionFind in
//      capture_analysis.cpp) — whichever arm actually ran, the VM holds one
//      open upvalue for that stack location at the merge, so there is one
//      cell. This case is NOT decoder or compiler drift; joinInto unites the
//      tokens instead of throwing.
//   5. The only throw left is a CLOSE_UPVALUE that names no open slot in
//      either the per-path state or the static overlay — R19's own
//      "has no open captured slot to close in either layer".
//   6. An instance's firstCaptureOffset is the MINIMUM capturing CLOSURE
//      offset of its united class (OriginUnionFind::unite always keeps the
//      smaller offset as canonical).

#include "cfg.h"
#include "chunk_decoder.h"

#include <map>
#include <string>
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
    // pass uses the CFG (N1) to resolve WHICH slot a CLOSE_UPVALUE closes
    // and which closures share one cell, but that is a different question
    // from WHERE a slot's declaration runs on a given path — that needs the
    // abstract stack (N2), which this node still does not depend on, so it
    // reports the bound it can prove and no more.
    //
    // N7 must NOT use this field to decide where to insert cell-allocation
    // code. The safe algorithm is: walk the chunk once for codegen (as N7
    // already must), and for any local slot this map marks captured within
    // the range it is currently in, lazily allocate a fresh cell on the
    // FIRST write N7's own walk encounters since the range opened (function
    // entry, or the previous CLOSE_UPVALUE for that slot) — never keyed off
    // this offset directly. That is correct regardless of how many paths
    // reach the write, because it is decided by execution, not by a static
    // offset comparison.
    //
    // Referee amendment 2, rule 6: when several CLOSURE offsets share this
    // one live instance (see capturingClosureOffsets below — an ordinary
    // if/else where every arm captures the same outer local), this field is
    // the MINIMUM of them, which is also always the one program order
    // reaches first.
    int firstCaptureOffset{-1};

    // Every real CLOSE_UPVALUE offset that resolves to this exact range, one
    // per path that closes it. A range genuinely gets more than one when its
    // scope has more than one mutually exclusive exit — break, continue, a
    // match arm's own exit, and the normal fall-through can each emit their
    // OWN CLOSE_UPVALUE for the same captured local, on their own path — and
    // exactly one of them fires per real execution, never more than one and
    // never zero (unless the range is closedImplicitly, in which case this
    // is empty: see that field).
    //
    // R18 (referee amendment 1): earlier drafts also carried an `end` field,
    // duplicating `allCloseOffsets.back()` and, for a closedImplicitly-only
    // range, a chunk-length sentinel that was not a real offset. Removed —
    // read `allCloseOffsets` and `closedImplicitly` instead, and neither
    // mixes a real offset with a sentinel.
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
    // "unreachable" apart from "this pass lost track of a real close".
    std::vector<int> unreachableCloseOffsets;
};

// Capture info for a whole ObjFunction tree (see decodeFunctionTree),
// keyed by DecodedFunction::id. std::map for the same determinism reason as
// FunctionCaptureInfo::liveRangesBySlot.
struct CaptureAnalysis {
    std::map<std::string, FunctionCaptureInfo> functions;
};

// Builds the CFG (N1) of every chunk in `root`'s tree and derives the
// capture map per execution path (round-3 referee decision, amended by
// referee amendments 1 and 2, PR #101): a dataflow over the CFG, not a flat,
// order-only walk over the instruction list. A normal program can
// legitimately have more than one real CLOSE_UPVALUE for the same capture,
// on mutually exclusive break/continue/match-arm-exit/fall-through paths; a
// flat walk cannot tell that apart from a real scope exit followed by a
// genuinely different, later variable that reuses the same slot number,
// because CLOSE_UPVALUE carries no operand. The CFG resolves it: a slot is
// open at a block's entry exactly when some predecessor that reaches it
// still has it open. CLOSE_UPVALUE resolves against the higher of that
// per-path state and a program-order static overlay (see the header comment
// above and analyzeOneChunk in capture_analysis.cpp) — the per-path state
// alone is not sound when a branch captures a slot and returns, leaving a
// sibling path to run that scope's close for a slot it never dynamically
// opened. Throws std::runtime_error only if a CLOSE_UPVALUE names no open
// captured slot in EITHER layer on a REACHABLE block's fully-converged path
// — that signals decoder or compiler drift, not a normal program.
//
// Referee amendment 2, rule 4: two predecessors reaching one block CAN
// disagree on which CLOSURE offset opened a slot without that being drift —
// an ordinary if/else (or match) where both arms capture the same outer,
// never-redeclared local opens it with two different offsets, one per arm,
// and both are live where the arms rejoin. Whichever arm ran, `vm.cpp`'s
// `captureUpvalue` reuses the slot's already-open cell, so the runtime holds
// ONE cell at the merge, never two. This pass unites the two origins into
// one live instance instead of throwing (see OriginUnionFind and joinInto in
// capture_analysis.cpp) — the reported range then carries both CLOSURE
// offsets in capturingClosureOffsets, exactly like two closures sharing one
// already-open cell in the same block would. This supersedes amendment 1's
// decision 1, item 5, which called this exact case an error.
CaptureAnalysis analyzeCaptures(const DecodedFunction& root);
