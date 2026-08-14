#include "abstract_stack.h"

#include "exec_objects.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// Provisional local CFG.
//
// N1 (CFG/label recovery) has not merged yet. Per N2.md's hazards, this node
// computes its own leaders/edges rather than block on N1; a later node
// unifies the two builders. This is intentionally minimal: only what the
// stack walk needs (a successor list per instruction index), not a reusable
// basic-block abstraction.
// ---------------------------------------------------------------------------

struct LocalCfg {
    std::vector<std::vector<int>> successors;
    std::vector<std::vector<int>> predecessors;
};

LocalCfg buildCfg(const std::vector<DecodedInstruction>& ins) {
    std::unordered_map<int, int> offsetToIndex;
    offsetToIndex.reserve(ins.size());
    for (size_t i = 0; i < ins.size(); i++) {
        offsetToIndex[ins[i].offset] = static_cast<int>(i);
    }

    LocalCfg cfg;
    cfg.successors.resize(ins.size());
    cfg.predecessors.resize(ins.size());
    auto addEdge = [&](int from, int to) {
        cfg.successors[from].push_back(to);
        cfg.predecessors[to].push_back(from);
    };

    for (size_t i = 0; i < ins.size(); i++) {
        int idx = static_cast<int>(i);
        int fallthrough = (i + 1 < ins.size()) ? idx + 1 : -1;
        switch (ins[i].op) {
        // Terminators: the frame ends (RETURN) or the VM always raises
        // (MATCH_ERROR) — control never falls through to the next offset.
        case Op::RETURN:
        case Op::MATCH_ERROR:
            break;
        case Op::JUMP:
        case Op::LOOP:
            addEdge(idx, offsetToIndex.at(ins[i].jumpTarget));
            break;
        case Op::JUMP_IF_FALSE:
            // Peeks (vm.cpp); both edges carry the same abstract stack.
            addEdge(idx, offsetToIndex.at(ins[i].jumpTarget));
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        case Op::JUMP_TABLE:
            for (const auto& arm : ins[i].jumpTable) {
                addEdge(idx, offsetToIndex.at(arm.target));
            }
            // Out-of-range tag: falls through to whatever follows the table
            // (MATCH_ERROR, per P8).
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        default:
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Per-opcode stack effect.
// ---------------------------------------------------------------------------

// How many cells an instruction removes from the *current top*, in order,
// before it pushes anything back — not just the net height delta. This
// matters because a cell popped this way can be a named local, not only a
// temporary: e.g. `print match c {...};` has PRINT directly pop() a
// match expression's synthetic result cell (enum_match.lox), the same way
// a plain POP can (P1). Tracking pop *count* (not just net delta) lets
// advance() check every removed cell, in order, for that.
//
// Every entry here is grounded in a specific vm.cpp handler, not the
// notes/jvm-backend-plan.md table (that table has known errors — see
// bytecode-translation-problems.md). Two shapes recur:
//   - "shuffle": pop N, push 1 *new* cell (SET_PROPERTY, SET_INDEX, CALL,
//     BUILD_LIST/MAP, ...) — assignment-as-expression (P2) or an
//     aggregate result. The pushed cell is a temporary; nothing here
//     revives one of the popped cells' identity as local.
//   - peek family (P2): pop 0 — the tested/assigned value is left in
//     place, not pushed again (SET_LOCAL, SET_GLOBAL, SET_UPVALUE,
//     JUMP_IF_FALSE).
struct StackEffect {
    int popCount;
    int pushCount;
};

StackEffect stackEffect(const DecodedInstruction& ins) {
    switch (ins.op) {
    // Pure pushes.
    case Op::CONSTANT:
    case Op::NIL:
    case Op::TRUE:
    case Op::FALSE:
    case Op::GET_GLOBAL:
    case Op::CLASS:
    case Op::CLOSURE:
    case Op::GET_UPVALUE:
    case Op::GET_LOCAL:
        return {0, 1};

    // Pop 1, push 1: value replaced in place.
    case Op::NEGATE:
    case Op::NOT:
    case Op::GET_TAG:
    case Op::IS_SEQ:
    case Op::INSTANCEOF:
    case Op::GET_PROPERTY:
    case Op::ITER_HAS_NEXT: // pop iterator copy, push bool
    case Op::ITER_NEXT:     // pop iterator copy, push element
        return {1, 1};

    // Peek family (P2): pop 0 — the assigned/tested value is left in place.
    case Op::SET_LOCAL:
    case Op::SET_GLOBAL:
    case Op::SET_UPVALUE:
    case Op::JUMP_IF_FALSE:
        return {0, 0};

    // No stack effect at all.
    case Op::JUMP:
    case Op::LOOP:
    case Op::MATCH_ERROR:
    case Op::GET_ITER: // in-place replace (vm.cpp: stackTop[-1] = ...)
        return {0, 0};

    // Pop 2, push 1.
    case Op::EQUAL:
    case Op::GREATER:
    case Op::LESS:
    case Op::ADD:
    case Op::SUBTRACT:
    case Op::MULTIPLY:
    case Op::DIVIDE:
    case Op::MODULO:
    case Op::GET_INDEX:
    case Op::IN:
    case Op::SET_PROPERTY: // pop val,instance; push val (P2 shuffle)
    case Op::GET_SUPER:    // pop superclass,this; push bound method
        return {2, 1};

    // Pure pops.
    case Op::PRINT:
    case Op::POP:
    case Op::DEFINE_GLOBAL: // peek then pop (vm.cpp)
    case Op::CLOSE_UPVALUE:
    case Op::RETURN:
        return {1, 0};

    // Pop the tag, no push (JUMP_TABLE's arms are jumps, not values).
    case Op::JUMP_TABLE:
        return {1, 0};

    case Op::DEFINE_METHOD: // pop fn only; cls (beneath) is never popped
    case Op::INHERIT:       // pop subclass only; superclass is never popped
        return {1, 0};

    // Pop 3, push 1 (shuffle).
    case Op::SET_INDEX:
    case Op::SLICE:
        return {3, 1};

    // Pop argCount+1 (callee/receiver + args), push 1 result — true across
    // every callee kind vm.cpp dispatches CALL/INVOKE to (P6): closure,
    // native, bound method, class (+ init), enum ctor.
    case Op::CALL:
    case Op::INVOKE:
        return {ins.byteOperand + 1, 1};

    // SUPER_INVOKE additionally pops the superclass GET_SUPER left on top.
    case Op::SUPER_INVOKE:
        return {ins.byteOperand + 2, 1};

    case Op::BUILD_LIST:
        return {ins.byteOperand, 1};
    case Op::BUILD_MAP:
        return {2 * ins.byteOperand, 1};
    }
    throw std::runtime_error("abstract_stack: no stack effect for opcode " +
                             std::to_string(static_cast<int>(ins.op)) +
                             " at offset " + std::to_string(ins.offset));
}

bool topIsLocal(const StackState& s) { return s.height - 1 < s.localCount; }

// True when a CLOSURE's pushed value is consumed immediately rather than
// bound to a local: `Compiler::method` always follows CLOSURE with
// DEFINE_METHOD, and `Compiler::funDeclaration` at scope depth 0 (a
// top-level `fun`) follows it with DEFINE_GLOBAL (compiler.cpp). Any other
// CLOSURE is a nested `fun name() {...}` declaration — declareVariable() +
// markInitialized() ran before it compiled, so its result is left in place
// as an invisible var (P1) exactly like `var name = ...;`, whether or not
// anything ever reads it back with GET_LOCAL (06_shared_upvalue's `set` is
// captured by nothing and never called, but is still a real local slot).
bool closureIsConsumedImmediately(const std::vector<DecodedInstruction>& ins,
                                  size_t closureIndex) {
    size_t next = closureIndex + 1;
    if (next >= ins.size()) {
        return false;
    }
    return ins[next].op == Op::DEFINE_GLOBAL ||
           ins[next].op == Op::DEFINE_METHOD;
}

// Advances `before` across `ins[idx]`, producing the state after it runs.
//
// Pops are peeled off the top one at a time, in order, checking each
// against the *current* local region before it shrinks — not just the
// literal POP opcode. A cell an instruction pops can be a named local, not
// only a temporary: `print match c {...};` has PRINT directly pop() a
// match expression's synthetic result cell (P1's dual-meaning POP applies
// just as well to any other instruction that plainly discards the top).
//
// `declaredSlotsAt`, when given, is a per-instruction-index list of slots
// whose declaring push this instruction *is* (see findInvisibleVarIndices):
// recognition happens here, on this instruction's own outgoing state, not
// retroactively on some later referencing instruction's incoming state. A
// local becomes visible to the analysis at the exact offset the emitter
// must insert its store, closing the gap R1 found between `invisibleVars`
// and the reported per-offset stack states. Null during the first,
// height-only pass (see runFixpoint's two call sites in analyzeStack):
// nothing is recognized as local yet, because the declaring-push sites
// haven't been found (that search needs this same height data first).
StackState advance(const std::vector<DecodedInstruction>& ins, size_t idx,
                   StackState before,
                   const std::vector<std::vector<int>>* declaredSlotsAt) {
    const DecodedInstruction& instr = ins[idx];
    StackEffect effect = stackEffect(instr);

    StackState after = before;
    for (int i = 0; i < effect.popCount; i++) {
        if (topIsLocal(after)) {
            after.localCount -= 1; // reclaim: free the slot for a sibling scope
        }
        after.height -= 1;
    }
    after.height += effect.pushCount;

    if (declaredSlotsAt != nullptr) {
        for (int slot : (*declaredSlotsAt)[idx]) {
            after.localCount = std::max(after.localCount, slot + 1);
        }
    }
    return after;
}

// Finds the declaring push(es) for `slot`'s *current value*, walking
// backward from `fromIndex` over the CFG's predecessor edges.
//
// This follows the value, not the stack position (R7): an instruction whose
// own pop reaches down to (or past) `slot` destroys whatever was there
// before it ran. If its own push then lands back at `slot`, that is a brand
// new value occupying the same numeric position — the declaring push — and
// the search must not walk past it looking for the *earlier* value's origin
// (11_for_in.lox: BUILD_LIST pops three cells and pushes the list back at
// position 1; the list is a new value, not the first CONSTANT that once
// lived there). An instruction whose pop does not reach `slot` leaves that
// position's value untouched, so the search continues further back through
// it.
//
// A slot can have more than one static declaring push — e.g. a captured
// local initialised by `a and b` lands via whichever branch of the
// short-circuit was taken (bytecode-translation-problems.md P2's merge) — so
// this explores every predecessor, not just one.
void findDeclaringPushIndices(int fromIndex, int slot,
                              const std::vector<DecodedInstruction>& ins,
                              const LocalCfg& cfg,
                              const std::vector<StackState>& before,
                              const std::vector<StackState>& after,
                              const std::vector<bool>& reached,
                              std::set<std::pair<int, int>>& sites) {
    std::vector<bool> visited(ins.size(), false);
    std::deque<int> frontier(cfg.predecessors[fromIndex].begin(),
                             cfg.predecessors[fromIndex].end());
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop_front();
        if (visited[cur] || !reached[cur]) {
            continue;
        }
        visited[cur] = true;

        StackEffect effect = stackEffect(ins[cur]);
        // Height right after `cur`'s pops, before its own push — equally
        // `after[cur].height - effect.pushCount`, but computed from `before`
        // so it needs no assumption about how `after` was derived.
        int popReach = before[cur].height - effect.popCount;

        if (popReach <= slot) {
            // `cur`'s pop reached at least this deep: the value that sat at
            // `slot` before `cur` ran is gone.
            if (slot < after[cur].height) {
                // ...and `cur`'s own push lands back at `slot` — a new
                // value born right here. Found it.
                sites.insert({cur, slot});
            }
            // Either way, stop: anything further back belongs to a value
            // `cur` already destroyed, not to the one live at `fromIndex`.
            continue;
        }
        // `slot` sat beneath everything `cur` touched — its value passed
        // through `cur` unchanged. Keep searching further back for its
        // origin.
        for (int pred : cfg.predecessors[cur]) {
            frontier.push_back(pred);
        }
    }
}

// Finds every invisible-var declaring push in the whole function,
// deduplicated, keyed by instruction index (converted to source offset by
// the caller). Needs before/after height for the *whole* function up front
// — a backward search from an early offset can cross a LOOP instruction
// physically later in the byte stream (05_for's back-edges) — which is why
// this runs against a completed height-only fixpoint (see analyzeStack),
// not interleaved with it.
std::set<std::pair<int, int>> findInvisibleVarIndices(
    const std::vector<DecodedInstruction>& ins, const LocalCfg& cfg,
    const std::vector<StackState>& before, const std::vector<StackState>& after,
    const std::vector<bool>& reached) {
    std::set<std::pair<int, int>> sites;
    for (size_t i = 0; i < ins.size(); i++) {
        if (!reached[i]) {
            continue;
        }
        int idx = static_cast<int>(i);
        switch (ins[i].op) {
        case Op::GET_LOCAL:
        case Op::SET_LOCAL:
            findDeclaringPushIndices(idx, ins[i].byteOperand, ins, cfg, before,
                                     after, reached, sites);
            break;
        case Op::CLOSURE:
            // isLocal upvalue entries name slots in *this* function
            // (chunk_decoder.h) — a local that is captured but never read
            // back via plain GET_LOCAL/SET_LOCAL.
            for (const auto& uv : ins[i].upvalues) {
                if (uv.isLocal) {
                    findDeclaringPushIndices(idx, uv.index, ins, cfg, before,
                                             after, reached, sites);
                }
            }
            // The CLOSURE's *own* pushed value can itself become a local
            // (a nested `fun name() {...}` declaration) — its declaring
            // push is this very instruction, at the position its own push
            // lands (06_shared_upvalue's `set`: never captured, never read
            // back, still a real local slot — findDeclaringPushIndices
            // above would never find it, since nothing ever names its
            // slot).
            if (!closureIsConsumedImmediately(ins, i)) {
                sites.insert({idx, after[i].height - 1});
            }
            break;
        case Op::CLOSE_UPVALUE:
            // Always closes the current top of stack (vm.cpp), and only a
            // captured local is ever closed.
            findDeclaringPushIndices(idx, before[i].height - 1, ins, cfg,
                                     before, after, reached, sites);
            break;
        default:
            break;
        }
    }
    return sites;
}

// Forward data-flow fixpoint over the local CFG, from `initial` at
// instruction 0. Run twice by analyzeStack (see there):
//
//   Pass 1 — `declaredSlotsAt == nullptr`, `initial.localCount == 0`. Only
//   height is meaningful; its purpose is solely to give
//   findInvisibleVarIndices the before/after heights it needs to locate
//   every declaring push. No recognition happens yet, so there is nothing
//   for a recognition-timing bug to lag (R1's failure mode does not exist
//   in this pass).
//
//   Pass 2 — `declaredSlotsAt` is pass 1's result. `advance()` now
//   recognizes each local exactly at its declaring push (not at first
//   reference), so `before`/`after` here are what analyzeStack reports.
//
// The merge join takes `max` of height and of localCount independently.
// This remains necessary (not merely tolerated) even after pass 2's fix:
// two arms of a `match` that destructure a different number of pattern
// fields legitimately reach shared post-match code at different raw
// heights and localCounts, with the *same* operand depth, because the
// extra fields are still local on the longer arm (observed in
// bootstrap/loxpp_interpreter.lox's `resolveStmt`). Taking the max of each
// dimension independently is sound here because, in every such case, the
// same edge supplies both maxima (the longer arm has strictly more of
// everything, never a trade-off between the two) — the join only becomes
// unsound (R4) when recognition timing itself, not genuine structure,
// causes two edges to split one edge's cells between "local" and "temp"
// differently at the *same* height. That split can't happen once
// recognition is declaring-push-timed, and analyzeStack checks it isn't:
// see validateMergeConsistency, which is the real assertion for checkpoint
// 5 (R3) — a post-convergence check, not this join, because a join that
// throws on every *transient* mid-fixpoint disagreement (before a
// loop's back-edge has propagated) would reject legitimate programs.
std::vector<std::optional<StackState>>
runFixpoint(const std::vector<DecodedInstruction>& ins, const LocalCfg& cfg,
            StackState initial,
            const std::vector<std::vector<int>>* declaredSlotsAt) {
    size_t n = ins.size();
    std::vector<std::optional<StackState>> state(n);
    state[0] = initial;
    std::deque<int> worklist{0};
    std::vector<bool> queued(n, false);
    queued[0] = true;

    while (!worklist.empty()) {
        int i = worklist.front();
        worklist.pop_front();
        queued[i] = false;
        StackState after = advance(ins, i, *state[i], declaredSlotsAt);
        for (int succ : cfg.successors[i]) {
            if (!state[succ]) {
                state[succ] = after;
                worklist.push_back(succ);
                queued[succ] = true;
                continue;
            }
            StackState& existing = *state[succ];
            bool changed = false;
            if (after.height > existing.height) {
                existing.height = after.height;
                changed = true;
            }
            if (after.localCount > existing.localCount) {
                existing.localCount = after.localCount;
                changed = true;
            }
            if (changed && !queued[succ]) {
                worklist.push_back(succ);
                queued[succ] = true;
            }
        }
    }
    return state;
}

// Checkpoint 5, asserted for real (R3/R4): every reached instruction with
// two or more reached predecessors must see the *same* operand depth on
// every incoming edge — the invariant the JVM/CLR verifier enforces at
// every control-flow merge. Runs once, after pass 2 has fully converged,
// using each predecessor's own final `after` state directly: recognition
// is declaring-push-timed now (R1), not reference-timed, so a
// predecessor's `after` state already reflects every declaration on its
// own path with no further reconciliation needed at the point of use.
//
// This must run post-convergence, not inside runFixpoint's join: mid-
// fixpoint, a loop's back-edge can (temporarily) disagree with its
// fallthrough edge before the worklist has propagated the back-edge's
// contribution all the way around, and throwing on that transient state
// would reject legitimate programs. A disagreement that survives to
// convergence is a real one.
void validateMergeConsistency(const std::vector<DecodedInstruction>& ins,
                              const LocalCfg& cfg,
                              const std::vector<StackState>& after,
                              const std::vector<bool>& reached,
                              const std::string& functionId) {
    for (size_t i = 0; i < ins.size(); i++) {
        if (!reached[i]) {
            continue;
        }
        std::optional<int> depth;
        for (int pred : cfg.predecessors[i]) {
            if (!reached[pred]) {
                continue;
            }
            int d = after[pred].operandDepth();
            if (!depth) {
                depth = d;
                continue;
            }
            if (*depth != d) {
                throw std::runtime_error(
                    "abstract_stack: merge disagreement in function '" +
                    functionId + "' at offset " +
                    std::to_string(ins[i].offset) +
                    ": incoming operand depths disagree (" +
                    std::to_string(*depth) + " vs " + std::to_string(d) +
                    ") — the JVM/CLR verifier would reject this merge");
            }
        }
    }
}

} // namespace

FunctionStackAnalysis analyzeStack(const DecodedFunction& fn) {
    const auto& ins = fn.instructions;
    FunctionStackAnalysis result;
    result.functionId = fn.id;
    size_t n = ins.size();
    if (n == 0) {
        return result;
    }

    LocalCfg cfg = buildCfg(ins);

    // Slot 0 is always the callee/receiver, slots 1..arity the parameters
    // (bytecode-translation-problems.md P5) — true for the script chunk too
    // (vm.cpp: VM::interpret calls it with argCount=0, just like any other
    // closure).
    int arity = fn.function->arity;
    int initialHeight = arity + 1;

    // Pass 1: height-and-reachability only (see runFixpoint's comment).
    std::vector<std::optional<StackState>> heightState =
        runFixpoint(ins, cfg, StackState{initialHeight, 0}, nullptr);

    // `endCompiler()` (compiler.cpp) unconditionally appends a trailing
    // NIL;RETURN, even when every path already returned explicitly — that
    // tail can be unreachable. Record reachability so later passes can skip
    // it instead of asserting on a state that was never propagated.
    std::vector<bool> reached(n);
    for (size_t i = 0; i < n; i++) {
        reached[i] = heightState[i].has_value();
    }
    result.reached = reached;

    std::vector<StackState> heightBefore(n);
    std::vector<StackState> heightAfter(n);
    for (size_t i = 0; i < n; i++) {
        if (!reached[i]) {
            continue;
        }
        heightBefore[i] = *heightState[i];
        heightAfter[i] = advance(ins, i, heightBefore[i], nullptr);
    }

    // Locate every declaring push using pass 1's heights (R7-fixed search),
    // then index them by instruction so pass 2 can recognize each local
    // exactly there instead of at its first reference (R1).
    std::set<std::pair<int, int>> siteIndices =
        findInvisibleVarIndices(ins, cfg, heightBefore, heightAfter, reached);
    std::vector<std::vector<int>> declaredSlotsAt(n);
    for (const auto& [idx, slot] : siteIndices) {
        declaredSlotsAt[idx].push_back(slot);
    }

    // Pass 2: full recognition-aware fixpoint.
    StackState initial{initialHeight, initialHeight};
    std::vector<std::optional<StackState>> state =
        runFixpoint(ins, cfg, initial, &declaredSlotsAt);

    // Structural sanity check on the converged values: a position can never
    // be local without existing, and height can never go negative. Guards
    // against a stack-effect table or CFG-building bug; not itself
    // checkpoint 5 (see validateMergeConsistency below for that).
    for (size_t i = 0; i < n; i++) {
        if (!reached[i]) {
            continue;
        }
        const StackState& s = *state[i];
        if (s.height < 0 || s.localCount < 0 || s.localCount > s.height) {
            throw std::runtime_error(
                "abstract_stack: impossible stack state in function '" + fn.id +
                "' at offset " + std::to_string(ins[i].offset) +
                ": height=" + std::to_string(s.height) +
                " localCount=" + std::to_string(s.localCount));
        }
    }

    // Derive before/after for every reached instruction, POP
    // classifications, and the operand high-water mark from pass 2's
    // converged, recognition-complete states.
    result.before.resize(n);
    result.after.resize(n);
    for (size_t i = 0; i < n; i++) {
        if (!reached[i]) {
            continue;
        }
        StackState before = *state[i];
        StackState after = advance(ins, i, before, &declaredSlotsAt);
        result.before[i] = before;
        result.after[i] = after;

        int depth = std::max(before.operandDepth(), after.operandDepth());
        result.maxOperandDepth = std::max(result.maxOperandDepth, depth);

        if (ins[i].op == Op::POP) {
            PopKind kind =
                topIsLocal(before) ? PopKind::LOCAL_RECLAIM : PopKind::TEMP;
            result.pops.push_back({ins[i].offset, kind});
        }
    }

    validateMergeConsistency(ins, cfg, result.after, reached, fn.id);

    result.invisibleVars.reserve(siteIndices.size());
    for (const auto& [idx, slot] : siteIndices) {
        result.invisibleVars.push_back({ins[idx].offset, slot});
    }

    return result;
}

StackAnalysisTree analyzeStackTree(const DecodedFunction& root) {
    StackAnalysisTree node;
    node.self = analyzeStack(root);
    node.nested.reserve(root.nested.size());
    for (const auto& child : root.nested) {
        node.nested.push_back(analyzeStackTree(child));
    }
    return node;
}

bool peeksInsteadOfPops(Op op) {
    switch (op) {
    case Op::SET_LOCAL:
    case Op::SET_GLOBAL:
    case Op::SET_UPVALUE:
    case Op::SET_PROPERTY:
    case Op::SET_INDEX:
    case Op::JUMP_IF_FALSE:
        return true;
    default:
        return false;
    }
}
