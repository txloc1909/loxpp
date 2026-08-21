#include "clr_emitter.h"

#include "cfg.h"
#include "exec_objects.h"
#include "object.h"
#include "value.h"
#include "zero_depth_local.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace clr {

namespace {

// Every Op enumerator's own spelling — for the "not implemented:" message
// only. A name missing here still throws, just with "UNKNOWN_OP".
std::string opName(Op op) {
    switch (op) {
    case Op::CONSTANT:
        return "CONSTANT";
    case Op::NIL:
        return "NIL";
    case Op::TRUE:
        return "TRUE";
    case Op::FALSE:
        return "FALSE";
    case Op::EQUAL:
        return "EQUAL";
    case Op::GREATER:
        return "GREATER";
    case Op::LESS:
        return "LESS";
    case Op::NEGATE:
        return "NEGATE";
    case Op::ADD:
        return "ADD";
    case Op::SUBTRACT:
        return "SUBTRACT";
    case Op::MULTIPLY:
        return "MULTIPLY";
    case Op::DIVIDE:
        return "DIVIDE";
    case Op::MODULO:
        return "MODULO";
    case Op::NOT:
        return "NOT";
    case Op::PRINT:
        return "PRINT";
    case Op::POP:
        return "POP";
    case Op::GET_LOCAL:
        return "GET_LOCAL";
    case Op::SET_LOCAL:
        return "SET_LOCAL";
    case Op::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case Op::GET_GLOBAL:
        return "GET_GLOBAL";
    case Op::SET_GLOBAL:
        return "SET_GLOBAL";
    case Op::JUMP:
        return "JUMP";
    case Op::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case Op::LOOP:
        return "LOOP";
    case Op::CALL:
        return "CALL";
    case Op::RETURN:
        return "RETURN";
    case Op::CLOSURE:
        return "CLOSURE";
    case Op::GET_UPVALUE:
        return "GET_UPVALUE";
    case Op::SET_UPVALUE:
        return "SET_UPVALUE";
    case Op::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    case Op::CLASS:
        return "CLASS";
    case Op::GET_PROPERTY:
        return "GET_PROPERTY";
    case Op::SET_PROPERTY:
        return "SET_PROPERTY";
    case Op::DEFINE_METHOD:
        return "DEFINE_METHOD";
    case Op::INVOKE:
        return "INVOKE";
    case Op::INHERIT:
        return "INHERIT";
    case Op::GET_SUPER:
        return "GET_SUPER";
    case Op::SUPER_INVOKE:
        return "SUPER_INVOKE";
    case Op::BUILD_LIST:
        return "BUILD_LIST";
    case Op::BUILD_MAP:
        return "BUILD_MAP";
    case Op::GET_INDEX:
        return "GET_INDEX";
    case Op::SET_INDEX:
        return "SET_INDEX";
    case Op::SLICE:
        return "SLICE";
    case Op::IN:
        return "IN";
    case Op::GET_ITER:
        return "GET_ITER";
    case Op::ITER_HAS_NEXT:
        return "ITER_HAS_NEXT";
    case Op::ITER_NEXT:
        return "ITER_NEXT";
    case Op::MATCH_ERROR:
        return "MATCH_ERROR";
    case Op::JUMP_TABLE:
        return "JUMP_TABLE";
    case Op::GET_TAG:
        return "GET_TAG";
    case Op::INSTANCEOF:
        return "INSTANCEOF";
    case Op::IS_SEQ:
        return "IS_SEQ";
    }
    return "UNKNOWN_OP";
}

[[noreturn]] void notImplemented(Op op) {
    throw std::runtime_error("clr_emitter: the CLR backend does not lower " +
                             opName(op) + " yet");
}

// Accumulates ilasm instruction text for one method body while tracking the
// CIL evaluation stack's depth in slots. Unlike the JVM operand stack, CIL's
// `.maxstack` counts one slot per value regardless of its underlying width —
// an unboxed `float64` occupies exactly one slot, the same as a boxed
// reference — so, unlike jvm_emitter.cpp's `Builder`, this one never needs a
// 2-word case for a wide primitive.
struct Builder {
    std::ostringstream text;
    int depth{0};
    int maxDepth{0};

