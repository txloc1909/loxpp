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
// genuinely different, later variable that reuses the same slot number. The
// CFG resolves this: two CLOSURE instructions for the same slot share one
// live range exactly when some CFG path connects them without crossing a
// CLOSE_UPVALUE for that slot; a slot is open at a block's entry exactly when
// every predecessor that reaches it agrees it is still open. This pass still
// carries no stack simulation (N2), so every offset it reports is a bound on
// a slot's capture, not a per-execution-path fact. See
// FunctionCaptureInfo::firstCaptureOffset below for what that means for N7.

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
    int firstCaptureOffset{-1};

    // A real CLOSE_UPVALUE offset that ends this range. Meaningless (left at
    // -1) only when `allCloseOffsets` is empty AND `closedImplicitly` is
    // false, which never happens for a committed range — see that field.
    // `end` is `allCloseOffsets.back()`, or the chunk's length when
    // `allCloseOffsets` is empty and the range is `closedImplicitly`.
    int end{-1};

    // Every real CLOSE_UPVALUE offset that resolves to this exact range, one
    // per path that closes it. A range genuinely gets more than one when its
    // scope has more than one mutually exclusive exit — break, continue, a
    // match arm's own exit, and the normal fall-through can each emit their
    // OWN CLOSE_UPVALUE for the same captured local, on their own path — and
    // exactly one of them fires per real execution, never more than one and
    // never zero (unless the range is closedImplicitly, in which case this
    // is empty: see that field). `end` is always `allCloseOffsets.back()`.
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
    // (06_shared_upvalue's `outer`) — there, `end` is the chunk's own
    // length, not a real instruction offset, because there is no explicit
    // close to report instead.
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
    // cell (06_shared_upvalue, V2_shared) — do not allocate one per closure.
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
// capture map per execution path (round-3 referee decision, PR #101): a
// dataflow over the CFG, not a flat, order-only walk over the instruction
// list. A normal program can legitimately have more than one real
// CLOSE_UPVALUE for the same capture, on mutually exclusive
// break/continue/match-arm-exit/fall-through paths; a flat walk cannot tell
// that apart from a real scope exit followed by a genuinely different, later
// variable that reuses the same slot number, because CLOSE_UPVALUE carries
// no operand. The CFG resolves it: a slot is open at a block's entry exactly
// when every predecessor that reaches it agrees it is still open (two
// disagreeing predecessors is a real error, not an alternate exit), and
// CLOSE_UPVALUE always closes the highest open captured slot on whichever
// single path is being walked. Throws std::runtime_error if a CLOSE_UPVALUE
// names no open captured slot on a REACHABLE block's fully-converged path,
// or if two CFG paths reach one block disagreeing on which capture holds a
// slot open. Either signals decoder or compiler drift, not a normal program.
CaptureAnalysis analyzeCaptures(const DecodedFunction& root);
