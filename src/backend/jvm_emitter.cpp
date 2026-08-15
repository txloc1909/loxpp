#include "jvm_emitter.h"

#include "cfg.h"
#include "exec_objects.h"
#include "object.h"
#include "value.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace jvm {

namespace {

// Every Op enumerator's own spelling — for the "not implemented in N5:"
// message only. Not the disassembly oracle (test_chunk_decoder.cpp owns
// that); a name missing here still throws, just with "UNKNOWN_OP".
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
    throw std::runtime_error("not implemented in N5: " + opName(op));
}

// Accumulates Jasmin instruction text for one method body while tracking the
// JVM operand-stack depth in words (a double/long literal costs 2 before it
// is boxed down to 1 — see CONSTANT below), so `.limit stack` is measured
// directly against what this emitter actually produces instead of reused
// from N2's abstract-stack bound plus a guessed margin. N2's bound does not
// know which concrete shuffle (dup, or the scratch-slot peek below) N4
// chooses, so a direct simulation is the only number this node can trust.
struct Builder {
    std::ostringstream text;
    int depth{0};
    int maxDepth{0};

    void emit(const std::string& instruction, int wordDelta) {
        text << "    " << instruction << "\n";
        depth += wordDelta;
        if (depth < 0) {
            throw std::runtime_error(
                "jvm_emitter: operand stack underflow emitting '" +
                instruction + "'");
        }
        maxDepth = std::max(maxDepth, depth);
    }

    // A jasmin label marker. Unindented (jasmin convention) and zero stack
    // effect — a label is a name for an offset, not an instruction.
    void label(const std::string& name) { text << name << ":\n"; }

    // Re-anchors `depth` to a value this pass did not itself derive by
    // emitting instructions (N5: a CFG merge's entry depth, trusted from
    // N2). Goes through here, not a bare assignment, so `maxDepth` still
    // sees it — a merge can be the first place a wide expression's width
    // becomes visible to this Builder.
    void resync(int newDepth) {
        depth = newDepth;
        maxDepth = std::max(maxDepth, depth);
    }
};

// Everything one chunk's straight-line/control-flow lowering needs, threaded
// through instead of captured by a wall of ad hoc lambdas (nodes/N6.md,
// "split the opcode switch first" — PR #109 R8 measured emitScript's
// cognitive complexity at 69 against a threshold of 25, and four more nodes
// still add cases to it). Each opcode family below is its own function
// taking this by reference, so emitScript's own body shrinks to a dispatch
// table plus the parts genuinely specific to walking the instruction array
// (labels, the R1 depth safety net, invisible-var stores).
struct Emitter {
    const DecodedFunction& fn;
    const FunctionStackAnalysis& analysis;
    Builder b;

    std::unordered_map<int, PopKind> popKinds;
    std::unordered_map<int, std::vector<int>> invisibleVarsByOffset;
    std::unordered_map<int, std::string> labelAtOffset;

    int globalsSlot{0};
    int scratchSlot{0};

    // R4 fix (PR #109 nit): -1 is a sentinel for "no invisible-var site has
    // run yet", not a real slot — see loadLastInvisibleVar.
    int lastInvisibleVarSlot{-1};

    // R5 fix (PR #109 nit): whether the position this walk is about to visit
    // can be reached by fall-through from the instruction this pass most
    // recently emitted — false at the very start, and reset every time a
    // JUMP/LOOP/RETURN/MATCH_ERROR is emitted (none of those fall through) or
    // dead code is skipped between two live instructions (nothing physical
    // bridges that gap).
    bool prevCanFallThrough{false};
    int prevNaturalSuccessorOffset{-1};

    [[nodiscard]] bool reached(std::size_t idx) const {
        return idx < analysis.reached.size() &&
               static_cast<bool>(analysis.reached[idx]);
    }

    [[nodiscard]] const std::string& labelFor(int offset) const {
        auto it = labelAtOffset.find(offset);
        if (it == labelAtOffset.end()) {
            throw std::runtime_error(
                "jvm_emitter: no CFG label at jump target " +
                std::to_string(offset));
        }
        return it->second;
    }