    // `cellsRead` is how many cells this instruction consumes as input,
    // independent of `netDelta` (its net effect on depth). The two diverge
    // for `dup` (reads 1, net +1) and for `box` (reads 1, net 0) exactly as
    // much as for a plain `pop` (reads 1, net -1) — checking depth against
    // `cellsRead` before applying `netDelta` catches every one of those
    // reads, not only a net-negative one. A single earlier version of this
    // check compared post-apply depth against zero instead, which passes
    // `dup`/`box` on an undersized stack and only lets CoreCLR's later
    // InvalidProgramException find it at JIT time.
    void emit(const std::string& instruction, int cellsRead, int netDelta) {
        if (depth < cellsRead) {
            throw std::runtime_error(
                "clr_emitter: evaluation stack underflow emitting '" +
                instruction + "'");
        }
        text << "    " << instruction << "\n";
        depth += netDelta;
        maxDepth = std::max(maxDepth, depth);
    }

    // An ilasm label marker. Unindented (matching jvm_emitter.cpp's own
    // jasmin convention) and zero stack effect — a label is a name for an
    // offset, not an instruction.
    void label(const std::string& name) { text << name << ":\n"; }

    // Re-anchors `depth` to a value this pass did not itself derive by
    // emitting instructions — a CFG merge's entry depth, trusted from the
    // shared abstract-stack analysis. Goes through here, not a bare
    // assignment, so `maxDepth` still sees it.
    void resync(int newDepth) {
        depth = newDepth;
        maxDepth = std::max(maxDepth, depth);
    }
};

// Everything one chunk's straight-line lowering needs, threaded through by
// reference (jvm_emitter.cpp's own `Emitter` does the same, for the same
// reason: one dispatch function per opcode family instead of a single
// switch capturing a wall of local state).
struct Emitter {
    const DecodedFunction& fn;
    const FunctionStackAnalysis& analysis;
    Builder b;

    std::unordered_map<int, PopKind> popKinds;
    std::unordered_map<int, std::vector<int>> invisibleVarsByOffset;
    std::unordered_map<int, std::string> labelAtOffset;

    // Local index 0 holds the LoxGlobals instance; Lox frame slot `n` maps
    // to local index `baseSlot + n`; `scratchSlot` is one shared shuffle
    // slot for the SET_GLOBAL/DEFINE_GLOBAL P2 shuffle (globalsCall) — see
    // that function's own note. No calleeScratchSlot/argScratchBase: this
    // node lowers no CALL, BUILD_LIST, or BUILD_MAP.
    int baseSlot{1};
    int globalsSlot{0};
    int scratchSlot{0};

    // This pass's own forward walk, updated in offset order alongside every
    // invisible-var store: the Lox slot the most RECENTLY DECLARED
    // invisible-var site bound. -1 is a sentinel for "no site has run yet".
    // `resolveZeroDepthLocalSlot` (zero_depth_local.h) cross-checks this
    // against `before[i].localCount - 1` at a CFG merge, where the latter is
    // only an upper bound (abstract_stack.h) — see this file's own
    // top-of-file note.
    int lastInvisibleVarSlot{-1};

    // Whether the position this walk is about to visit is reachable by
    // fall-through from the instruction most recently emitted — false at
    // the very start, and reset every time a JUMP/LOOP/RETURN is emitted
    // (none of those fall through) or dead code is skipped between two live
    // instructions (nothing physical bridges that gap).
    bool prevCanFallThrough{false};
    int prevNaturalSuccessorOffset{-1};

    [[nodiscard]] int slotForLocal(int loxSlot) const {
        return baseSlot + loxSlot;
    }

    [[nodiscard]] bool reached(std::size_t idx) const {
        return idx < analysis.reached.size() &&
               static_cast<bool>(analysis.reached[idx]);
    }

