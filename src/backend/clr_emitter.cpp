#include "clr_emitter.h"

#include "exec_objects.h"
#include "object.h"
#include "value.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
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

    void emit(const std::string& instruction, int slotDelta) {
        text << "    " << instruction << "\n";
        depth += slotDelta;
        if (depth < 0) {
            throw std::runtime_error(
                "clr_emitter: evaluation stack underflow emitting '" +
                instruction + "'");
        }
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

    // Local index 0 holds the LoxGlobals instance; Lox frame slot `n` maps
    // to local index `baseSlot + n`; `scratchSlot` is one shared shuffle
    // slot for the SET_GLOBAL/DEFINE_GLOBAL P2 shuffle (globalsCall) — see
    // that function's own note. No calleeScratchSlot/argScratchBase: this
    // node lowers no CALL, BUILD_LIST, or BUILD_MAP.
    int baseSlot{1};
    int globalsSlot{0};
    int scratchSlot{0};

    [[nodiscard]] int slotForLocal(int loxSlot) const {
        return baseSlot + loxSlot;
    }

    [[nodiscard]] bool reached(std::size_t idx) const {
        return idx < analysis.reached.size() &&
               static_cast<bool>(analysis.reached[idx]);
    }

    // SET_LOCAL/SET_GLOBAL peek (the value stays on the stack); a following
    // POP the shared abstract-stack analysis already proved TEMP is exactly
    // that peeked value being discarded as a statement result, so folding
    // the pair into one plain store needs no dup and no separate pop
    // (bytecode-translation-problems.md P2). No CFG-label guard is needed
    // here, unlike jvm_emitter.cpp's own `fusablePop`: this node emits no
    // jump target and so no label ever lands on the following POP's offset.
    [[nodiscard]] bool fusablePop(std::size_t i) const {
        std::size_t j = i + 1;
        if (j >= fn.instructions.size() || !reached(j) ||
            fn.instructions[j].op != Op::POP) {
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
    e.b.emit("stloc " + scratch, -1);
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), +1);
    e.b.emit("ldstr " + nameLiteral, +1);
    e.b.emit("ldloc " + scratch, +1);
    e.b.emit(std::string("call instance void [LoxRuntime]Lox.LoxGlobals::") +
                 method + "(string, object)",
             -3);
    if (peek) {
        e.b.emit("ldloc " + scratch, +1);
    }
}

// `analysis.before[i].localCount - 1` names the topmost live local exactly,
// with no cross-check needed: unlike jvm_emitter.cpp's
// `loadNamedLocalAtZeroDepth`, this pass never places a CFG label (no
// jumps in this node's scope), so `localCount - 1` is never merely an
// upper bound here — see this file's own top-of-file note.
int topLiveLocalSlot(const Emitter& e, std::size_t i) {
    return e.slotForLocal(e.analysis.before[i].localCount - 1);
}

void emitConstant(Emitter& e, const DecodedInstruction& in) {
    Value v = e.fn.function->chunk.getConstant(
        static_cast<uint16_t>(in.constantIndex));
    if (is<Number>(v)) {
        e.b.emit("ldc.r8 " + ilasmDoubleLiteral(as<Number>(v)), +1);
        e.b.emit("box [System.Runtime]System.Double", 0);
    } else if (isString(v)) {
        e.b.emit("ldstr " +
                     ilasmStringLiteral(std::string(asObjString(v)->chars)),
                 +1);
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
        e.b.emit("pop", -1);
    }
    // LOCAL_RECLAIM: the slot lives in the local array, not on the
    // evaluation stack — nothing to pop.
}

void emitGetLocal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.slotForLocal(in.byteOperand)), +1);
}

// See this file's own top-of-file note and `topLiveLocalSlot`'s: when the
// value SET_LOCAL peeks was already folded into a named local by the
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
        e.b.emit("ldloc " + std::to_string(topLiveLocalSlot(e, i)), +1);
        e.b.emit("stloc " + std::to_string(slot), -1);
    } else if (fuse) {
        e.b.emit("stloc " + std::to_string(slot), -1);
    } else {
        e.b.emit("dup", +1);
        e.b.emit("stloc " + std::to_string(slot), -1);
    }
    consumedFollowingPop = fuse;
}