    // SET_LOCAL/SET_GLOBAL peek (the value stays on the stack); a following
    // POP that N2 already proved TEMP is exactly that peeked value being
    // discarded as a statement result, so folding the pair into one plain
    // store — what javac emits for the same idiom — needs no dup and no
    // separate pop (bytecode-translation-problems.md P2).
    [[nodiscard]] bool fusablePop(std::size_t i) const {
        std::size_t j = i + 1;
        if (j >= fn.instructions.size() || !reached(j) ||
            fn.instructions[j].op != Op::POP) {
            return false;
        }
        // PR #109 R1 fix: a POP that is a CFG block leader must stay a real
        // instruction. Every edge into that leader needs its jasmin label
        // (fusing away the instruction fuses away the label with it — a
        // jasmin assemble error, not a wrong result), and the short-circuit
        // edge into this leader carries its own copy of the condition, which
        // needs a real `pop` of its own regardless of what the fall-through
        // edge does with its copy.
        if (labelAtOffset.find(fn.instructions[j].offset) !=
            labelAtOffset.end()) {
            return false;
        }
        auto it = popKinds.find(fn.instructions[j].offset);
        return it != popKinds.end() && it->second == PopKind::TEMP;
    }

    [[nodiscard]] std::string constantString(int idx) const {
        Value v = fn.function->chunk.getConstant(static_cast<uint16_t>(idx));
        if (!isString(v)) {
            throw std::runtime_error("jvm_emitter: DEFINE/GET/SET_GLOBAL "
                                     "constant is not a name string");
        }
        return escapeJasminString(std::string(asObjString(v)->chars));
    }

    // LoxGlobals.define/get/set are instance methods (design decision A2),
    // so every global access needs a receiver in scope — held in a JVM-only
    // local the Lox++ frame never sees.
    void globalsCall(const char* method, const std::string& escapedName,
                     bool peek) {
        std::string call = std::string("invokevirtual lox/LoxGlobals/") +
                           method + "(Ljava/lang/String;Ljava/lang/Object;)V";
        if (!peek) {
            b.emit("aload " + std::to_string(globalsSlot), +1);
            b.emit("swap", 0);
            b.emit("ldc \"" + escapedName + "\"", +1);
            b.emit("swap", 0);
            b.emit(call, -3);
            return;
        }
        // No dup-based shuffle leaves [globalsRef,name,value] on top with
        // the value surviving underneath in one step, so the value is
        // parked in a scratch local instead: simpler to verify than a
        // dup_x2 chain, and this pass tracks its own depth anyway.
        std::string scratch = std::to_string(scratchSlot);
        b.emit("astore " + scratch, -1);
        b.emit("aload " + std::to_string(globalsSlot), +1);
        b.emit("ldc \"" + escapedName + "\"", +1);
        b.emit("aload " + scratch, +1);
        b.emit(call, -3);
        b.emit("aload " + scratch, +1);
    }

    // R4 fix (PR #109 nit): `jvmSlotForLocal(-1)` would silently `aload` the
    // globals-receiver slot as if it were a Lox value instead of failing
    // loudly, so every read of `lastInvisibleVarSlot` goes through here.
    int loadLastInvisibleVar() {
        if (lastInvisibleVarSlot < 0) {
            throw std::runtime_error(
                "jvm_emitter: peek of an invisible var before any "
                "invisible-var site ran");
        }
        int sourceSlot = 2 + lastInvisibleVarSlot;
        b.emit("aload " + std::to_string(sourceSlot), +1);
        return sourceSlot;
    }
};

// N5.md, "inherited from N4": `analysis.before[i].localCount` is only an
// upper bound at a CFG merge (abstract_stack.h), so the SET_LOCAL/
// SET_GLOBAL peek-of-a-named-local case (below) cannot use it to name the
// slot a peek reads once JUMP/JUMP_IF_FALSE/LOOP exist. `lastInvisibleVarSlot`
// tracks the same fact a different, merge-safe way: the slot an
// invisible-var site just bound, updated only by this pass's own forward
// walk in offset order, never by a count aggregated across incoming edges.
//
// PR #109 R3: no program in this node's opcode set (if/else, while, for —
// no functions, no match) has yet made `lastInvisibleVarSlot` disagree with
// `jvmSlotForLocal(before[i].localCount - 1)` at the offsets that read it.
// The reason is the scope-exit rule (brief 5c): `endScope` and
// `emitLoopCleanup` retire every local a block declared, on every path out
// of that block, before control ever reaches a point outside it. Two edges
// into one merge can therefore disagree on `localCount` only through a
// construct that leaves a *different* number of locals live past that merge
// on each path — a function frame or a `match` arm. `lastInvisibleVarSlot`
// still stays, not `localCount`, because it costs nothing today and reads
// correctly the moment either later node breaks that premise.

