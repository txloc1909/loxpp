#include "qbe_promotion.h"

#include "exec_objects.h" // ObjFunction (arity)

#include <algorithm>
#include <deque>
#include <set>
#include <stdexcept>
#include <string>

namespace qbe {

namespace {

// Above this many promotion candidates in one function, only read-only
// slots are promoted (see the pressure cap in planPromotion).
constexpr int kMaxPromotedSlots = 6;

std::string paramName(int slot) { return "%qp" + std::to_string(slot); }
std::string defName(int offset) { return "%q" + std::to_string(offset); }
std::string phiName(int block, int slot) {
    return "%qf" + std::to_string(block) + "_" + std::to_string(slot);
}
std::string catchName(int block, int slot) {
    return "%qc" + std::to_string(block) + "_" + std::to_string(slot);
}

// True when an opcode reads or writes the value in its own top stack cell.
// A pure push (CONSTANT, GET_LOCAL, ...) only looks at the cell below, and a
// pure control transfer has no stack operand at all; every other opcode
// consumes or peeks its top cell, so a local slot sitting there is being
// used as a temporary and must not be promoted. POP is excluded: reclaiming
// a local is the normal end of its scope, not a use as a temporary.
bool usesTopOperand(Op op) {
    switch (op) {
    case Op::CONSTANT:
    case Op::NIL:
    case Op::TRUE:
    case Op::FALSE:
    case Op::GET_LOCAL:
    case Op::GET_GLOBAL:
    case Op::GET_UPVALUE:
    case Op::CLASS:
    case Op::CLOSURE:
    case Op::JUMP:
    case Op::LOOP:
    case Op::PUSH_HANDLER:
    case Op::POP_HANDLER:
    case Op::RUN_DEFERS:
    case Op::POP:
        return false;
    default:
        return true;
    }
}

// Applies one block's promotion events to `cur`. The emitter mirrors this
// exactly, so a value name the plan reports is always the one the emitter
// defines.
void advance(
    const BasicBlock& block,
    const std::unordered_map<int, std::pair<int, std::string>>& declareAt,
    const std::unordered_map<int, std::pair<int, std::string>>& setLocalAt,
    const std::unordered_map<int, int>& reclaimAt,
    std::unordered_map<int, std::string>& cur) {
    for (const DecodedInstruction& ins : block.instructions) {
        auto d = declareAt.find(ins.offset);
        if (d != declareAt.end()) {
            cur[d->second.first] = d->second.second;
        }
        auto s = setLocalAt.find(ins.offset);
        if (s != setLocalAt.end()) {
            cur[s->second.first] = s->second.second;
        }
        auto r = reclaimAt.find(ins.offset);
        if (r != reclaimAt.end()) {
            cur.erase(r->second);
        }
    }
}

// A phi whose predecessors all carry one value carries no information: the
// block cycle did not change the slot, so the phi is that value. The merge
// records such a collapse here instead of rewriting the phi forever (Braun et
// al., "Simple and Efficient Construction of Static Single Assignment Form",
// the trivial-phi rule), and every later comparison goes through resolve().
using CollapseMap = std::unordered_map<std::string, std::string>;

std::string resolve(const CollapseMap& collapse, const std::string& name) {
    std::string cur = name;
    // A collapse points at a definition or at another phi, so a chain is
    // short; the bound keeps a malformed map from looping.
    for (int i = 0; i < 64; i++) {
        auto it = collapse.find(cur);
        if (it == collapse.end() || it->second == cur) {
            return cur;
        }
        cur = it->second;
    }
    return cur;
}

// Recomputes `t`'s entry value from its reachable predecessors' exit values:
// a slot survives only when every reachable predecessor carries it. When the
// predecessors carry one value it is forwarded; when they carry more the
// block gets an SSA phi.
//
// Two cases leave the slot in its stack cell instead, recorded permanently in
// droppedSlots so the decision cannot be revisited. One: the phi would need
// an argument naming itself, which QBE cannot express (an argument must be
// defined in its own predecessor). Two: the block already collapsed its phi
// onto one value and a later merge carries another, so the collapse no longer
// holds. Both write the slot back on every incoming edge (tailSpill), so the
// block's reload reads this iteration's value and not a stale one — a branch
// edge is not a safe point. A merge that keeps giving up and taking the slot
// back would never stop rewriting it, and the worklist would never converge
// (#1052).
void mergeInto(
    int t, const Cfg& cfg, const std::vector<bool>& reachable,
    const std::vector<bool>& computed,
    const std::vector<std::unordered_map<int, std::string>>& outValue,
    const std::unordered_map<int, std::string>& prologueValue,
    std::vector<std::unordered_map<int, std::string>>& entryValue,
    std::vector<std::vector<PromoPhi>>& phis,
    std::vector<std::set<int>>& tailSpill,
    std::vector<std::set<int>>& droppedSlots, CollapseMap& collapse,
    std::set<std::string>& createdPhis) {
    // (predecessor index, its exit values). The emitter's prologue block is
    // a real predecessor of cfg block 0 even though no cfg edge names it.
    std::vector<std::pair<int, const std::unordered_map<int, std::string>*>>
        preds;
    auto addPred = [&](int p,
                       const std::unordered_map<int, std::string>* values) {
        for (const auto& existing : preds) {
            if (existing.first == p) {
                return;
            }
        }
        preds.emplace_back(p, values);
    };
    if (t == 0) {
        addPred(kProloguePred, &prologueValue);
    }
    for (int p : cfg.blocks[static_cast<std::size_t>(t)].predecessors) {
        if (!reachable[static_cast<std::size_t>(p)] ||
            !computed[static_cast<std::size_t>(p)]) {
            continue;
        }
        addPred(p, &outValue[static_cast<std::size_t>(p)]);
    }

    std::unordered_map<int, std::string> merged;
    std::vector<PromoPhi> newPhis;
    auto leaveInStackCell = [&](int slot) {
        droppedSlots[static_cast<std::size_t>(t)].insert(slot);
        for (const auto& [p, values] : preds) {
            (void)values;
            if (p != kProloguePred) {
                tailSpill[static_cast<std::size_t>(p)].insert(slot);
            }
        }
    };
    if (!preds.empty()) {
        std::set<int> slots;
        for (const auto& [p, values] : preds) {
            (void)p;
            for (const auto& [slot, value] : *values) {
                (void)value;
                slots.insert(slot);
            }
        }
        for (int slot : slots) {
            if (droppedSlots[static_cast<std::size_t>(t)].contains(slot)) {
                continue;
            }
            bool allHave = true;
            for (const auto& [p, values] : preds) {
                (void)p;
                if (!values->contains(slot)) {
                    allHave = false;
                    break;
                }
            }
            if (!allHave) {
                continue;
            }
            std::string name = phiName(t, slot);
            bool collapsedPhi = resolve(collapse, name) != name;
            std::set<std::string> values;
            bool selfReferenced = false;
            for (const auto& [p, pred] : preds) {
                (void)p;
                std::string value = resolve(collapse, pred->at(slot));
                if (!collapsedPhi && value == name) {
                    selfReferenced = true;
                } else {
                    values.insert(std::move(value));
                }
            }
            if (values.size() == 1) {
                // One value on every path that changes the slot; the paths
                // that carry this block's own phi change nothing.
                std::string value = *values.begin();
                if (createdPhis.contains(name)) {
                    collapse[name] = value;
                }
                merged[slot] = std::move(value);
                continue;
            }
            if (values.empty() || selfReferenced || collapsedPhi) {
                leaveInStackCell(slot);
                continue;
            }
            PromoPhi phi;
            phi.slot = slot;
            phi.name = name;
            for (const auto& [p, pred] : preds) {
                phi.args.emplace_back(p, resolve(collapse, pred->at(slot)));
            }
            merged[slot] = name;
            createdPhis.insert(name);
            newPhis.push_back(std::move(phi));
        }
    }
    entryValue[static_cast<std::size_t>(t)] = std::move(merged);
    phis[static_cast<std::size_t>(t)] = std::move(newPhis);
}

} // namespace

PromotionPlan planPromotion(const DecodedFunction& fn,
                            const FunctionStackAnalysis& analysis,
                            const FunctionCaptureInfo& captures,
                            const Cfg& cfg) {
    PromotionPlan plan;

    std::unordered_map<int, const StackState*> beforeByOffset;
    std::unordered_map<int, std::size_t> indexByOffset;
    for (std::size_t i = 0; i < fn.instructions.size(); i++) {
        beforeByOffset[fn.instructions[i].offset] = &analysis.before[i];
        indexByOffset[fn.instructions[i].offset] = i;
    }

    int maxHeight = 0;
    for (std::size_t i = 0; i < fn.instructions.size(); i++) {
        if (!analysis.reached[i]) {
            continue;
        }
        maxHeight = std::max(
            {maxHeight, analysis.before[i].height, analysis.after[i].height});
    }

    plan.candidate.assign(static_cast<std::size_t>(maxHeight) + 1, false);
    auto markCandidate = [&](int slot) {
        if (slot > 0 && slot < static_cast<int>(plan.candidate.size())) {
            plan.candidate[static_cast<std::size_t>(slot)] = true;
        }
    };
    for (int s = 1; s <= fn.function->arity; s++) {
        markCandidate(s);
    }
    for (const InvisibleVarSite& iv : analysis.invisibleVars) {
        markCandidate(iv.slot);
    }
    for (const auto& [slot, ranges] : captures.liveRangesBySlot) {
        (void)ranges;
        if (slot >= 0 && slot < static_cast<int>(plan.candidate.size())) {
            plan.candidate[static_cast<std::size_t>(slot)] = false;
        }
    }
    // A local slot used directly as an instruction's operand — a folded
    // `match` result, the hidden slot of an operand that a `match` folds
    // into (the `r` of `r + str(match ...)`), or a for-in loop's hidden
    // iterator slot, which the compiler leaves on the stack instead of
    // copying with GET_LOCAL — is read and written as a temporary. Its
    // memory is authoritative, so keep it out of registers. Every cell the
    // instruction pops counts, not only the top one: ADD consumes the cell
    // below its top too, and writes its result there.
    for (std::size_t i = 0; i < fn.instructions.size(); i++) {
        if (!analysis.reached[i] || !usesTopOperand(fn.instructions[i].op)) {
            continue;
        }
        const StackState& before = analysis.before[i];
        int operands = std::max(1, stackPopCount(fn.instructions[i]));
        for (int k = 0; k < operands; k++) {
            int slot = before.height - 1 - k;
            if (slot > 0 && slot < before.localCount &&
                slot < static_cast<int>(plan.candidate.size())) {
                plan.candidate[static_cast<std::size_t>(slot)] = false;
            }
        }
    }

    // Register-pressure cap. Promoting a reassigned local adds a phi and
    // lengthens its live range. In a function with many locals that loses
    // to QBE's own register allocation, because QBE spills the extra live
    // values anyway. Above this count, keep only read-only slots: a single
    // dominating definition needs no phi, and a read-only slot's stack cell
    // never goes stale.
    {
        int total = 0;
        for (bool c : plan.candidate) {
            total += c ? 1 : 0;
        }
        if (total > kMaxPromotedSlots) {
            std::vector<bool> isSet(plan.candidate.size(), false);
            for (const DecodedInstruction& ins : fn.instructions) {
                if (ins.op == Op::SET_LOCAL && ins.byteOperand >= 0 &&
                    ins.byteOperand < static_cast<int>(isSet.size())) {
                    isSet[static_cast<std::size_t>(ins.byteOperand)] = true;
                }
            }
            for (std::size_t i = 0; i < plan.candidate.size(); i++) {
                if (isSet[i]) {
                    plan.candidate[i] = false;
                }
            }
        }
    }

    bool hasCandidate = false;
    for (bool c : plan.candidate) {
        hasCandidate = hasCandidate || c;
    }
    // Size the per-block vectors now: the emitter indexes them for every
    // block, even when nothing is a candidate (an empty candidate set still
    // has to answer "no promotion" per block, not read past the end).
    std::size_t n = cfg.blocks.size();
    plan.entryValue.resize(n);
    plan.phis.resize(n);
    plan.catchReload.resize(n);
    plan.tailSpill.resize(n);

    if (!hasCandidate) {
        return plan;
    }

    for (const DecodedInstruction& ins : fn.instructions) {
        if (ins.op != Op::SET_LOCAL) {
            continue;
        }
        int slot = ins.byteOperand;
        if (slot > 0 && slot < static_cast<int>(plan.candidate.size()) &&
            plan.candidate[static_cast<std::size_t>(slot)]) {
            plan.setLocalAt[ins.offset] = {slot, defName(ins.offset)};
        }
    }
    for (const InvisibleVarSite& iv : analysis.invisibleVars) {
        // A catch variable's slot is both declared and SET_LOCAL at the same
        // offset (the thrown value is stored into it): one definition, not
        // two. SET_LOCAL already owns that offset.
        if (plan.setLocalAt.contains(iv.offset)) {
            continue;
        }
        if (iv.slot > 0 && iv.slot < static_cast<int>(plan.candidate.size()) &&
            plan.candidate[static_cast<std::size_t>(iv.slot)]) {
            plan.declareAt[iv.offset] = {iv.slot, defName(iv.offset)};
        }
    }
    for (const PopClassification& pop : analysis.pops) {
        if (pop.kind != PopKind::LOCAL_RECLAIM) {
            continue;
        }
        auto it = beforeByOffset.find(pop.offset);
        if (it == beforeByOffset.end()) {
            continue;
        }
        int slot = it->second->height - 1;
        if (slot > 0 && slot < static_cast<int>(plan.candidate.size()) &&
            plan.candidate[static_cast<std::size_t>(slot)]) {
            plan.reclaimAt[pop.offset] = slot;
        }
    }

    std::vector<bool> reachable(n, false);
    for (std::size_t b = 0; b < n; b++) {
        auto it = indexByOffset.find(cfg.blocks[b].leaderOffset);
        if (it != indexByOffset.end()) {
            reachable[b] = analysis.reached[it->second];
        }
    }

    // The prologue block's exit values: every promoted parameter, loaded
    // there. Block 0 starts from these and merges its cfg predecessors in
    // (a loop can jump back to block 0, making it a header).
    std::unordered_map<int, std::string> prologueValue;
    for (int s = 1; s <= fn.function->arity; s++) {
        if (s < static_cast<int>(plan.candidate.size()) &&
            plan.candidate[static_cast<std::size_t>(s)]) {
            prologueValue[s] = paramName(s);
        }
    }
    if (n > 0) {
        plan.entryValue[0] = prologueValue;
    }
    for (const HandlerEntry& he : cfg.handlerEntries) {
        std::size_t cb = static_cast<std::size_t>(he.catchBlock);
        if (cb >= n || !reachable[cb]) {
            continue;
        }
        auto it = beforeByOffset.find(he.pushHandlerOffset);
        if (it == beforeByOffset.end()) {
            continue;
        }
        int localCount = it->second->localCount;
        for (int s = 1; s < localCount; s++) {
            if (s < static_cast<int>(plan.candidate.size()) &&
                plan.candidate[static_cast<std::size_t>(s)]) {
                std::string name = catchName(he.catchBlock, s);
                plan.entryValue[cb][s] = name;
                plan.catchReload[cb][s] = name;
            }
        }
    }

    std::vector<std::unordered_map<int, std::string>> outValue(n);
    std::vector<bool> computed(n, false);
    // Slots a merge had to leave in the stack cell. The decision is
    // permanent: re-deriving a value from a predecessor that later gains one
    // would restart the rewrite that made the merge give up.
    std::vector<std::set<int>> dropped(n);
    CollapseMap collapse;
    std::set<std::string> createdPhis;
    std::deque<int> worklist;
    std::vector<bool> queued(n, false);
    auto enqueue = [&](int b) {
        if (!queued[static_cast<std::size_t>(b)]) {
            queued[static_cast<std::size_t>(b)] = true;
            worklist.push_back(b);
        }
    };
    // Seed only the blocks whose entry value is defined without a
    // predecessor: block 0 (the prologue loads the promoted parameters) and
    // each catch handler (cfg.h wires no generic edge into one; every
    // fallible call spills the promoted slots before it can throw there).
    // Every other block is computed when a real predecessor first reaches
    // it. Seeding all of them with an empty value is not equivalent: an
    // empty value is indistinguishable from a genuine "no value on this
    // path", so the block's own merge would drop the slot, and a block
    // cycle (a nested loop, or a try body that loops) could then never carry
    // it back in — the promotion dataflow is a must analysis and only
    // converges to a useful fixpoint when each block starts from what its
    // predecessors actually carry (#1052).
    if (n > 0 && reachable[0]) {
        enqueue(0);
    }
    for (const HandlerEntry& he : cfg.handlerEntries) {
        if (he.catchBlock >= 0 && static_cast<std::size_t>(he.catchBlock) < n &&
            reachable[static_cast<std::size_t>(he.catchBlock)]) {
            enqueue(he.catchBlock);
        }
    }

    std::size_t guard = 0;
    std::size_t guardLimit = (n + 1) * (maxHeight + 1) * 8 + 1024;
    while (!worklist.empty()) {
        if (++guard > guardLimit) {
            throw std::runtime_error(
                "qbe_promotion: promotion dataflow did not converge in "
                "function id=" +
                fn.id);
        }
        int b = worklist.front();
        worklist.pop_front();
        queued[static_cast<std::size_t>(b)] = false;
        std::unordered_map<int, std::string> cur =
            plan.entryValue[static_cast<std::size_t>(b)];
        advance(cfg.blocks[static_cast<std::size_t>(b)], plan.declareAt,
                plan.setLocalAt, plan.reclaimAt, cur);
        if (computed[static_cast<std::size_t>(b)] &&
            cur == outValue[static_cast<std::size_t>(b)]) {
            continue;
        }
        outValue[static_cast<std::size_t>(b)] = cur;
        computed[static_cast<std::size_t>(b)] = true;
        for (const CfgEdge& edge :
             cfg.blocks[static_cast<std::size_t>(b)].successors) {
            int t = edge.targetBlock;
            if (t < 0 || static_cast<std::size_t>(t) >= n ||
                !reachable[static_cast<std::size_t>(t)]) {
                continue;
            }
            auto before = plan.entryValue[static_cast<std::size_t>(t)];
            mergeInto(t, cfg, reachable, computed, outValue, prologueValue,
                      plan.entryValue, plan.phis, plan.tailSpill, dropped,
                      collapse, createdPhis);
            // A target that has never been computed still moves forward
            // even when its entry value does not change, or the blocks
            // downstream of it would never be visited at all.
            if (!computed[static_cast<std::size_t>(t)] ||
                plan.entryValue[static_cast<std::size_t>(t)] != before) {
                enqueue(t);
            }
        }
    }

    // Resolve what the collapse map redirected, and drop every phi that ends
    // up carrying a single value. A phi can turn out trivial only after the
    // merge that created it, so repeat until the plan stops changing.
    for (std::size_t round = 0;; round++) {
        bool changed = false;
        for (auto& blockPhis : plan.phis) {
            std::vector<PromoPhi> kept;
            for (PromoPhi& phi : blockPhis) {
                if (resolve(collapse, phi.name) != phi.name) {
                    changed = true;
                    continue;
                }
                std::set<std::string> args;
                for (const auto& [p, value] : phi.args) {
                    (void)p;
                    args.insert(resolve(collapse, value));
                }
                if (args.size() == 1) {
                    collapse[phi.name] = *args.begin();
                    changed = true;
                    continue;
                }
                for (auto& [p, value] : phi.args) {
                    (void)p;
                    value = resolve(collapse, value);
                }
                kept.push_back(std::move(phi));
            }
            blockPhis = std::move(kept);
        }
        for (auto& blockValues : plan.entryValue) {
            for (auto& [slot, value] : blockValues) {
                std::string resolved = resolve(collapse, value);
                if (resolved != value) {
                    value = std::move(resolved);
                    changed = true;
                }
            }
        }
        if (!changed || round > createdPhis.size() + 1) {
            break;
        }
    }

    return plan;
}

} // namespace qbe
