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
// src/backend/cfg.{h,cpp} builds a general CFG/label-recovery pass. This
// file still keeps its own private leaders/edges builder — see
// abstract_stack.h's note on unifying the two. This is intentionally
// minimal: only what the stack walk needs
// (a successor list per instruction index), not a reusable basic-block
// abstraction. The two builders agree on every edge rule today (verified by
// hand against src/backend/cfg.cpp: JUMP/LOOP take one edge, JUMP_IF_FALSE/
// JUMP_TABLE take the branch edges plus fallthrough, RETURN/MATCH_ERROR take
// none) — a later merge of the two builders is a deduplication, not a
// semantic reconciliation.
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

// `localCount` after popping `popCount` cells off `s`, following the same
// reclaim rule `advance()` applies one cell at a time (a popped cell frees
// its slot only while it is still within the local region). Shared by
// `advance()` and `validateNoInvisibleVarGaps` so the two can never drift
// apart on what "the local count right before a recognition point" means.
int localCountAfterPops(StackState s, int popCount) {
    for (int i = 0; i < popCount; i++) {
        if (topIsLocal(s)) {
            s.localCount -= 1;
        }
        s.height -= 1;
    }
    return s.localCount;
}

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
    after.localCount = localCountAfterPops(after, effect.popCount);
    after.height -= effect.popCount;
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
//
// Returns whether it found at least one declaring push, independent of
// whether `sites` already held it — findInvisibleVarIndices's R8 backfill
// (below) needs that to tell "this slot has no push instruction at all here
// (an initial parameter — stop descending)" apart from "this slot's push was
// already known from elsewhere", which look identical if judged only by
// whether `sites` grew.
bool findDeclaringPushIndices(int fromIndex, int slot,
                              const std::vector<DecodedInstruction>& ins,
                              const LocalCfg& cfg,
                              const std::vector<StackState>& before,
                              const std::vector<StackState>& after,
                              const std::vector<bool>& reached,
                              std::set<std::pair<int, int>>& sites) {
    bool found = false;
    std::vector<bool> visited(ins.size(), false);
    std::deque<int> frontier(cfg.predecessors[fromIndex].begin(),
                             cfg.predecessors[fromIndex].end());
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop_front();
        if (visited[cur] || !static_cast<bool>(reached[cur])) {
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
                found = true;
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
    return found;
}

// Chases declaring pushes for every slot from `topSlot` down to 0, using
// `originIdx` as the search origin, and stops at the first slot with no
// reachable declaring push — the parameter boundary (frame entry pushed no
// instruction there), or a prefix some earlier call already fully covered.
// Sound, not heuristic: clox's scoping is strictly LIFO (StackState's own
// comment in abstract_stack.h), so once one slot is independently confirmed
// local — a GET_LOCAL/SET_LOCAL/CLOSE_UPVALUE/CLOSURE-capture names it, a
// CLOSURE's own push declares it, or RETURN's operandDepth()==1 invariant
// proves it (backfillFromFrameTeardown) — every lower slot must be local
// too, whether or not anything ever names *those*. This is not the "run
// length implies local count" rule the referee rejected: that rule had no
// independently-confirmed anchor at all, only POP adjacency; every call
// here starts from one.
void chaseSlotsDownward(int originIdx, int topSlot,
                        const std::vector<DecodedInstruction>& ins,
                        const LocalCfg& cfg,
                        const std::vector<StackState>& before,
                        const std::vector<StackState>& after,
                        const std::vector<bool>& reached,
                        std::set<std::pair<int, int>>& sites) {
    for (int k = topSlot; k >= 0; k--) {
        if (!findDeclaringPushIndices(originIdx, k, ins, cfg, before, after,
                                      reached, sites)) {
            return;
        }
    }
}

// R8, the binding design rule (replaces an earlier reference-driven backfill
// a counter-example defeated): whether a POP discards a named local or a
// compiler temporary cannot be read off the POP itself. `{ var a = 1; }`
// and `1;` compile to byte-identical chunks with opposite source truth
// (see notes/jvm-emission-contract.md), so no reference-driven rule can
// recover it; this analysis needs a canonical rule instead. The rule is a
// *persistence test*: walk
// backward from the POP for the cell it discards, looking for a **cover
// witness** — some instruction that later pushed a new value directly on
// top of that cell while it sat untouched at the exposed top of stack
// (`popCount == 0 && pushCount >= 1 && before.height == slot + 1`). A
// compiler temporary never survives that: every temp-discard idiom in
// compiler.cpp consumes its value at the moment its own expression ends, so
// only a value the compiler meant to keep around — a named local — is ever
// still sitting there when something else gets pushed on top of it. Where no
// cover witness exists (the undecidable corner, e.g. `1;`), the canonical
// answer is TEMP.
//
// One exception: a traversed DEFINE_METHOD whose own peek (the class value
// beneath the method closure it pops) reaches the cell disqualifies it
// regardless of any cover witness found elsewhere in the walk. That cell is
// the class-declaration accumulator (P2: DEFINE_METHOD leaves the class
// value on the stack across every method in the body — the CLOSURE pushed
// before the *next* method would otherwise look like a cover witness for the
// class value itself). Tracked as a separate flag, not an early return, so
// the order the walk visits instructions in cannot change the verdict.
struct PersistenceResult {
    bool coverWitness = false;
    bool disqualified = false;
    std::set<std::pair<int, int>> births;
};

// Same backward walk as findDeclaringPushIndices (follows the *value* at
// `slot`, not its stack position — R7), instrumented to also decide
// persistence. `result.births` collects every declaring push found, exactly
// like findDeclaringPushIndices's `sites`; the caller only keeps them when
// the verdict is LOCAL.
void walkForPersistence(int fromIndex, int slot,
                        const std::vector<DecodedInstruction>& ins,
                        const LocalCfg& cfg,
                        const std::vector<StackState>& before,
                        const std::vector<StackState>& after,
                        const std::vector<bool>& reached,
                        PersistenceResult& result) {
    std::vector<bool> visited(ins.size(), false);
    std::deque<int> frontier(cfg.predecessors[fromIndex].begin(),
                             cfg.predecessors[fromIndex].end());
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop_front();
        if (visited[cur] || !static_cast<bool>(reached[cur])) {
            continue;
        }
        visited[cur] = true;

        StackEffect effect = stackEffect(ins[cur]);
        int popReach = before[cur].height - effect.popCount;

        if (ins[cur].op == Op::DEFINE_METHOD && popReach - 1 == slot) {
            result.disqualified = true;
        }

        if (popReach <= slot) {
            // Same birth condition as findDeclaringPushIndices: `cur`'s pop
            // reached `slot`, and its own push lands back there.
            if (slot < after[cur].height) {
                result.births.insert({cur, slot});
            }
            continue;
        }

        if (effect.popCount == 0 && effect.pushCount >= 1 &&
            before[cur].height == slot + 1) {
            result.coverWitness = true;
        }

        for (int pred : cfg.predecessors[cur]) {
            frontier.push_back(pred);
        }
    }
}