    [[nodiscard]] const std::string& labelFor(int offset) const {
        auto it = labelAtOffset.find(offset);
        if (it == labelAtOffset.end()) {
            throw std::runtime_error(
                "clr_emitter: no CFG label at jump target " +
                std::to_string(offset));
        }
        return it->second;
    }

    // SET_LOCAL/SET_GLOBAL peek (the value stays on the stack); a following
    // POP the shared abstract-stack analysis already proved TEMP is exactly
    // that peeked value being discarded as a statement result, so folding
    // the pair into one plain store needs no dup and no separate pop
    // (bytecode-translation-problems.md P2).
    //
    // A POP that is a CFG block leader must stay a real instruction: every
    // edge into that leader needs its ilasm label (fusing away the
    // instruction fuses away the label with it — an ilasm assemble error,
    // not a wrong result), and the short-circuit edge into this leader
    // carries its own copy of the condition, which needs a real `pop` of
    // its own regardless of what the fall-through edge does with its copy
    // (probe 22 is exactly this shape).
    [[nodiscard]] bool fusablePop(std::size_t i) const {
        std::size_t j = i + 1;
        if (j >= fn.instructions.size() || !reached(j) ||
            fn.instructions[j].op != Op::POP) {
            return false;
        }
        if (labelAtOffset.find(fn.instructions[j].offset) !=
            labelAtOffset.end()) {
            return false;
        }
        auto it = popKinds.find(fn.instructions[j].offset);
        return it != popKinds.end() && it->second == PopKind::TEMP;
    }

    [[nodiscard]] std::string globalNameLiteral(int constantIndex) const {
        Value v = fn.function->chunk.getConstant(
            static_cast<uint16_t>(constantIndex));
        if (!isString(v)) {
            throw std::runtime_error("clr_emitter: DEFINE/GET/SET_GLOBAL "
                                     "constant is not a name string");
        }
        return ilasmStringLiteral(std::string(asObjString(v)->chars));
    }
};

// LoxGlobals.Define/Set are instance methods taking (string, object)
// (design decision A2), so every global write needs a receiver in scope —
// held in a CLR-only local the Lox++ frame never sees. The value to write
// already sits on top of the stack; CIL has no `swap`, so it is parked in
// `scratchSlot` while the receiver and name load, then reloaded as the
// call's last argument — one shape for both the peeking and
// fully-consuming case, differing only in whether the value is reloaded
// once more afterward.
void emitGlobalsCall(Emitter& e, const char* method,
                     const std::string& nameLiteral, bool peek) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), 0, +1);
    e.b.emit("ldstr " + nameLiteral, 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit(std::string("call instance void [LoxRuntime]Lox.LoxGlobals::") +
                 method + "(string, object)",
             3, -3);
    if (peek) {
        e.b.emit("ldloc " + scratch, 0, +1);
    }
}

// The CLR twin of jvm_emitter.cpp's own `loadNamedLocalAtZeroDepth`: at a
// peek-family consumer whose operand depth is zero, the value to consume
// is not a genuine evaluation-stack temporary — the shared abstract-stack
// pass already folded it into a named local (P2). `resolveZeroDepthLocalSlot`
// (zero_depth_local.h) is the one authority for naming that local: exact
// away from a CFG merge, and cross-checked against this pass's own forward
// tracker at a merge (see this file's own top-of-file note); this function
// only adds the CLR-specific `ldloc`.
int loadNamedLocalAtZeroDepth(Emitter& e, std::size_t i, int offset) {
    int loxSlot = resolveZeroDepthLocalSlot(
        e.analysis.before[i].localCount - 1, e.labelAtOffset.contains(offset),
        e.lastInvisibleVarSlot, offset, "clr_emitter");
    int slot = e.slotForLocal(loxSlot);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    return slot;
}