void emitDefineGlobal(Emitter& e, const DecodedInstruction& in) {
    emitGlobalsCall(e, "Define", e.globalNameLiteral(in.constantIndex),
                    /*peek=*/false);
}

void emitGetGlobal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), +1);
    e.b.emit("ldstr " + e.globalNameLiteral(in.constantIndex), +1);
    e.b.emit("call instance object [LoxRuntime]Lox.LoxGlobals::Get(string)",
             -1);
}

// Same eager-fold shape as `emitSetLocal`'s own note, for a global
// initializer instead of a local one (probe 19).
void emitSetGlobal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                   bool& consumedFollowingPop) {
    bool fuse = e.fusablePop(i);
    if (e.analysis.before[i].operandDepth() == 0) {
        e.b.emit("ldloc " + std::to_string(topLiveLocalSlot(e, i)), +1);
        emitGlobalsCall(e, "Set", e.globalNameLiteral(in.constantIndex),
                        /*peek=*/false);
    } else {
        emitGlobalsCall(e, "Set", e.globalNameLiteral(in.constantIndex),
                        /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
}

void emitPrint(Emitter& e) {
    e.b.emit("call void [LoxRuntime]Lox.LoxOps::Print(object)", -1);
}

void emitNot(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::Not(object)", 0);
}

void emitNegate(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::Negate(object)", 0);
}

void emitBinaryOp(Emitter& e, const char* method) {
    e.b.emit(std::string("call object [LoxRuntime]Lox.LoxOps::") + method +
                 "(object, object)",
             -1);
}

void emitComparisonOp(Emitter& e, const char* method) {
    e.b.emit(std::string("call bool [LoxRuntime]Lox.LoxOps::") + method +
                 "(object, object)",
             -1);
    e.b.emit("box [System.Runtime]System.Boolean", 0);
}

// The script's own `RETURN`: `Compiler::endCompiler()` always appends a
// trailing NIL before it, so this instruction's own operand is always a
// genuine evaluation-stack temporary here — never a name-folded local (see
// this file's own top-of-file note) — and `Main` is `void`, so ECMA-335's
// own `ret` rule (III.3.40: the stack must hold only the value being
// returned, none for `void`) needs that value discarded first.
void emitReturn(Emitter& e) {
    e.b.emit("pop", -1);
    e.b.emit("ret", 0);
}

// The part of one instruction's handling that is not the opcode's own
// concern: storing any invisible-var slot this offset declares (P1 — the
// value already sits where the store above just left it; nothing about
// this is opcode-specific), then computing the next walk index from
// whether this instruction fused away a following POP.
std::size_t finishInstruction(Emitter& e, const DecodedInstruction& in,
                              std::size_t i, bool consumedFollowingPop) {
    auto varsIt = e.invisibleVarsByOffset.find(in.offset);
    if (varsIt != e.invisibleVarsByOffset.end()) {
        for (int slot : varsIt->second) {
            e.b.emit("stloc " + std::to_string(e.slotForLocal(slot)), -1);
        }
    }
    return i + (consumedFollowingPop ? 2 : 1);
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
            e.b.emit("ldnull", +1);
            break;
        case Op::TRUE:
            e.b.emit("ldc.i4.1", +1);
            e.b.emit("box [System.Runtime]System.Boolean", 0);
            break;
        case Op::FALSE:
            e.b.emit("ldc.i4.0", +1);
            e.b.emit("box [System.Runtime]System.Boolean", 0);
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
    return e;
}

void emitPrologue(Emitter& e) {
    e.b.emit("call class [LoxRuntime]Lox.LoxGlobals "
             "[LoxRuntime]Lox.LoxRuntime::Init()",
             +1);
    e.b.emit("stloc " + std::to_string(e.globalsSlot), -1);
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
