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
#include <vector>

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

// CLOSURE itself is implemented, but only for the zero-upvalue
// construction (this file's own top-of-file note): wiring a captured cell
// into the generated constructor is a later node's job. Kept distinct from
// notImplemented(Op) so the message names the actual gap instead of the
// whole opcode.
[[noreturn]] void notImplementedClosureUpvalues(std::size_t count) {
    throw std::runtime_error(
        "clr_emitter: the CLR backend does not lower CLOSURE with " +
        std::to_string(count) +
        " upvalue(s) yet (upvalue wiring is a later node)");
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

// Smallest-encoding int push. Reused for CALL's array-size/index operands,
// a generated constructor's own literal arity, and the argument-prologue
// unpacking loop; never asked for a value outside [-1, 255] here (CALL
// argCount and ObjFunction::arity both fit a byte — compiler.cpp enforces
// each ceiling independently).
std::string pushIntInstruction(int n) {
    if (n == -1) {
        return "ldc.i4.m1";
    }
    if (n >= 0 && n <= 8) {
        return "ldc.i4." + std::to_string(n);
    }
    if (n >= -128 && n <= 127) {
        return "ldc.i4.s " + std::to_string(n);
    }
    return "ldc.i4 " + std::to_string(n);
}

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
    // that function's own note. This mapping is the SAME for a script
    // chunk and a function chunk (clr_emitter.h's own top-of-file note):
    // unlike the JVM local-variable array, CIL keeps `self`/`args` in their
    // own argument slots, so the Lox frame never has to share space with
    // them.
    int baseSlot{1};
    int globalsSlot{0};
    int scratchSlot{0};

    // Set (to scratchSlot+1 / scratchSlot+2) only when this chunk contains
    // a CALL with at least one argument; -1 otherwise, so a stray use
    // before buildEmitter's own computation fails loudly instead of
    // silently aliasing scratchSlot.
    int calleeScratchSlot{-1};
    int argScratchBase{-1};

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
    // the very start, and set false whenever that instruction was a
    // JUMP/LOOP/RETURN (none of those fall through). A dead-code gap
    // between two live instructions is not closed by resetting this flag;
    // it is closed by `prevNaturalSuccessorOffset` holding the skipped
    // instruction's array successor offset, which then fails to match the
    // next LIVE instruction's offset, so the label-resync test in
    // `emitBody` still refuses to trust the carry-forward across the gap.
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

// The one test every peek-family consumer below (SET_LOCAL, SET_GLOBAL,
// JUMP_IF_FALSE, RETURN, and any later one that peeks or returns a value
// the abstract-stack pass may have folded) must run before it decides
// between `loadNamedLocalAtZeroDepth` and its own ordinary
// stack-value path. Centralized so a future consumer calls this instead
// of re-deriving the raw `operandDepth() == 0` expression inline — the
// resolution it guards (`resolveZeroDepthLocalSlot`) is already the one
// shared authority; this is the one shared guard in front of it.
bool isFoldedAtZeroDepth(const Emitter& e, std::size_t i) {
    return e.analysis.before[i].operandDepth() == 0;
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
    if (isFoldedAtZeroDepth(e, i)) {
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
    if (isFoldedAtZeroDepth(e, i)) {
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
    if (isFoldedAtZeroDepth(e, i)) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    } else {
        e.b.emit("dup", 1, +1);
    }
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::IsFalsy(object)", 1, 0);
    e.b.emit("brtrue " + e.labelFor(in.jumpTarget), 1, -1);
}

// P5 (calling convention): CALL argCount finds [callee, arg0, ...,
// arg(argCount-1)] already loose on the evaluation stack (arg(argCount-1)
// on top; vm.cpp's own bottom-to-top push order) and must hand
// LoxOps.Call one object[]. argCount == 0 needs no reshaping at all — the
// callee is already the sole, topmost value, so the empty array builds
// directly on top of it. argCount >= 1 spills every value to a scratch
// local first: the elements are already on the stack BELOW where a fresh
// array reference would land, so a dup-based build cannot reach them.
// buildEmitter reserves one scratch slot per argument the chunk's widest
// CALL needs, plus one for the callee, computed once before this pass
// starts — reused across every CALL site in the chunk, because calls run
// one at a time, never concurrently.
void emitCall(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    const char* callSig =
        "call object [LoxRuntime]Lox.LoxOps::Call(object, object[])";
    if (argCount == 0) {
        e.b.emit(pushIntInstruction(0), 0, +1);
        e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
        e.b.emit(callSig, 2, -1);
        return;
    }
    for (int i = argCount - 1; i >= 0; i--) {
        e.b.emit("stloc " + std::to_string(e.argScratchBase + i), 1, -1);
    }
    e.b.emit("stloc " + std::to_string(e.calleeScratchSlot), 1, -1);

    e.b.emit("ldloc " + std::to_string(e.calleeScratchSlot), 0, +1);
    e.b.emit(pushIntInstruction(argCount), 0, +1);
    e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
    for (int i = 0; i < argCount; i++) {
        e.b.emit("dup", 1, +1);
        e.b.emit(pushIntInstruction(i), 0, +1);
        e.b.emit("ldloc " + std::to_string(e.argScratchBase + i), 0, +1);
        e.b.emit("stelem.ref", 3, -3);
    }
    e.b.emit(callSig, 2, -1);
}

// CLOSURE with zero upvalues (this file's own top-of-file note; a captured
// upvalue is a later node's wiring). `childClassNames[i]` names the class
// this chunk's own i-th nested function (chunk_decoder.h:
// DecodedInstruction::nestedIndex) was assigned by emitProgram's pre-order
// walk. emitScript (no nested functions in any caller) always passes an
// empty vector, so a CLOSURE reaching this from there is a real bug,
// caught below rather than silently mis-indexed.
//
// `newobj` builds the whole `object[][]` (always empty here) and calls the
// generated class's own constructor in one instruction — unlike the JVM
// backend's `new; dup; ...; invokespecial <init>` idiom, CIL's `newobj`
// pops its constructor arguments and pushes the fresh reference itself, so
// no `dup` is needed to keep a copy of the still-uninitialized object
// around.
void emitClosure(Emitter& e, const DecodedInstruction& in,
                 const std::vector<std::string>& childClassNames) {
    if (!in.upvalues.empty()) {
        notImplementedClosureUpvalues(in.upvalues.size());
    }
    if (in.nestedIndex < 0 ||
        static_cast<std::size_t>(in.nestedIndex) >= childClassNames.size()) {
        throw std::runtime_error("clr_emitter: CLOSURE nestedIndex " +
                                 std::to_string(in.nestedIndex) +
                                 " has no assigned class name");
    }
    const std::string& cls =
        childClassNames[static_cast<std::size_t>(in.nestedIndex)];
    e.b.emit(pushIntInstruction(0), 0, +1);
    e.b.emit("newarr object[]", 1, 0);
    e.b.emit("newobj instance void " + cls + "::.ctor(object[][])", 1, 0);
}

// RETURN's two roles (P5). The script's own RETURN: `Compiler::
// endCompiler()` always appends a trailing NIL before it, so this
// instruction's own operand is always a genuine evaluation-stack temporary
// here — never a name-folded local (this file's own top-of-file note) —
// and `Main` is `void`, so ECMA-335's own `ret` rule (III.3.40: the stack
// must hold only the value being returned, none for `void`) needs that
// value discarded first. The explicit depth check (rather than folding it
// into `emit`'s own `cellsRead`) is deliberate: `ret` does not merely need
// N cells present, it needs the stack completely empty, a stronger
// condition `cellsRead` alone cannot express.
//
// A function's own RETURN hands its value back through `Invoke`'s own
// non-`void` return type instead — `ret` there needs the stack to hold
// EXACTLY the one value being returned, the mirror image of the script's
// "exactly empty" rule. That value is not always a genuine evaluation-stack
// temporary: `bytecode-translation-problems.md` documents 33 corpus sites
// where the shared abstract-stack analysis already folded the returned
// value into a named local before RETURN runs (`before[i].operandDepth()
// == 0`), with no separate load in the bytecode — the same eager
// invisible-var materialization SET_LOCAL/SET_GLOBAL/JUMP_IF_FALSE already
// handle. This node's own checkpoint never reaches that shape (its `return
// a + b;` is an ordinary temporary), but the check here does not assume
// the stack case just because it is the only one exercised end to end yet.
void emitReturn(Emitter& e, std::size_t i, const DecodedInstruction& in,
                bool isFunction) {
    if (!isFunction) {
        e.b.emit("pop", 1, -1);
        if (e.b.depth != 0) {
            throw std::runtime_error(
                "clr_emitter: evaluation stack must be empty before 'ret' "
                "from a void method, got depth " +
                std::to_string(e.b.depth));
        }
        e.b.emit("ret", 0, 0);
        return;
    }
    if (isFoldedAtZeroDepth(e, i)) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    }
    if (e.b.depth != 1) {
        throw std::runtime_error(
            "clr_emitter: evaluation stack must hold exactly the return "
            "value before 'ret' from a function, got depth " +
            std::to_string(e.b.depth));
    }
    e.b.emit("ret", 1, -1);
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

void emitBody(Emitter& e, bool isFunction,
              const std::vector<std::string>& childClassNames) {
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
        case Op::CALL:
            emitCall(e, in);
            break;
        case Op::CLOSURE:
            emitClosure(e, in, childClassNames);
            break;
        case Op::RETURN:
            emitReturn(e, i, in, isFunction);
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
    // slot 0: the callee/receiver, never named by itself, but always live.
    return std::max(maxLocalCount, 1);
}

// The widest aggregate this chunk builds by spilling loose operand-stack
// values to scratch locals before assembling one aggregate object —
// CALL's own argument count today. A later opcode with the same
// N-loose-values-then-one-aggregate shape (BUILD_LIST, BUILD_MAP) widens
// this same std::max scan instead of opening a second, parallel
// scratch-slot area — jvm_emitter.cpp's own computeMaxSpillWidth is the
// JVM twin of this rule. Ignores argCount == 0 (needs no scratch slot —
// see emitCall); 0 here means the chunk needs none at all, keeping
// `.locals init` byte-identical to pre-this-node output on every chunk
// that builds no aggregate.
int computeMaxAggregateWidth(const DecodedFunction& fn) {
    int maxWidth = 0;
    for (const DecodedInstruction& instr : fn.instructions) {
        if (instr.op == Op::CALL) {
            maxWidth = std::max(maxWidth, instr.byteOperand);
        }
    }
    return maxWidth;
}

Emitter buildEmitter(const DecodedFunction& fn,
                     const FunctionStackAnalysis& analysis, int maxLocalCount,
                     int maxAggregateWidth) {
    Emitter e{fn, analysis, {}};
    e.scratchSlot = e.baseSlot + maxLocalCount;
    if (maxAggregateWidth > 0) {
        e.calleeScratchSlot = e.scratchSlot + 1;
        e.argScratchBase = e.scratchSlot + 2;
    }

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

// The globals reference and, for a function chunk only, the argument
// prologue (P5): `Invoke`'s own CIL argument slots are `self` (arg 1,
// `ldarg.1`) and `args` (arg 2, an object[], `ldarg.2`). Copies `self` into
// the Lox frame's own slot-0 mirror and unpacks `args[i]` into slot
// `i + 1`'s — the fixed mapping every opcode-family function above
// assumes. A script chunk's own prologue instead forwards `Main`'s own
// `string[]` argument (arg 0) to `LoxRuntime.SetProgramArgs` before
// `Init()` runs, so the native `args()` global answers the program's real
// command-line arguments once CALL makes it reachable, the same as the
// JVM backend's own script prologue.
//
// A generated class has no field of its own for the shared LoxGlobals
// instance (clr_emitter.h's own top-of-file note: the Lox frame mapping is
// identical for both roles, and adding a per-instance field would not be),
// so a function chunk reads the one instance the script's own `Init()`
// call already built, through `LoxRuntime.Current()`.
void emitPrologue(Emitter& e, const DecodedFunction& fn, bool isFunction) {
    if (!isFunction) {
        e.b.emit("ldarg.0", 0, +1);
        e.b.emit("call void [LoxRuntime]Lox.LoxRuntime::SetProgramArgs"
                 "(string[])",
                 1, -1);
        e.b.emit("call class [LoxRuntime]Lox.LoxGlobals "
                 "[LoxRuntime]Lox.LoxRuntime::Init()",
                 0, +1);
        e.b.emit("stloc " + std::to_string(e.globalsSlot), 1, -1);
        return;
    }
    e.b.emit("call class [LoxRuntime]Lox.LoxGlobals "
             "[LoxRuntime]Lox.LoxRuntime::Current()",
             0, +1);
    e.b.emit("stloc " + std::to_string(e.globalsSlot), 1, -1);
    e.b.emit("ldarg.1", 0, +1);
    e.b.emit("stloc " + std::to_string(e.slotForLocal(0)), 1, -1);
    int arity = fn.function->arity;
    for (int i = 0; i < arity; i++) {
        e.b.emit("ldarg.2", 0, +1);
        e.b.emit(pushIntInstruction(i), 0, +1);
        e.b.emit("ldelem.ref", 2, -1);
        e.b.emit("stloc " + std::to_string(e.slotForLocal(i + 1)), 1, -1);
    }
}

// The constructor every generated function class needs (clr_emitter.h's
// own top-of-file note): calls straight through to LoxClosure's own
// constructor with this function's compile-time name/arity as literals,
// so only the upvalues array is a real parameter — always empty from this
// pass's own construction (emitClosure); a later node fills it with real
// cells without changing this shape.
std::string emitConstructorMethod(const DecodedFunction& fn) {
    std::ostringstream out;
    out << "  .method public specialname rtspecialname instance void "
           ".ctor(object[][] upvalues) cil managed\n";
    out << "  {\n";
    out << "    .maxstack 4\n";
    out << "    ldarg.0\n";
    if (fn.function->name != nullptr) {
        out << "    ldstr "
            << ilasmStringLiteral(std::string(fn.function->name->chars))
            << "\n";
    } else {
        out << "    ldnull\n";
    }
    out << "    " << pushIntInstruction(fn.function->arity) << "\n";
    out << "    ldarg.1\n";
    out << "    call instance void "
           "[LoxRuntime]Lox.LoxClosure::.ctor(string, int32, object[][])\n";
    out << "    ret\n";
    out << "  }\n\n";
    return out.str();
}

// The four header directives one whole program shares — the externs this
// pass's own opcode set touches, plus the one assembly manifest and module
// every class in the program lives in. Written once per program
// (emitProgram), not once per class: CoreCLR has no single mscorlib
// umbrella (notes/backend-implementation-dag.md), but a single module can
// hold more than one class without each one repeating its own manifest.
std::string emitHeader(const std::string& moduleClassName) {
    std::ostringstream out;
    out << ".assembly extern System.Runtime { .ver 8:0:0:0 }\n";
    out << ".assembly extern LoxRuntime {}\n";
    out << ".assembly " << moduleClassName << " {}\n";
    out << ".module " << moduleClassName << ".dll\n\n";
    return out.str();
}

// The class header and the method(s) this chunk becomes, plus the
// `.maxstack`/`.locals init` directives measured from what emitBody
// actually produced. A script chunk becomes a static `Main(string[] args)`
// with `.entrypoint` — the parameter is CoreCLR's own standard entry-point
// shape, and `dotnet` binds the process's own command-line arguments to it
// with no extra wiring; a function chunk becomes a class extending
// [LoxRuntime]Lox.LoxClosure, with the constructor every such class needs
// plus the `Invoke` override that holds this chunk's own lowered body.
std::string emitClassBody(const Emitter& e, const DecodedFunction& fn,
                          const std::string& className, bool isFunction,
                          int totalLocals) {
    std::ostringstream out;
    out << ".class public auto ansi " << className;
    if (isFunction) {
        out << " extends [LoxRuntime]Lox.LoxClosure\n{\n";
        out << emitConstructorMethod(fn);
        out << "  .method family virtual instance object "
               "Invoke(object self, object[] args) cil managed\n  {\n";
    } else {
        out << " extends [System.Runtime]System.Object\n{\n";
        out << "  .method public static void Main(string[] args) cil "
               "managed\n  {\n";
        out << "    .entrypoint\n";
    }
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

// The shared lowering pass for one chunk, script or function alike:
// clr_emitter.h's own top-of-file note explains why the Lox-frame slot
// mapping needs no isFunction split here, unlike the JVM backend's own
// analogous pass. `childClassNames[i]` names the class this chunk's own
// i-th nested function was assigned — see emitProgram.
std::string emitChunk(const DecodedFunction& fn,
                      const FunctionStackAnalysis& analysis,
                      const std::string& className, bool isFunction,
                      const std::vector<std::string>& childClassNames) {
    int maxLocalCount = computeMaxLocalCount(analysis);
    int maxAggregateWidth = computeMaxAggregateWidth(fn);
    int extraSpillSlots = maxAggregateWidth > 0 ? maxAggregateWidth + 1 : 0;

    Emitter e = buildEmitter(fn, analysis, maxLocalCount, maxAggregateWidth);
    emitPrologue(e, fn, isFunction);
    emitBody(e, isFunction, childClassNames);

    // globals (1) + the Lox frame's own slots + the shuffle scratch (1) +
    // the aggregate spill area, if this chunk builds one.
    int totalLocals = 1 + maxLocalCount + 1 + extraSpillSlots;
    return emitClassBody(e, fn, className, isFunction, totalLocals);
}

// Assigns every node in the decoded tree a stable class name, by one fixed
// pre-order walk (clr_emitter.h: "deterministic naming"): the root becomes
// `scriptClassName`, and every other node becomes `LoxFn$<n>` in visit
// order, counted across the WHOLE tree, not per parent — two sibling
// functions and a great-grandchild all draw from the same counter. Keyed
// by DecodedFunction::id (stable, name-independent) rather than a
// pointer, so the two passes below (naming, then emission) can each walk
// the tree their own way without having to agree on traversal order.
void assignClassNames(const DecodedFunction& fn, bool isRoot,
                      const std::string& scriptClassName, int& counter,
                      std::unordered_map<std::string, std::string>& names) {
    names[fn.id] =
        isRoot ? scriptClassName : ("LoxFn$" + std::to_string(counter++));
    for (const DecodedFunction& child : fn.nested) {
        assignClassNames(child, /*isRoot=*/false, scriptClassName, counter,
                         names);
    }
}

void emitAll(const DecodedFunction& fn, const StackAnalysisTree& node,
             bool isRoot,
             const std::unordered_map<std::string, std::string>& names,
             std::ostringstream& out) {
    // decodeFunctionTree (fn.nested) and analyzeStackTree (node.nested) are
    // two independently-walked passes over the same ObjFunction tree. They
    // agree today because both use one fixed traversal order, but nothing
    // enforces that at the type level — an unchecked node.nested[i] below
    // would read out of range with no diagnostic the moment they ever
    // disagreed, instead of failing loudly like every other consumer in
    // this file.
    if (fn.nested.size() != node.nested.size()) {
        throw std::runtime_error(
            "clr_emitter: decoded function tree and stack analysis tree "
            "disagree on child count (" +
            std::to_string(fn.nested.size()) + " vs " +
            std::to_string(node.nested.size()) + ")");
    }

    std::vector<std::string> childClassNames;
    childClassNames.reserve(fn.nested.size());
    for (const DecodedFunction& child : fn.nested) {
        childClassNames.push_back(names.at(child.id));
    }
    const std::string& className = names.at(fn.id);
    out << emitChunk(fn, node.self, className, /*isFunction=*/!isRoot,
                     childClassNames);

    for (std::size_t i = 0; i < fn.nested.size(); i++) {
        emitAll(fn.nested[i], node.nested[i], /*isRoot=*/false, names, out);
    }
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
    return emitHeader(className) +
           emitChunk(fn, analysis, className, /*isFunction=*/false, {});
}

std::string emitProgram(const DecodedFunction& root,
                        const StackAnalysisTree& tree,
                        const std::string& scriptClassName) {
    std::unordered_map<std::string, std::string> names;
    int counter = 0;
    assignClassNames(root, /*isRoot=*/true, scriptClassName, counter, names);

    std::ostringstream out;
    out << emitHeader(scriptClassName);
    emitAll(root, tree, /*isRoot=*/true, names, out);
    return out.str();
}

} // namespace clr