void emitConstant(Emitter& e, const DecodedInstruction& in) {
    Value v = e.fn.function->chunk.getConstant(
        static_cast<uint16_t>(in.constantIndex));
    if (is<Number>(v)) {
        e.b.emit("ldc.r8 " + ilasmDoubleLiteral(as<Number>(v)), 0, +1);
        e.b.emit("box [System.Runtime]System.Double", 1, 0);
    } else if (isString(v)) {
        e.b.emit("ldstr " +
                     ilasmStringLiteral(std::string(asObjString(v)->chars)),
                 0, +1);
    } else {
        notImplemented(in.op);
    }
}

void emitPop(Emitter& e, const DecodedInstruction& in) {
    PopKind kind = PopKind::TEMP;
    auto it = e.popKinds.find(in.offset);
    if (it != e.popKinds.end()) {
        kind = it->second;
    }
    if (kind == PopKind::TEMP) {
        e.b.emit("pop", 1, -1);
    }
    // LOCAL_RECLAIM: the slot lives in the local array, not on the
    // evaluation stack — nothing to pop.
}

void emitGetLocal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.slotForLocal(in.byteOperand)), 0, +1);
}

// See this file's own top-of-file note and `loadNamedLocalAtZeroDepth`'s:
// when the value SET_LOCAL peeks was already folded into a named local by the
// shared abstract-stack pass (`before[i].operandDepth() == 0`), nothing
// physically sits on the CIL stack to `dup` — the emitter for whichever
// earlier instruction produced that value already stored it straight into
// its own slot (see `finishInstruction`'s handling of
// `invisibleVarsByOffset`). Reload it explicitly instead, and store with no
// further copy: nothing downstream ever needs SET_LOCAL's own peeked
// result once it is folded this way (its sole reader is the local
// declaration that just consumed it), matching jvm_emitter.cpp's own
// `emitSetLocal` on this exact shape.
void emitSetLocal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                  bool& consumedFollowingPop) {
    int slot = e.slotForLocal(in.byteOperand);
    bool fuse = e.fusablePop(i);
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    } else if (fuse) {
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    } else {
        e.b.emit("dup", 1, +1);
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    }
    consumedFollowingPop = fuse;
}

void emitDefineGlobal(Emitter& e, const DecodedInstruction& in) {
    emitGlobalsCall(e, "Define", e.globalNameLiteral(in.constantIndex),
                    /*peek=*/false);
}

void emitGetGlobal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), 0, +1);
    e.b.emit("ldstr " + e.globalNameLiteral(in.constantIndex), 0, +1);
    e.b.emit("call instance object [LoxRuntime]Lox.LoxGlobals::Get(string)", 2,
             -1);
}

// Same eager-fold shape as `emitSetLocal`'s own note, for a global
// initializer instead of a local one (probe 19).
void emitSetGlobal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                   bool& consumedFollowingPop) {
    bool fuse = e.fusablePop(i);
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        emitGlobalsCall(e, "Set", e.globalNameLiteral(in.constantIndex),
                        /*peek=*/false);
    } else {
        emitGlobalsCall(e, "Set", e.globalNameLiteral(in.constantIndex),
                        /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
}

void emitPrint(Emitter& e) {
    e.b.emit("call void [LoxRuntime]Lox.LoxOps::Print(object)", 1, -1);
}

void emitNot(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::Not(object)", 1, 0);
}

void emitNegate(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::Negate(object)", 1, 0);
}

void emitBinaryOp(Emitter& e, const char* method) {
    e.b.emit(std::string("call object [LoxRuntime]Lox.LoxOps::") + method +
                 "(object, object)",
             2, -1);
}

void emitComparisonOp(Emitter& e, const char* method) {
    e.b.emit(std::string("call bool [LoxRuntime]Lox.LoxOps::") + method +
                 "(object, object)",
             2, -1);
    e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
}

// JUMP and LOOP share the same lowering: an unconditional `br` carries no
// operand budget of its own, so nothing distinguishes a forward skip from a
// loop's back edge once the CFG pass has resolved both to a label.
void emitJumpOrLoop(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("br " + e.labelFor(in.jumpTarget), 0, 0);
}

// P2/P3: JUMP_IF_FALSE peeks — the condition must still be present, on
// *both* outgoing edges, for whatever follows to see (03_and_or keeps it as
// the short-circuit result; 02/04/05 discard it with their own, ordinary
// POP right after). `dup` supplies that second, independent copy so the
// taken edge does not lose its copy to `IsFalsy`'s own read.
//
// `before[i].operandDepth() == 0` is the same eager invisible-var
// materialization `emitSetLocal`/`emitSetGlobal` handle (a local
// initializer whose top-level operator is `and`/`or`, probe 23) — the
// condition is not on the CIL evaluation stack to `dup`, because the
// shared abstract-stack analysis already folded it into a named local.
// `loadNamedLocalAtZeroDepth` names and loads a fresh copy; `IsFalsy`/
// `brtrue` still only consume that one copy, so the depth-preserving
// contract holds on both edges (0 in, 0 out) exactly as the dup path holds
// it at (D, D) for D > 0.
void emitJumpIfFalse(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    } else {
        e.b.emit("dup", 1, +1);
    }
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::IsFalsy(object)", 1, 0);
    e.b.emit("brtrue " + e.labelFor(in.jumpTarget), 1, -1);
}

