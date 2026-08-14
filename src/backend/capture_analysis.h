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
// Scope note: this pass is a single linear walk over one chunk's decoded
// instructions, in program order. It carries no control-flow graph (N1) and
// no stack simulation (N2) by design, so every offset it reports is a bound
// on a slot's capture, not a per-execution-path fact. See
// FunctionCaptureInfo::firstCaptureOffset below for what that means for N7.

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
    // Concretely, `firstCaptureOffset` is NOT guaranteed to dominate `end`:
    // a jump can land inside (firstCaptureOffset, end] without ever having
    // run firstCaptureOffset. V3_loopvar.lox is the standing counter-example
    // — a zero-trip loop reaches this range's CLOSE_UPVALUE without ever
    // running the CLOSURE that opened it, because the loop variable's real
    // declaration (the init clause) runs once, before the loop, while the
    // capturing CLOSURE sits inside the conditionally-skipped body. Recovering
    // the exact declaration offset needs either the CFG (N1) or the abstract
    // stack (N2); this node deliberately depends on neither, so it reports
    // the bound it can prove from a single linear pass and no more.
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

    // The CLOSE_UPVALUE offset that ends this range. Meaningless when
    // `closedImplicitly` is set — see that field. Like
    // `firstCaptureOffset`, this is the offset this pass can prove from a
    // single linear pass; it is not guaranteed to be the only point some
    // execution path could close the range (RETURN also closes every open
    // upvalue in the frame — see `closedImplicitly`), but every such RETURN
    // is frame-terminal, so no code after it in this chunk can observe the
    // difference.
    int end{-1};

    // True when no CLOSE_UPVALUE closes this range in the chunk's linear
    // instruction stream. Exactly one legitimate cause produces this, once
    // the compiler emits CLOSE_UPVALUE for every captured local it reclaims
    // (see Compiler::endScope and Compiler::emitLoopCleanup): the range is
    // scoped to the function's own top-level scope, which the compiler never
    // wraps in an explicit close — the native VM's RETURN closes it instead,
    // via closeUpvalues(frame->slots) (06_shared_upvalue's `outer`). `end` is
    // then the chunk's own length, not a real instruction offset.
    //
    // A captured local in a NESTED block scope always gets an explicit
    // CLOSE_UPVALUE somewhere in the chunk (at that block's own endScope, or
    // at emitLoopCleanup on every early-exit path out of it), so it is never
    // closedImplicitly — even when some OTHER path out of that same block
    // returns early. An early RETURN needs no explicit close of its own
    // because it is frame-terminal: nothing in this chunk runs afterward, so
    // there is nothing left that could read a stale cell.
    bool closedImplicitly{false};

    // True when a LOOP back-edge wraps [firstCaptureOffset, end]: this exact
    // bytecode span re-executes every iteration, so the declaration re-runs
    // and needs a fresh cell each time (V1_fresh_cell). False means one cell
    // serves the whole range, however many times it runs (V3_loopvar, and
    // any capture outside a loop).
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
};

// Capture info for a whole ObjFunction tree (see decodeFunctionTree),
// keyed by DecodedFunction::id. std::map for the same determinism reason as
// FunctionCaptureInfo::liveRangesBySlot.
struct CaptureAnalysis {
    std::map<std::string, FunctionCaptureInfo> functions;
};

// Walks every chunk in `root`'s tree and builds its capture map. Throws
// std::runtime_error if a CLOSE_UPVALUE names no open captured slot AND no
// slot has closed yet at all in the chunk — every other CLOSE_UPVALUE has an
// open slot to close, or is tolerated as an alternate exit of the
// most-recently-closed one (see recordClose in the .cpp; a normal program
// can legitimately have more than one CLOSE_UPVALUE for the same capture, on
// break/continue/match-arm-exit paths). A throw here signals decoder or
// compiler drift, not a normal program.
CaptureAnalysis analyzeCaptures(const DecodedFunction& root);
