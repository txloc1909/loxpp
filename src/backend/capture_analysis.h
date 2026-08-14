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

#include "chunk_decoder.h"

#include <string>
#include <unordered_map>
#include <vector>

// One continuous live range of one captured local slot: the span during
// which one heap ref-cell backs that slot.
//
// `start` is approximate: the offset of the first CLOSURE instruction that
// captures this incarnation of the slot. The true declaration point is at or
// before `start` — recovering it exactly needs the abstract-stack pass (N2),
// which this node deliberately does not depend on. Every consumer of `start`
// must treat it as "captured no later than here," not "declared here."
struct CaptureLiveRange {
    int slot{-1};
    int start{-1};

    // The CLOSE_UPVALUE offset that ends this range. Meaningless when
    // `closedImplicitly` is set — see that field.
    int end{-1};

    // True when no CLOSE_UPVALUE closes this range in the chunk. The
    // compiler never emits one for a function's own top-level scope; the
    // native VM's frame-pop closes it instead (06_shared_upvalue's `outer`).
    // `end` is then the chunk's own length, not a real instruction offset.
    bool closedImplicitly{false};

    // True when a LOOP back-edge wraps [start, end]: this exact bytecode
    // span re-executes every iteration, so the declaration re-runs and needs
    // a fresh cell each time (V1_fresh_cell). False means one cell serves
    // the whole range, however many times it runs (V3_loopvar, and any
    // capture outside a loop).
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
    std::unordered_map<int, std::vector<CaptureLiveRange>> liveRangesBySlot;

    // How this function's OWN upvalue array is wired: entry i names either
    // a parent local (isLocal true) or a parent upvalue (isLocal false).
    // Copied from the CLOSURE instruction, in the PARENT chunk, that creates
    // this function. Empty for the root script, which no CLOSURE creates.
    std::vector<ClosureUpvalue> ownUpvalues;
};

// Capture info for a whole ObjFunction tree (see decodeFunctionTree),
// keyed by DecodedFunction::id.
struct CaptureAnalysis {
    std::unordered_map<std::string, FunctionCaptureInfo> functions;
};

// Walks every chunk in `root`'s tree and builds its capture map. Throws
// std::runtime_error if a CLOSE_UPVALUE has no open captured live range to
// close — the compiler never emits one otherwise, so this signals decoder or
// compiler drift, not a normal program.
CaptureAnalysis analyzeCaptures(const DecodedFunction& root);