// The script's own `RETURN`: `Compiler::endCompiler()` always appends a
// trailing NIL before it, so this instruction's own operand is always a
// genuine evaluation-stack temporary here — never a name-folded local (see
// this file's own top-of-file note) — and `Main` is `void`, so ECMA-335's
// own `ret` rule (III.3.40: the stack must hold only the value being
// returned, none for `void`) needs that value discarded first. The
// explicit depth check (rather than folding it into `emit`'s own
// `cellsRead`) is deliberate: `ret` does not merely need N cells present,
// it needs the stack completely empty, a stronger condition `cellsRead`
// alone cannot express.
void emitReturn(Emitter& e) {
    e.b.emit("pop", 1, -1);
    if (e.b.depth != 0) {
        throw std::runtime_error(
            "clr_emitter: evaluation stack must be empty before 'ret' from "
            "a void method, got depth " +
            std::to_string(e.b.depth));
    }
    e.b.emit("ret", 0, 0);
}

// The part of one instruction's handling that is not the opcode's own
// concern: storing any invisible-var slot this offset declares (P1 — the
// value already sits where the store above just left it; nothing about
// this is opcode-specific), updating the forward-walk tracker
// `resolveZeroDepthLocalSlot` cross-checks (this file's own top-of-file
// note), then computing the next walk index and the fall-through carry the
// *next* iteration's label-resync test (see emitBody) reads.
std::size_t finishInstruction(Emitter& e, const DecodedInstruction& in,
                              std::size_t i, bool consumedFollowingPop) {
    auto varsIt = e.invisibleVarsByOffset.find(in.offset);
    if (varsIt != e.invisibleVarsByOffset.end()) {
        for (int slot : varsIt->second) {
            e.b.emit("stloc " + std::to_string(e.slotForLocal(slot)), 1, -1);
            e.lastInvisibleVarSlot = slot;
        }
    }

    std::size_t nextIndex = i + (consumedFollowingPop ? 2 : 1);
    e.prevCanFallThrough =
        in.op != Op::JUMP && in.op != Op::LOOP && in.op != Op::RETURN;
    e.prevNaturalSuccessorOffset = (nextIndex < e.fn.instructions.size())
                                       ? e.fn.instructions[nextIndex].offset
                                       : -1;
    return nextIndex;
}