void emitConstant(Emitter& e, const DecodedInstruction& in) {
    Value v = e.fn.function->chunk.getConstant(
        static_cast<uint16_t>(in.constantIndex));
    if (is<Number>(v)) {
        // Long bits in, exact double out (PR #107 R6/R7): see
        // formatDoubleBitsLiteral. `ldc2_w` of a `long` pushes 2 words;
        // `longBitsToDouble(J)D` consumes 2 (the long) and produces 2 (the
        // double) — net 0 words, so the 3-line net effect (+2, 0, -1) is the
        // same +1 as before this fix.
        e.b.emit("ldc2_w " + formatDoubleBitsLiteral(as<Number>(v)), +2);
        e.b.emit("invokestatic java/lang/Double/longBitsToDouble(J)D", 0);
        e.b.emit("invokestatic java/lang/Double/valueOf(D)Ljava/lang/Double;",
                 -1);
    } else if (isString(v)) {
        e.b.emit("ldc \"" +
                     escapeJasminString(std::string(asObjString(v)->chars)) +
                     "\"",
                 +1);
    } else {
        notImplemented(in.op);
    }
}

// True for the "pure stack effect, no operand" family: literals, arithmetic,
// comparisons, NOT, PRINT. One `Emitter::b.emit` call each, none needing
// anything from `in` beyond the opcode itself.
bool emitSimpleOp(Emitter& e, Op op) {
    switch (op) {
    case Op::NIL:
        e.b.emit("aconst_null", +1);
        return true;
    case Op::TRUE:
        e.b.emit("getstatic java/lang/Boolean/TRUE Ljava/lang/Boolean;", +1);
        return true;
    case Op::FALSE:
        e.b.emit("getstatic java/lang/Boolean/FALSE Ljava/lang/Boolean;", +1);
        return true;
    case Op::EQUAL:
        e.b.emit("invokestatic lox/LoxOps/equal(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Z",
                 -1);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                 0);
        return true;
    case Op::GREATER:
        e.b.emit("invokestatic lox/LoxOps/greater(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Z",
                 -1);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                 0);
        return true;
    case Op::LESS:
        e.b.emit("invokestatic lox/LoxOps/less(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Z",
                 -1);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                 0);
        return true;
    case Op::NEGATE:
        // Returns Object already (auto-boxed inside LoxOps.negate itself).
        e.b.emit("invokestatic lox/LoxOps/negate(Ljava/lang/Object;)Ljava/lang/"
                 "Object;",
                 0);
        return true;
    case Op::ADD:
        e.b.emit("invokestatic lox/LoxOps/add(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Ljava/lang/Object;",
                 -1);
        return true;
    case Op::SUBTRACT:
        e.b.emit(
            "invokestatic lox/LoxOps/subtract(Ljava/lang/Object;Ljava/lang/"
            "Object;)Ljava/lang/Object;",
            -1);
        return true;
    case Op::MULTIPLY:
        e.b.emit(
            "invokestatic lox/LoxOps/multiply(Ljava/lang/Object;Ljava/lang/"
            "Object;)Ljava/lang/Object;",
            -1);
        return true;
    case Op::DIVIDE:
        e.b.emit("invokestatic lox/LoxOps/divide(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Ljava/lang/Object;",
                 -1);
        return true;
    case Op::MODULO:
        e.b.emit("invokestatic lox/LoxOps/modulo(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Ljava/lang/Object;",
                 -1);
        return true;
    case Op::NOT:
        e.b.emit("invokestatic lox/LoxOps/not(Ljava/lang/Object;)Ljava/lang/"
                 "Object;",
                 0);
        return true;
    case Op::PRINT:
        e.b.emit("invokestatic lox/LoxOps/print(Ljava/lang/Object;)V", -1);
        return true;
    default:
        return false;
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
    // LOCAL_RECLAIM: the slot lives in the local array, not on the operand
    // stack — this pass never put it there, so there is nothing here to pop.
}

void emitGetLocal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("aload " + std::to_string(2 + in.byteOperand), +1);
}

