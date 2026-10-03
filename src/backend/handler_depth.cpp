#include "backend/handler_depth.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int handlerEffect(Op op) {
    switch (op) {
    case Op::PUSH_HANDLER:
        return 1;
    case Op::POP_HANDLER:
        return -1;
    default:
        return 0;
    }
}

bool isTerminal(Op op) {
    return op == Op::RETURN || op == Op::THROW || op == Op::MATCH_ERROR;
}

} // namespace

HandlerDepthAnalysis
analyzeHandlerDepthIns(const std::vector<DecodedInstruction>& ins,
                       const std::string& functionId) {
    HandlerDepthAnalysis out;
    out.functionId = functionId;
    out.before.assign(ins.size(), 0);
    out.after.assign(ins.size(), 0);
    out.reached.assign(ins.size(), false);
    if (ins.empty()) {
        return out;
    }

    std::unordered_map<int, int> offsetToIndex;
    offsetToIndex.reserve(ins.size());
    for (size_t i = 0; i < ins.size(); i++) {
        offsetToIndex[ins[i].offset] = static_cast<int>(i);
    }

    // One lookup style for all jump targets: missing targets throw
    // runtime_error with the source offset, never bare out_of_range.
    auto targetIndex = [&](int fromOffset, int targetOffset) {
        auto it = offsetToIndex.find(targetOffset);
        if (it == offsetToIndex.end()) {
            throw std::runtime_error(
                "handler_depth: jump at offset " + std::to_string(fromOffset) +
                " targets unknown offset " + std::to_string(targetOffset) +
                " in " + functionId);
        }
        return it->second;
    };

    // Catch entries are never ordinary branch targets (chunk.h). Collect
    // them first so no edge below lands on one by accident.
    std::vector<bool> isCatchEntry(ins.size(), false);
    std::vector<int> pushToCatch(ins.size(), -1);
    for (size_t i = 0; i < ins.size(); i++) {
        if (ins[i].op == Op::PUSH_HANDLER) {
            int catchIdx = targetIndex(ins[i].offset, ins[i].jumpTarget);
            isCatchEntry[static_cast<size_t>(catchIdx)] = true;
            pushToCatch[i] = catchIdx;
        }
    }

    // Structural LIFO pairing: a POP_HANDLER closes the nearest open
    // PUSH_HANDLER in program order (abstract_stack.cpp). Names no region
    // of its own, so reachability cannot pair them instead.
    {
        std::vector<int> open;
        for (size_t i = 0; i < ins.size(); i++) {
            if (ins[i].op == Op::PUSH_HANDLER) {
                open.push_back(static_cast<int>(i));
            } else if (ins[i].op == Op::POP_HANDLER) {
                if (open.empty()) {
                    throw std::runtime_error(
                        "handler_depth: POP_HANDLER at offset " +
                        std::to_string(ins[i].offset) +
                        " has no matching PUSH_HANDLER");
                }
                open.pop_back();
            }
        }
    }

    std::vector<std::vector<int>> successors(ins.size());
    auto addEdge = [&](int from, int to) {
        if (isCatchEntry[static_cast<size_t>(to)]) {
            return;
        }
        successors[static_cast<size_t>(from)].push_back(to);
    };

    for (size_t i = 0; i < ins.size(); i++) {
        int idx = static_cast<int>(i);
        int fallthrough = (i + 1 < ins.size()) ? idx + 1 : -1;
        switch (ins[i].op) {
        case Op::JUMP:
        case Op::LOOP:
            addEdge(idx, targetIndex(ins[i].offset, ins[i].jumpTarget));
            break;
        case Op::PUSH_HANDLER:
        case Op::JUMP_IF_FALSE:
            addEdge(idx, targetIndex(ins[i].offset, ins[i].jumpTarget));
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        case Op::JUMP_TABLE:
            for (const auto& arm : ins[i].jumpTable) {
                addEdge(idx, targetIndex(ins[i].offset, arm.target));
            }
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        default:
            if (isTerminal(ins[i].op)) {
                break;
            }
            if (fallthrough >= 0) {
                addEdge(idx, fallthrough);
            }
            break;
        }
    }

    // matchingPush[popIdx]: the PUSH_HANDLER a given POP_HANDLER closes,
    // from the same structural LIFO pairing validated above. Unlike the
    // depth count, identifying which record closes cannot come from a plain
    // byte-order scan alone (a catch block's own bytes sit BETWEEN its own
    // PUSH_HANDLER and POP_HANDLER — "POP_HANDLER belongs only to the
    // normal-completion path", compiler.cpp — so a byte-order-only reading
    // would wrongly see the catch body as still "inside" its own handler's
    // region). This pairing only NAMES which push a pop closes; activeHandler
    // itself is still derived below through the same CFG worklist `before`
    // already uses, so a catch block's own lower depth is handled the same
    // way `before`/`after` already handle it (maybeSeedCatch).
    std::vector<int> matchingPush(ins.size(), -1);
    {
        std::vector<int> open;
        for (size_t i = 0; i < ins.size(); i++) {
            if (ins[i].op == Op::PUSH_HANDLER) {
                open.push_back(static_cast<int>(i));
            } else if (ins[i].op == Op::POP_HANDLER) {
                matchingPush[i] = open.back();
                open.pop_back();
            }
        }
    }

    // activeHandlerBefore[i]: the index of the innermost active PUSH_HANDLER
    // immediately before instruction i runs, threaded through the SAME
    // worklist as `before` (depth) — see HandlerDepthAnalysis::activeHandler
    // in handler_depth.h for the full rationale. parentOf[p] (p a
    // PUSH_HANDLER's own index) is the activeHandler in force right before p
    // itself ran — i.e. the ENCLOSING handler p's own POP_HANDLER must
    // restore control to. It is set at the same point `before[p]` becomes
    // available (p's own processing step below), so by the time any
    // POP_HANDLER matching p is processed, parentOf[p] is already resolved:
    // that POP_HANDLER is only reachable via a path that already ran
    // through p (matchingPush's own structural pairing), and p's own
    // successors — the only way to reach it — are not visited until AFTER
    // p's own processing step sets parentOf[p].
    std::vector<int> activeHandlerBefore(ins.size(), -1);
    std::vector<int> parentOf(ins.size(), -1);

    auto setBefore = [&](int idx, int depth, int activeHandler,
                         std::vector<int>& worklist) {
        if (!out.reached[static_cast<size_t>(idx)]) {
            out.reached[static_cast<size_t>(idx)] = true;
            out.before[static_cast<size_t>(idx)] = depth;
            activeHandlerBefore[static_cast<size_t>(idx)] = activeHandler;
            worklist.push_back(idx);
        } else if (out.before[static_cast<size_t>(idx)] != depth ||
                   activeHandlerBefore[static_cast<size_t>(idx)] !=
                       activeHandler) {
            throw std::runtime_error(
                "handler_depth: merge disagreement at offset " +
                std::to_string(ins[static_cast<size_t>(idx)].offset) + " in " +
                functionId);
        }
    };

    std::vector<int> worklist;
    setBefore(0, 0, -1, worklist);

    // A catch entry is seeded from its own PUSH_HANDLER, not from generic
    // predecessors: THROW removes the record before it jumps, so the depth
    // (and active handler) at catch entry equals the state before the PUSH.
    // Only a reached PUSH seeds its catch; a dead try seeds nothing. A
    // second PUSH for one catch rechecks agreement through setBefore.
    auto maybeSeedCatch = [&](int pushIdx, std::vector<int>& wl) {
        int catchIdx = pushToCatch[static_cast<size_t>(pushIdx)];
        if (catchIdx < 0) {
            return;
        }
        setBefore(catchIdx, out.before[static_cast<size_t>(pushIdx)],
                  activeHandlerBefore[static_cast<size_t>(pushIdx)], wl);
    };

    while (!worklist.empty()) {
        int idx = worklist.back();
        worklist.pop_back();
        size_t u = static_cast<size_t>(idx);
        int after = out.before[u] + handlerEffect(ins[u].op);
        if (after < 0) {
            throw std::runtime_error("handler_depth: POP_HANDLER at offset " +
                                     std::to_string(ins[u].offset) +
                                     " drives depth below zero" + " in " +
                                     functionId);
        }
        out.after[u] = after;
        int afterActiveHandler = activeHandlerBefore[u];
        if (ins[u].op == Op::PUSH_HANDLER) {
            parentOf[u] = activeHandlerBefore[u];
            afterActiveHandler = idx;
            maybeSeedCatch(idx, worklist);
        } else if (ins[u].op == Op::POP_HANDLER) {
            afterActiveHandler = parentOf[static_cast<size_t>(matchingPush[u])];
        }
        for (int succ : successors[u]) {
            setBefore(succ, after, afterActiveHandler, worklist);
        }
    }

    out.activeHandler = std::move(activeHandlerBefore);
    return out;
}

HandlerDepthAnalysis analyzeHandlerDepth(const DecodedFunction& fn) {
    return analyzeHandlerDepthIns(fn.instructions, fn.id);
}