void emitBody(Emitter& e) {
    const std::vector<DecodedInstruction>& ins = e.fn.instructions;
    std::size_t n = ins.size();

    for (std::size_t i = 0; i < n;) {
        if (!e.reached(i)) {
            i++;
            continue; // endCompiler()'s trailing NIL;RETURN can be dead code.
        }
        const DecodedInstruction& in = ins[i];
        bool consumedFollowingPop = false;

        auto labelIt = e.labelAtOffset.find(in.offset);
        if (labelIt != e.labelAtOffset.end()) {
            e.b.label(labelIt->second);
            bool trustCarryForward = e.prevCanFallThrough &&
                                     e.prevNaturalSuccessorOffset == in.offset;
            if (!trustCarryForward) {
                // A block leader with no live fall-through predecessor: the
                // array position right before it is a JUMP/LOOP/RETURN
                // whose real successor is somewhere else entirely, or dead
                // code that never executed, so this pass's own
                // carry-forward is not this block's entry depth at all.
                // The shared abstract-stack analysis already proved
                // operandDepth() exact at every merge, so resync to it.
                e.b.resync(e.analysis.before[i].operandDepth());
            }
        }

        // Safety net: every correctly-lowered opcode in this pass keeps the
        // CIL evaluation stack's physical depth equal to the shared
        // abstract-stack analysis's own operandDepth() at the same offset.
        // A mismatch means a peek is about to `dup`/reload a cell that is
        // not physically there — abort loudly instead of letting ilasm or
        // the CLR runtime find it.
        if (e.b.depth != e.analysis.before[i].operandDepth()) {
            throw std::runtime_error(
                "clr_emitter: simulated stack depth " +
                std::to_string(e.b.depth) + " disagrees with analysis depth " +
                std::to_string(e.analysis.before[i].operandDepth()) +
                " at offset " + std::to_string(in.offset));
        }

        switch (in.op) {
        case Op::CONSTANT:
            emitConstant(e, in);
            break;
        case Op::NIL:
            e.b.emit("ldnull", 0, +1);
            break;
        case Op::TRUE:
            e.b.emit("ldc.i4.1", 0, +1);
            e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
            break;
        case Op::FALSE:
            e.b.emit("ldc.i4.0", 0, +1);
            e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
            break;
        case Op::POP:
            emitPop(e, in);
            break;
        case Op::GET_LOCAL:
            emitGetLocal(e, in);
            break;
        case Op::SET_LOCAL:
            emitSetLocal(e, i, in, consumedFollowingPop);
            break;
        case Op::DEFINE_GLOBAL:
            emitDefineGlobal(e, in);
            break;
        case Op::GET_GLOBAL:
            emitGetGlobal(e, in);
            break;
        case Op::SET_GLOBAL:
            emitSetGlobal(e, i, in, consumedFollowingPop);
            break;
        case Op::PRINT:
            emitPrint(e);
            break;
        case Op::NOT:
            emitNot(e);
            break;
        case Op::NEGATE:
            emitNegate(e);
            break;
        case Op::ADD:
            emitBinaryOp(e, "Add");
            break;
        case Op::SUBTRACT:
            emitBinaryOp(e, "Subtract");
            break;
        case Op::MULTIPLY:
            emitBinaryOp(e, "Multiply");
            break;
        case Op::DIVIDE:
            emitBinaryOp(e, "Divide");
            break;
        case Op::MODULO:
            emitBinaryOp(e, "Modulo");
            break;
        case Op::EQUAL:
            emitComparisonOp(e, "Equal");
            break;
        case Op::GREATER:
            emitComparisonOp(e, "Greater");
            break;
        case Op::LESS:
            emitComparisonOp(e, "Less");
            break;
        case Op::JUMP:
        case Op::LOOP:
            emitJumpOrLoop(e, in);
            break;
        case Op::JUMP_IF_FALSE:
            emitJumpIfFalse(e, i, in);
            break;
        case Op::RETURN:
            emitReturn(e);
            break;
        default:
            notImplemented(in.op);
        }

        i = finishInstruction(e, in, i, consumedFollowingPop);
    }
}

// High-water mark of concurrently-bound Lox frame slots (arity+1 at entry,
// growing with every `var`) — mirrors jvm_emitter.cpp's own
// `computeMaxLocalCount`; the shared abstract-stack analysis, not a private
// count, is the source for both.
int computeMaxLocalCount(const FunctionStackAnalysis& analysis) {
    int maxLocalCount = 0;
    for (std::size_t i = 0; i < analysis.before.size(); i++) {
        if (i >= analysis.reached.size() ||
            !static_cast<bool>(analysis.reached[i])) {
            continue;
        }
        maxLocalCount = std::max({maxLocalCount, analysis.before[i].localCount,
                                  analysis.after[i].localCount});
    }
    // slot 0: the script's own never-read callee, always live.
    return std::max(maxLocalCount, 1);
}