void emitSetLocal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                  bool& consumedFollowingPop) {
    int slot = 2 + in.byteOperand;
    bool fuse = e.fusablePop(i);
    // R1 fix (PR #107 round 1): before[i].operandDepth() == 0 means N2
    // already folded the peeked value into a named local (the eager
    // invisible-var materialization, abstract_stack.h) — nothing sits on the
    // JVM operand stack to `dup`. Load it back from its slot instead:
    // `lastInvisibleVarSlot`, not `before[i].localCount`, names it (R9,
    // resolved for N5 — localCount is only an upper bound at a CFG merge,
    // but lastInvisibleVarSlot is this pass's own forward walk, so it is
    // exact regardless of merges upstream).
    if (e.analysis.before[i].operandDepth() == 0) {
        e.loadLastInvisibleVar();
        e.b.emit("astore " + std::to_string(slot), -1);
    } else if (fuse) {
        e.b.emit("astore " + std::to_string(slot), -1);
    } else {
        e.b.emit("dup", +1);
        e.b.emit("astore " + std::to_string(slot), -1);
    }
    consumedFollowingPop = fuse;
}

void emitDefineGlobal(Emitter& e, const DecodedInstruction& in) {
    e.globalsCall("define", e.constantString(in.constantIndex), /*peek=*/false);
}

void emitGetGlobal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("aload " + std::to_string(e.globalsSlot), +1);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("invokevirtual lox/LoxGlobals/get(Ljava/lang/String;)Ljava/lang/"
             "Object;",
             -1);
}

void emitSetGlobal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                   bool& consumedFollowingPop) {
    bool fuse = e.fusablePop(i);
    // R1 fix (PR #107 round 1): same reasoning as SET_LOCAL above. When the
    // source is already a named local, load it explicitly and always use the
    // non-peek call: the plain store fully consumes the loaded copy either
    // way, so no separate fuse/non-fuse split is needed on this branch.
    if (e.analysis.before[i].operandDepth() == 0) {
        e.loadLastInvisibleVar();
        e.globalsCall("set", e.constantString(in.constantIndex),
                      /*peek=*/false);
    } else {
        e.globalsCall("set", e.constantString(in.constantIndex),
                      /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
}

// JUMP and LOOP share the same lowering: a `goto` carries no operand budget
// of its own, so nothing distinguishes a forward skip from a loop's back
// edge once N1 has resolved both to a label.
void emitJumpOrLoop(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("goto " + e.labelFor(in.jumpTarget), 0);
}

// P2/P3: JUMP_IF_FALSE peeks — the condition must still be present, on
// *both* outgoing edges, for whatever follows to see (03_and_or keeps it as
// the short-circuit result; 02/04/05 discard it with their own, ordinary POP
// right after — that POP is not special-cased here). `dup` supplies that
// second, independent copy so the taken edge does not lose its copy to
// `isFalsy`'s pop.
//
// R2 fix (PR #109 round 1): before[i].operandDepth() == 0 is the same eager
// invisible-var materialization as the SET_LOCAL/SET_GLOBAL peek above
// (P2/P3 initializer whose top-level operator is `and`/`or`) — the condition
// is not on the JVM operand stack to `dup`, because N2 already moved it into
// `lastInvisibleVarSlot`. Load a fresh copy from there instead; `isFalsy`/
// `ifne` still only consume that one copy, so the depth-preserving contract
// holds on both edges (0 in, 0 out) exactly as the dup path holds it at
// (D, D) for D > 0.
void emitJumpIfFalse(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        e.loadLastInvisibleVar();
    } else {
        e.b.emit("dup", +1);
    }
    e.b.emit("invokestatic lox/LoxOps/isFalsy(Ljava/lang/Object;)Z", 0);
    e.b.emit("ifne " + e.labelFor(in.jumpTarget), -1);
}

} // namespace

std::string escapeJasminString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            // Control bytes and anything above ASCII: an octal escape keeps
            // the .j file itself pure printable ASCII, so no text-mode
            // re-encoding of the source file can alter the byte the runtime
            // must see (LoxRuntime.CHARSET is ISO-8859-1 — one Lox byte is
            // one Java char, code point = byte value).
            if (c < 0x20 || c >= 0x7f) {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\%03o",
                              static_cast<unsigned>(c));
                out += buf.data();
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string formatDoubleBitsLiteral(double value) {
    // A bare decimal integer: jasmin 2.4 reads an `ldc2_w` operand shaped
    // like this as a `long`, at full precision, never at float precision
    // (PR #107 R6) and never rejected as "badly formatted" (R7) — both
    // defects are specific to the decimal-point/exponent literal forms.
    return std::to_string(std::bit_cast<int64_t>(value));
}