// Runs the persistence test at every reached POP and folds every LOCAL
// verdict's births into `sites`. Discovery here depends on no reference
// elsewhere in the function — the property that closes R8 as a class,
// per the referee ruling: every POP is examined directly.
void findPersistentPopLocals(const std::vector<DecodedInstruction>& ins,
                             const LocalCfg& cfg,
                             const std::vector<StackState>& before,
                             const std::vector<StackState>& after,
                             const std::vector<bool>& reached,
                             std::set<std::pair<int, int>>& sites) {
    for (size_t i = 0; i < ins.size(); i++) {
        if (!static_cast<bool>(reached[i]) || ins[i].op != Op::POP) {
            continue;
        }
        int idx = static_cast<int>(i);
        int slot = before[i].height - 1;
        PersistenceResult result;
        walkForPersistence(idx, slot, ins, cfg, before, after, reached, result);
        if (result.coverWitness && !result.disqualified) {
            sites.insert(result.births.begin(), result.births.end());
        }
    }
}

// A local's frame can end with no explicit reclaim opcode at all: a
// function's own top-level locals never run through endScope(), because
// there is no enclosing block left to close there — the whole frame is
// discarded at RETURN instead. A local that is both unread (no
// GET_LOCAL/SET_LOCAL/capture) *and* never reaches an explicit
// POP/CLOSE_UPVALUE either gets no site from anything above, at any offset
// (function `f(){ var a=1; var never=99; print a; }` — `never` has no POP in
// the whole chunk for findPersistentPopLocals's test to run at).
//
// Checkpoint 3's own invariant gives the fix for free: operandDepth() is
// exactly 1 immediately before every RETURN (the return value, and nothing
// else, is ever a temporary there — vm.cpp tears down the rest of the frame
// wholesale). So every slot below that one temporary is local, with no
// reference needed to prove it; chase each one's declaring push from the
// RETURN itself.
void backfillFromFrameTeardown(const std::vector<DecodedInstruction>& ins,
                               const LocalCfg& cfg,
                               const std::vector<StackState>& before,
                               const std::vector<StackState>& after,
                               const std::vector<bool>& reached,
                               std::set<std::pair<int, int>>& sites) {
    for (size_t i = 0; i < ins.size(); i++) {
        if (!static_cast<bool>(reached[i]) || ins[i].op != Op::RETURN) {
            continue;
        }
        int idx = static_cast<int>(i);
        chaseSlotsDownward(idx, before[i].height - 2, ins, cfg, before, after,
                           reached, sites);
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
        if (!static_cast<bool>(reached[i])) {
            continue;
        }
        int idx = static_cast<int>(i);
        switch (ins[i].op) {
        case Op::GET_LOCAL:
        case Op::SET_LOCAL:
            // Chases the named slot *and* every slot below it (see
            // chaseSlotsDownward): clox's scoping is strictly LIFO
            // (abstract_stack.h's own StackState comment), so a slot the VM
            // itself addresses as a local proves every lower slot is local
            // too, independent of whether anything ever names *them*. This
            // is not the rejected "run length implies local count" guess —
            // it starts from a slot GET_LOCAL/SET_LOCAL already confirms,
            // not from POP adjacency — and it is the only way a local both
            // unread and never reclaimed (no POP, and RETURN unreachable
            // because the function diverges, e.g. a trailing `for (;;) {}`)
            // is ever found at all.
            chaseSlotsDownward(idx, ins[i].byteOperand, ins, cfg, before, after,
                               reached, sites);
            break;
        case Op::CLOSURE:
            // isLocal upvalue entries name slots in *this* function
            // (chunk_decoder.h) — a local that is captured but never read
            // back via plain GET_LOCAL/SET_LOCAL. Chases downward from each,
            // for the same reason as the GET_LOCAL/SET_LOCAL case above.
            for (const auto& uv : ins[i].upvalues) {
                if (uv.isLocal) {
                    chaseSlotsDownward(idx, uv.index, ins, cfg, before, after,
                                       reached, sites);
                }
            }
            // The CLOSURE's *own* pushed value can itself become a local
            // (a nested `fun name() {...}` declaration) — its declaring
            // push is this very instruction, at the position its own push
            // lands (06_shared_upvalue's `set`: never captured, never read
            // back, still a real local slot — findDeclaringPushIndices
            // above would never find it, since nothing ever names its
            // slot). `chaseSlotsDownward` cannot find *this* site itself
            // (it searches idx's predecessors, and this slot is born at idx,
            // not before it), so it is inserted directly; slots below it are
            // then chased exactly as for the other sources above.
            if (!closureIsConsumedImmediately(ins, i)) {
                sites.insert({idx, after[i].height - 1});
                chaseSlotsDownward(idx, after[i].height - 2, ins, cfg, before,
                                   after, reached, sites);
            }
            break;
        case Op::CLOSE_UPVALUE:
            // Always closes the current top of stack (vm.cpp), and only a
            // captured local is ever closed. Chases downward for the same
            // reason as the GET_LOCAL/SET_LOCAL case above.
            chaseSlotsDownward(idx, before[i].height - 1, ins, cfg, before,
                               after, reached, sites);
            break;
        default:
            break;
        }
    }

    // A local reachable at RETURN with no explicit reclaim anywhere (a
    // function's own top-level scope) gets no site from the reference-driven
    // loop above at all — see backfillFromFrameTeardown's own comment.
    backfillFromFrameTeardown(ins, cfg, before, after, reached, sites);

    // R8: the persistence test at every POP — see findPersistentPopLocals's
    // own comment. Independent of the
    // reference-driven loop above; it can add a site the loop above never
    // could reach (no GET_LOCAL/SET_LOCAL/capture at all) and it can
    // re-derive one the loop above already found (deduplicated by `sites`
    // being a set).
    findPersistentPopLocals(ins, cfg, before, after, reached, sites);
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
            if (changed && !static_cast<bool>(queued[succ])) {
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
// is declaring-push-timed now (R1) and reclaim/return-anchored (R8 — see
// findInvisibleVarIndices), not reference-timed, so a predecessor's `after`
// state already reflects every declaration on its own path with no further
// reconciliation needed at the point of use.
//
// An alternative design compares raw (height, localCount) at a merge, not
// only operand depth, turning "the join becomes an assertion, not a
// repair." Tried and measured: it does not hold on the differential-test
// corpus. `bootstrap/loxpp_interpreter.lox`'s `resolveStmt`
// has 16 `match` arms sharing one exit; the 14 written as a single
// expression close only their own pattern bindings before the jump (raw
// state (4,4) at the shared point), but the 2 written as a `{ ...; ...; }`
// block (`WhileStmt`, `ForInStmt`) additionally close the match's own
// hidden scrutinee slot inside their own block scope, arriving instead at
// (3,3) — confirmed by hand at offset 705, both groups verified against the
// real disassembly. Operand depth agrees (0 both ways) exactly as the
// pre-existing comment on runFixpoint's join described; raw state does not,
// and this is a real, compiler-emitted difference, not an analysis gap — so
// operand depth remains the invariant this analysis guarantees. It is what
// the verifier actually enforces (JVM/CLR track slots and the operand stack
// separately; two arms may legitimately leave a different number of slots
// occupied at a merge, only the operand stack itself must match), and it is
// what the emitter needs for `.limit stack`/`.maxstack`.
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
        if (!static_cast<bool>(reached[i])) {
            continue;
        }
        std::optional<int> depth;
        for (int pred : cfg.predecessors[i]) {
            if (!static_cast<bool>(reached[pred])) {
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

// R11: this is a safety net for a *false* site, not a detector for a
// *missing* one. It iterates only over the sites discovery already found
// (`declaredSlotsAt[i].empty()` skips silently), so a slot R8 fails to
// recognize at all is invisible to this loop by construction — it is not
// what catches a slot R8 misses. What it does catch: if some future change
// makes findInvisibleVarIndices report a slot that does not match the true
// local count at its own recognition point, this throws immediately instead
// of letting a wrong number reach the JVM emitter. With the persistence test
// in place (findPersistentPopLocals), every POP receives a direct, decidable
// classification at the reclaim site itself, so no silent gap class remains
// for a bogus site to hide behind either — this guard is now a real net,
// not a false promise.
//
// Runs once post-convergence for the same reason validateMergeConsistency
// does: a recognition point can be advance()-ed several times while the
// fixpoint is still converging (a loop's back-edge has not propagated yet),
// and a transient, not-yet-final `before` there must not be mistaken for a
// real gap. Using `result.before` — already the fully converged state —
// removes that risk entirely; this is a pure re-derivation with no
// dependence on iteration order.
//
// Checks that every recognized slot equals the local count computed by
// popping the instruction's own operands, in the order declaredSlotsAt[i]
// lists them (ascending, since it is built from a std::set<pair<int,int>>).
//
// R15: given external linkage (not left in the anonymous namespace above),
// so test_backend_abstract_stack.cpp can drive it directly with a
// hand-built `declaredSlotsAt` and assert the throw — see
// DirectlyBuiltGapThrowsWithTheRightMessage. A gap needs a genuinely
// inconsistent `declaredSlotsAt` to fire (analyzeStack's own discovery
// never produces one; that is the property R8's redesign establishes), so
// no real chunk can drive this guard through analyzeStack alone.
void validateNoInvisibleVarGaps(
    const std::vector<DecodedInstruction>& ins,
    const std::vector<StackState>& before, const std::vector<bool>& reached,
    const std::vector<std::vector<int>>& declaredSlotsAt,
    const std::string& functionId) {
    for (size_t i = 0; i < ins.size(); i++) {
        if (!static_cast<bool>(reached[i]) || declaredSlotsAt[i].empty()) {
            continue;
        }
        int localCount =
            localCountAfterPops(before[i], stackEffect(ins[i]).popCount);
        for (int slot : declaredSlotsAt[i]) {
            if (slot != localCount) {
                throw std::runtime_error(
                    "abstract_stack: invisible-var recognition gap in "
                    "function '" +
                    functionId + "' at offset " +
                    std::to_string(ins[i].offset) + ": slot " +
                    std::to_string(slot) + " recognized with local count " +
                    std::to_string(localCount) +
                    " — an unread sibling local has no declaring-push site");
            }
            localCount = slot + 1;
        }
    }
}

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
        if (!static_cast<bool>(reached[i])) {
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
        if (!static_cast<bool>(reached[i])) {
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
        if (!static_cast<bool>(reached[i])) {
            continue;
        }
        StackState before = *state[i];
        StackState after = advance(ins, i, before, &declaredSlotsAt);
        result.before[i] = before;
        result.after[i] = after;

        // R12: when this instruction's own push
        // is an invisible var's declaring push (declaredSlotsAt[i] non-
        // empty), the emitter's store has not run yet at this exact point —
        // the value still needs real operand-stack room here, even though
        // `after` already counts it as local and reports operandDepth 0 for
        // it. after.operandDepth() alone can therefore undercount by exactly
        // the number of slots just recognized; add it back so the bound
        // stays safe by construction. An undercount is a JVM VerifyError,
        // and the CLR backend has no jasmin fallback to hide behind.
        int depth = std::max(before.operandDepth(),
                             after.operandDepth() +
                                 static_cast<int>(declaredSlotsAt[i].size()));
        result.maxOperandDepth = std::max(result.maxOperandDepth, depth);

        if (ins[i].op == Op::POP) {
            PopKind kind =
                topIsLocal(before) ? PopKind::LOCAL_RECLAIM : PopKind::TEMP;
            result.pops.push_back({ins[i].offset, kind});
        }
    }

    validateMergeConsistency(ins, cfg, result.after, reached, fn.id);
    validateNoInvisibleVarGaps(ins, result.before, reached, declaredSlotsAt,
                               fn.id);

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