Emitter buildEmitter(const DecodedFunction& fn,
                     const FunctionStackAnalysis& analysis, int maxLocalCount) {
    Emitter e{fn, analysis, {}};
    e.scratchSlot = e.baseSlot + maxLocalCount;

    for (const PopClassification& p : analysis.pops) {
        e.popKinds[p.offset] = p.kind;
    }
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        e.invisibleVarsByOffset[site.offset].push_back(site.slot);
    }

    // One ilasm label per CFG block leader. A leader with no predecessor
    // (e.g. the fall-through after an unconditional JUMP) still gets a
    // label; an unreferenced ilasm label is harmless, so this pass does not
    // bother filtering to only-referenced offsets.
    Cfg cfg = buildCfg(fn.instructions);
    e.labelAtOffset.reserve(cfg.blocks.size());
    for (const BasicBlock& block : cfg.blocks) {
        e.labelAtOffset.emplace(block.leaderOffset, block.label);
    }
    return e;
}

void emitPrologue(Emitter& e) {
    e.b.emit("call class [LoxRuntime]Lox.LoxGlobals "
             "[LoxRuntime]Lox.LoxRuntime::Init()",
             0, +1);
    e.b.emit("stloc " + std::to_string(e.globalsSlot), 1, -1);
}

std::string assembleClass(const Emitter& e, const std::string& className,
                          int totalLocals) {
    std::ostringstream out;
    out << ".assembly extern System.Runtime { .ver 8:0:0:0 }\n";
    out << ".assembly extern LoxRuntime {}\n";
    out << ".assembly " << className << " {}\n";
    out << ".module " << className << ".dll\n\n";
    out << ".class public auto ansi " << className
        << " extends [System.Runtime]System.Object\n{\n";
    out << "  .method public static void Main() cil managed\n  {\n";
    out << "    .entrypoint\n";
    out << "    .maxstack " << std::max(1, e.b.maxDepth) << "\n";
    out << "    .locals init (";
    for (int i = 0; i < totalLocals; i++) {
        if (i > 0) {
            out << ", ";
        }
        out << "object";
    }
    out << ")\n\n";
    out << e.b.text.str();
    out << "  }\n";
    out << "}\n";
    return out.str();
}

} // namespace

std::string ilasmStringLiteral(const std::string& raw) {
    std::ostringstream out;
    out << "bytearray (";
    for (std::size_t i = 0; i < raw.size(); i++) {
        if (i > 0) {
            out << ' ';
        }
        std::array<char, 3> buf{};
        std::snprintf(buf.data(), buf.size(), "%02x",
                      static_cast<unsigned char>(raw[i]));
        out << buf.data() << " 00";
    }
    out << ')';
    return out.str();
}

std::string ilasmDoubleLiteral(double value) {
    auto bits = std::bit_cast<uint64_t>(value);
    std::ostringstream out;
    out << '(';
    for (int i = 0; i < 8; i++) {
        if (i > 0) {
            out << ' ';
        }
        std::array<char, 3> buf{};
        std::snprintf(buf.data(), buf.size(), "%02x",
                      static_cast<unsigned>((bits >> (8 * i)) & 0xff));
        out << buf.data();
    }
    out << ')';
    return out.str();
}

std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className) {
    int maxLocalCount = computeMaxLocalCount(analysis);
    Emitter e = buildEmitter(fn, analysis, maxLocalCount);
    emitPrologue(e);
    emitBody(e);
    // globals (1) + the Lox frame's own slots + the shuffle scratch (1).
    int totalLocals = 1 + maxLocalCount + 1;
    return assembleClass(e, className, totalLocals);
}

} // namespace clr