std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className) {
    // High-water mark of concurrently-bound Lox frame slots (arity+1 at
    // entry, growing with every `var`) — the JVM local array must hold all
    // of them at their fixed positions, same as the abstract stack does.
    int maxLocalCount = 0;
    for (std::size_t i = 0; i < analysis.before.size(); i++) {
        if (i >= analysis.reached.size() ||
            !static_cast<bool>(analysis.reached[i])) {
            continue;
        }
        maxLocalCount = std::max({maxLocalCount, analysis.before[i].localCount,
                                  analysis.after[i].localCount});
    }
    maxLocalCount = std::max(maxLocalCount,
                             1); // slot 0: the script's own unnamed callee slot

    Emitter e{fn, analysis, {}};
    e.globalsSlot = 1;
    e.scratchSlot = 2 + maxLocalCount;

    for (const PopClassification& p : analysis.pops) {
        e.popKinds[p.offset] = p.kind;
    }
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        e.invisibleVarsByOffset[site.offset].push_back(site.slot);
    }

    const std::vector<DecodedInstruction>& ins = fn.instructions;
    std::size_t n = ins.size();

    // N5: one jasmin label per N1 block leader. A leader with no predecessor
    // (e.g. the fall-through after an unconditional JUMP) still gets a label;
    // an unreferenced jasmin label is harmless, so this pass does not bother
    // filtering to only-referenced offsets.
    Cfg cfg = buildCfg(ins);
    e.labelAtOffset.reserve(cfg.blocks.size());
    for (const BasicBlock& block : cfg.blocks) {
        e.labelAtOffset.emplace(block.leaderOffset, block.label);
    }

    e.b.emit("invokestatic lox/LoxRuntime/init()Llox/LoxGlobals;", +1);
    e.b.emit("astore " + std::to_string(e.globalsSlot), -1);

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
                // array position right before it is a JUMP/LOOP/RETURN whose
                // real successor is somewhere else entirely, or dead code
                // that never executed, so this pass's own carry-forward is
                // not this block's entry depth at all (the array is in
                // byte-offset order, not control-flow order). N2 already
                // proved operandDepth() exact at every merge, so resync to
                // it here.
                e.b.resync(analysis.before[i].operandDepth());
            }
        }

        // R1 safety net (PR #107 round 1): every correctly-lowered opcode in
        // this pass keeps the JVM operand stack's physical depth equal to
        // N2's own operandDepth() at the same offset — a temp this emitter
        // pushed is the only thing N2 counts as "on the stack". A mismatch
        // here means a peek is about to dup or pop a cell that is not
        // physically there, so abort loudly instead of letting jasmin or the
        // JVM verifier find it.
        if (e.b.depth != analysis.before[i].operandDepth()) {
            throw std::runtime_error(
                "jvm_emitter: simulated stack depth " +
                std::to_string(e.b.depth) + " disagrees with analysis depth " +
                std::to_string(analysis.before[i].operandDepth()) +
                " at offset " + std::to_string(in.offset));
        }

        switch (in.op) {
        case Op::CONSTANT:
            emitConstant(e, in);
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
        case Op::JUMP:
        case Op::LOOP:
            emitJumpOrLoop(e, in);
            break;
        case Op::JUMP_IF_FALSE:
            emitJumpIfFalse(e, i, in);
            break;
        case Op::RETURN:
            // Script form (vm.cpp: frameCount reaches 0, result discarded) —
            // a function's RETURN (areturn) is N6's responsibility.
            e.b.emit("return", 0);
            break;
        default:
            if (!emitSimpleOp(e, in.op)) {
                notImplemented(in.op);
            }
        }

        auto varsIt = e.invisibleVarsByOffset.find(in.offset);
        if (varsIt != e.invisibleVarsByOffset.end()) {
            for (int slot : varsIt->second) {
                e.b.emit("astore " + std::to_string(2 + slot), -1);
                e.lastInvisibleVarSlot = slot;
            }
        }

        std::size_t nextIndex = i + (consumedFollowingPop ? 2 : 1);
        e.prevCanFallThrough = in.op != Op::JUMP && in.op != Op::LOOP &&
                               in.op != Op::RETURN && in.op != Op::MATCH_ERROR;
        e.prevNaturalSuccessorOffset =
            (nextIndex < n) ? ins[nextIndex].offset : -1;
        i = nextIndex;
    }

    std::ostringstream out;
    out << ".class public " << className << "\n";
    out << ".super java/lang/Object\n\n";
    out << ".method public static main([Ljava/lang/String;)V\n";
    out << "    .limit stack " << std::max(1, e.b.maxDepth) << "\n";
    out << "    .limit locals " << (e.scratchSlot + 1) << "\n\n";
    out << e.b.text.str();
    out << ".end method\n";
    return out.str();
}

} // namespace jvm
