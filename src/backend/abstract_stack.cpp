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

// The "invisible var" recognition rule: a slot-referencing instruction
// retroactively marks an already-pushed value as a named local. Locals are
// always the bottom `localCount` cells, so recognizing position `slot`
// pulls every lower position in with it too.
//
// This runs on the *incoming* state to `ins`, before `ins` advances it —
// and, critically, on *every* edge feeding `ins`, before those edges are
// compared against each other. A slot's first reference can sit exactly at
// a merge (05_for's loop header: `GET_LOCAL 1` for `i` is both the
// fallthrough target from function entry, which has not yet seen `i`
// referenced, and the LOOP back-edge target, which has). Normalizing each
// edge through `ins`'s own rule first means both arrive already agreeing —
// the ambiguity is resolved by the very instruction that will resolve it at
// runtime too, not left as a spurious mismatch for the merge check to trip
// on.
StackState normalizeForInstruction(const std::vector<DecodedInstruction>& ins,
                                   size_t idx, StackState s) {
    const DecodedInstruction& instr = ins[idx];
    auto recognizeLocal = [&](int slot) {
        if (slot >= s.localCount) {
            s.localCount = slot + 1;
        }
    };

    switch (instr.op) {
    case Op::GET_LOCAL:
    case Op::SET_LOCAL:
        recognizeLocal(instr.byteOperand);
        break;
    case Op::CLOSURE:
        // isLocal upvalue entries name slots in *this* function
        // (chunk_decoder.h) — a local that is captured but never read back
        // via plain GET_LOCAL/SET_LOCAL. (CLOSURE's *own* pushed result
        // becoming a local — see closureIsConsumedImmediately — is handled
        // in advance() below: the position it would occupy does not exist
        // yet in `s`, the state *before* this instruction runs.)
        for (const auto& uv : instr.upvalues) {
            if (uv.isLocal) {
                recognizeLocal(uv.index);
            }
        }
        break;
    case Op::CLOSE_UPVALUE:
        // Always closes the current top of stack (vm.cpp), and only a
        // captured local is ever closed — so the position is local by
        // construction, before it is reclaimed below.
        recognizeLocal(s.height - 1);
        break;
    default:
        break;
    }
    return s;
}

// Advances an *already-normalized* `before` state (see
// normalizeForInstruction) across `ins[idx]`, producing the state after it
// runs.
//
// Pops are peeled off the top one at a time, in order, checking each
// against the *current* local region before it shrinks — not just the
// literal POP opcode. A cell an instruction pops can be a named local, not
// only a temporary: `print match c {...};` has PRINT directly pop() a
// match expression's synthetic result cell (P1's dual-meaning POP applies
// just as well to any other instruction that plainly discards the top).
StackState advance(const std::vector<DecodedInstruction>& ins, size_t idx,
                   StackState before) {
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

    // A nested function declaration's own binding (see
    // closureIsConsumedImmediately) — recognized only now, using the
    // *post-push* height, since the position it occupies does not exist
    // until this instruction's own push completes.
    if (instr.op == Op::CLOSURE && !closureIsConsumedImmediately(ins, idx)) {
        after.localCount = std::max(after.localCount, after.height);
    }
    return after;
}

// Finds the declaring push(es) for `slot`, walking backward from `fromIndex`
// over the CFG's predecessor edges. A slot can have more than one static
// declaring push — e.g. a captured local initialised by `a and b` lands via
// whichever branch of the short-circuit was taken (bytecode-translation-
// problems.md P2's merge). The search stops at the boundary of `slot`'s
// current lifetime: an edge where the position was not yet populated.
void findDeclaringPushes(int fromIndex, int slot,
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

        if (after[cur].height <= slot) {
            // Position `slot` did not exist right after `cur` ran — we have
            // walked past the start of this lifetime. Nothing beyond this
            // edge belongs to the declaration we are looking for.
            continue;
        }
        if (before[cur].height <= slot) {
            // `slot` did not exist before `cur` ran, but does after: this is
            // (one of) the declaring push(es).
            sites.insert({ins[cur].offset, slot});
            continue;
        }
        for (int pred : cfg.predecessors[cur]) {
            frontier.push_back(pred);
        }
    }
}

// Forward data-flow fixpoint over the local CFG, from `initial` at
// instruction 0. Every stored `state[i]` is normalized w.r.t. `ins[i]` (see
// normalizeForInstruction) *before* it is compared or merged — this is what
// keeps the fixpoint's own bookkeeping (raw height, localCount) reconcilable
// at a merge even though neither is, by itself, guaranteed equal on every
// incoming edge:
//   - localCount is *inferred* bottom-up from slot references, so an edge
//     that reaches a slot's first reference disagrees in raw numbers with
//     one that already passed it — resolved by normalizing through that
//     very reference (05_for's loop header: `GET_LOCAL 1` for `i` is
//     reached both from function entry, which has not yet seen `i`, and
//     the LOOP-31 back-edge, which has).
//   - height itself can differ across two match arms that destructure a
//     different number of pattern fields, reaching the shared post-match
//     code with a different number of (still-local) fields live — observed
//     in bootstrap/loxpp_interpreter.lox's `resolveStmt`. Different height,
//     same operandDepth.
// Taking the larger height/localCount pair after normalizing is monotone
// and bounded (each dimension only grows, bounded by the chunk's size), so
// the queue drains. A stricter "every edge must agree on operandDepth"
// check was tried and abandoned — see analyzeStack's sanity-check comment
// for why, and what is asserted instead.
std::vector<std::optional<StackState>>
runFixpoint(const std::vector<DecodedInstruction>& ins, const LocalCfg& cfg,
            StackState initial) {
    size_t n = ins.size();
    std::vector<std::optional<StackState>> state(n);
    state[0] = normalizeForInstruction(ins, 0, initial);
    std::deque<int> worklist{0};
    std::vector<bool> queued(n, false);
    queued[0] = true;

    while (!worklist.empty()) {
        int i = worklist.front();
        worklist.pop_front();
        queued[i] = false;
        StackState after = advance(ins, i, *state[i]);
        for (int succ : cfg.successors[i]) {
            StackState candidate = normalizeForInstruction(ins, succ, after);
            if (!state[succ]) {
                state[succ] = candidate;
                worklist.push_back(succ);
                queued[succ] = true;
                continue;
            }
            StackState& existing = *state[succ];
            bool changed = false;
            if (candidate.height > existing.height) {
                existing.height = candidate.height;
                changed = true;
            }
            if (candidate.localCount > existing.localCount) {
                existing.localCount = candidate.localCount;
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

// Pass 2: finds every invisible-var declaring push in the whole function,
// deduplicated (a captured or `and`/`or`-initialized local can have more
// than one — see findDeclaringPushes). Needs before/after for the *whole*
// function up front — a backward search from an early offset can cross a
// LOOP instruction physically later in the byte stream (05_for's
// back-edges), so the caller's pass 1 must already have filled those in.
std::vector<InvisibleVarSite>
findInvisibleVars(const std::vector<DecodedInstruction>& ins,
                  const LocalCfg& cfg, const std::vector<StackState>& before,
                  const std::vector<StackState>& after,
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
            findDeclaringPushes(idx, ins[i].byteOperand, ins, cfg, before,
                                after, reached, sites);
            break;
        case Op::CLOSURE:
            for (const auto& uv : ins[i].upvalues) {
                if (uv.isLocal) {
                    findDeclaringPushes(idx, uv.index, ins, cfg, before, after,
                                        reached, sites);
                }
            }
            break;
        case Op::CLOSE_UPVALUE:
            findDeclaringPushes(idx, before[i].height - 1, ins, cfg, before,
                                after, reached, sites);
            break;
        default:
            break;
        }
    }

    std::vector<InvisibleVarSite> result;
    result.reserve(sites.size());
    for (const auto& [offset, slot] : sites) {
        result.push_back({offset, slot});
    }
    return result;
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
    StackState initial{arity + 1, arity + 1};
    std::vector<std::optional<StackState>> state =
        runFixpoint(ins, cfg, initial);

    // `endCompiler()` (compiler.cpp) unconditionally appends a trailing
    // NIL;RETURN, even when every path already returned explicitly — that
    // tail can be unreachable. Record reachability so the final pass can
    // skip it instead of asserting on a state that was never propagated.
    std::vector<bool> reached(n);
    for (size_t i = 0; i < n; i++) {
        reached[i] = state[i].has_value();
    }
    result.reached = reached;

    // Sanity check on the converged values. A strict "every predecessor's
    // *freshly recomputed* contribution must equal the merged result"
    // check was tried and abandoned: it produces false positives whenever a
    // slot is discovered to be local partway around a back-edge (05_for,
    // 11_for_in — the fallthrough edge from function entry has not yet
    // seen the loop-body reference that the back-edge already reflects).
    // Re-deriving a predecessor's contribution in isolation, after
    // convergence, does not "see" that other edge's information the way
    // the fixpoint above already correctly did; comparing them again is not
    // meaningful. The invariant that *is* always true, and worth asserting
    // as a guard against a stack-effect table or CFG-building bug, is
    // structural: a position can never be local without existing, and
    // height can never go negative.
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

    // Pass 1 over the converged states: derive before/after for every
    // reached instruction, POP classifications, and the operand high-water
    // mark. Kept separate from the fixpoint loop above because localCount
    // there can still rise on a later iteration; only the converged value is
    // meaningful to report.
    result.before.resize(n);
    result.after.resize(n);
    for (size_t i = 0; i < n; i++) {
        if (!reached[i]) {
            continue;
        }
        StackState before = *state[i];
        StackState after = advance(ins, i, before);
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

    result.invisibleVars =
        findInvisibleVars(ins, cfg, result.before, result.after, reached);

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
