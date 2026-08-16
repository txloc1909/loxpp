#include "jvm_emitter.h"

#include "capture_analysis.h"
#include "cfg.h"
#include "container_objects.h"
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
#include <unordered_set>
#include <vector>

namespace jvm {

namespace {

// Every Op enumerator's own spelling — for the "not implemented in N6:"
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
    throw std::runtime_error("not implemented in N6: " + opName(op));
}

// A short, offset-anchored jasmin label for a micro-branch this emitter
// inserts on top of what N1 (cfg.h) already labeled — see
// ensureCapturedCell and the captured GET_LOCAL/SET_LOCAL lowering below.
// N1's own labels are "L_<offset>" (cfg.cpp); the "J" prefix here can never
// collide with one, and `offset` (always unique in one chunk) plus an
// optional sub-index (a CLOSURE can open more than one cell in one
// instruction, one per upvalue) keeps every one of THESE labels unique too.
std::string capLabel(const char* tag, int offset, int sub = -1) {
    std::string s = std::string("Jc") + tag + std::to_string(offset);
    if (sub >= 0) {
        s += "_" + std::to_string(sub);
    }
    return s;
}

// Smallest-encoding int push. Reused for CALL's array-size/index operands,
// GET_UPVALUE/SET_UPVALUE's array index, a generated <init>'s literal
// arity, and CLOSURE's upvalue-wiring loop; never asked for a value outside
// [-1, 255] here (CALL argCount, ObjFunction::arity, and UINT8_COUNT-bounded
// upvalue indices all fit a byte — compiler.cpp enforces each ceiling
// independently).
std::string pushIntInstruction(int n) {
    if (n == -1) {
        return "iconst_m1";
    }
    if (n >= 0 && n <= 5) {
        return "iconst_" + std::to_string(n);
    }
    if (n >= -128 && n <= 127) {
        return "bipush " + std::to_string(n);
    }
    return "sipush " + std::to_string(n);
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
// cognitive complexity at 69 against a threshold of 25). Each opcode family
// below is its own function taking this by reference, so emitChunk's own
// body shrinks to a dispatch table plus the parts genuinely specific to
// walking the instruction array (labels, the R1 depth safety net,
// invisible-var stores).
struct Emitter {
    const DecodedFunction& fn;
    const FunctionStackAnalysis& analysis;
    Builder b;

    std::unordered_map<int, PopKind> popKinds;
    std::unordered_map<int, std::vector<int>> invisibleVarsByOffset;
    std::unordered_map<int, std::string> labelAtOffset;

    // 2 for a script chunk (slot 0 = args, slot 1 = globals), 4 for a
    // function chunk (slot 0 = this, slot 1 = self, slot 2 = args array,
    // slot 3 = globals) — see jvm_emitter.h's layout comment. Every mapped
    // slot below is `baseSlot` plus an offset, so the two entry shapes share
    // one set of opcode-family functions.
    int baseSlot{2};
    int globalsSlot{0};
    int scratchSlot{0};

    // Set (to scratchSlot+1 / scratchSlot+2) only when this chunk contains a
    // CALL with at least one argument, a BUILD_LIST with at least one
    // element, or a BUILD_MAP with at least one pair; -1 otherwise, so a
    // stray use before emitChunk's own prologue computes them fails loudly
    // instead of silently aliasing scratchSlot. emitBuildList and
    // emitBuildMap only ever use argScratchBase, never calleeScratchSlot —
    // see computeMaxSpillWidth's own note.
    int calleeScratchSlot{-1};
    int argScratchBase{-1};

    // This pass's own forward walk, updated in offset order by
    // finishInstruction: the slot the most RECENTLY DECLARED invisible-var
    // site bound. -1 is a sentinel for "no site has run yet", not a real
    // slot.
    //
    // Referee decision (PR #113 round 3): this field is no longer the
    // primary source for "which local holds the zero-depth value" — it
    // names the most recently DECLARED slot, not the topmost LIVE one, and a
    // `match` separates the two (see loadNamedLocalAtZeroDepth). It survives
    // only as loadNamedLocalAtZeroDepth's cross-check input at a CFG merge,
    // where N2's own `localCount` is an upper bound and needs a second,
    // independent estimate to confirm it.
    int lastInvisibleVarSlot{-1};

    // Every Lox local slot this chunk's OWN captures (FunctionCaptureInfo::
    // liveRangesBySlot) ever wrap in an Object[1] ref-cell — see the design
    // note above ensureCapturedCell for why membership, not the exact live
    // range, is all GET_LOCAL/SET_LOCAL/CLOSURE need from N3's analysis
    // here. A slot absent from this set keeps N6's plain aload/astore
    // lowering untouched, so every pre-N7 probe stays byte-identical.
    std::unordered_set<int> capturedSlots;

    [[nodiscard]] bool isCaptured(int loxSlot) const {
        return capturedSlots.contains(loxSlot);
    }

    // R5 fix (PR #109 nit): whether the position this walk is about to visit
    // can be reached by fall-through from the instruction this pass most
    // recently emitted — false at the very start, and reset every time a
    // JUMP/LOOP/RETURN/MATCH_ERROR is emitted (none of those fall through) or
    // dead code is skipped between two live instructions (nothing physical
    // bridges that gap).
    bool prevCanFallThrough{false};
    int prevNaturalSuccessorOffset{-1};

    [[nodiscard]] int jvmSlotForLocal(int loxSlot) const {
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

    // GET_TAG followed immediately by JUMP_TABLE (P8): compileMatchBody's
    // only call site for JUMP_TABLE emits the pair back to back, with
    // nothing else between them, so this is the only shape a JUMP_TABLE is
    // ever found in — a GET_TAG that is not immediately followed by one is
    // the sparse, compare-and-branch match form instead (GET_TAG; CONSTANT;
    // EQUAL, per arm), which needs no fusion.
    //
    // R5 fix (PR #115 round 1): carries `fusablePop`'s own two guards —
    // `reached(j)` and the block-leader test — even though no program today
    // jumps into the middle of a match's own dispatch preamble, so JUMP_TABLE's
    // offset is never actually a label yet. Without these guards, a future
    // change that DID make it one would silently fuse away the label
    // `emitBody` needs for every edge into it, and jasmin would fail far from
    // the cause. With them, this returns false instead, and the ordinary,
    // unfused GET_TAG path emits the label correctly.
    [[nodiscard]] bool fusableJumpTable(std::size_t i) const {
        std::size_t j = i + 1;
        if (j >= fn.instructions.size() || !reached(j) ||
            fn.instructions[j].op != Op::JUMP_TABLE) {
            return false;
        }
        return labelAtOffset.find(fn.instructions[j].offset) ==
               labelAtOffset.end();
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
};

// N5.md, "inherited from N4": `analysis.before[i].localCount` is only an
// upper bound at a CFG merge (abstract_stack.h), so the SET_LOCAL/
// SET_GLOBAL peek-of-a-named-local case cannot use it ALONE to name the slot
// a peek reads once JUMP/JUMP_IF_FALSE/LOOP exist.
//
// CORRECTED (referee decision, PR #113 round 3): an earlier version of this
// note said `lastInvisibleVarSlot` tracked the same fact a merge-safe way,
// and that N6 tried and failed to build a program where the two disagree.
// That is false. `lastInvisibleVarSlot` names the most RECENTLY DECLARED
// invisible var, not the topmost LIVE one, and a `match` expression declares
// its own subject AFTER its own result (compiler.cpp, compileMatchBody) — so
// the two DO disagree, on a plain, unnested match, with no CFG merge
// involved at all (T1/T2/T3, test_jvm_emit.cpp). `loadNamedLocalAtZeroDepth`
// (below emitCapturedStore) is the current mechanism: `localCount - 1` off a
// CFG label, cross-checked against `lastInvisibleVarSlot` on one.
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
    } else if (isEnumCtor(v)) {
        // P8/P6: an enum declaration compiles each variant to a CONSTANT
        // (compiler.cpp's enumDeclaration) that names an ObjEnumCtor, then a
        // DEFINE_GLOBAL — the same shape a plain number or string literal
        // uses. This pass must materialise a real LoxEnumCtor here, once,
        // so every later CALL of it (LoxOps.call, since LoxEnumCtor already
        // implements LoxCallable) needs no case of its own for "the callee
        // came from a CONSTANT, not a CLOSURE".
        ObjEnumCtor* ctor = asObjEnumCtor(as<Obj*>(v));
        e.b.emit("new lox/LoxEnumCtor", +1);
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(ctor->tag), +1);
        e.b.emit(pushIntInstruction(ctor->arity), +1);
        e.b.emit("ldc \"" +
                     escapeJasminString(std::string(ctor->ctorName->chars)) +
                     "\"",
                 +1);
        e.b.emit("ldc \"" +
                     escapeJasminString(std::string(ctor->enumName->chars)) +
                     "\"",
                 +1);
        e.b.emit("invokespecial lox/LoxEnumCtor/<init>(IILjava/lang/String;"
                 "Ljava/lang/String;)V",
                 -5);
    } else {
        notImplemented(in.op);
    }
}

// True for the "pure stack effect, no operand" family: literals, arithmetic,
// comparisons, GET_INDEX/SET_INDEX and peers. One `Emitter::b.emit` call
// each, none needing anything from `in` beyond the opcode itself. NOT and
// GET_INDEX moved out to their own functions (R3, PR #115 round 1) — see
// emitNot's and emitGetIndex's own notes — because a folded `match` result
// needs the instruction index this dispatch does not carry; PRINT moved out
// the same way before this PR, for the same reason.
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
    // NOT is pulled out to its own function, emitNot (below emitDefineGlobal)
    // — the same reason PRINT was pulled out: it needs an instruction index
    // to route a folded `match` result through loadNamedLocalAtZeroDepth
    // (R3, PR #115 round 1), and this dispatch has none to give it.
    //
    // GET_INDEX/SET_INDEX (node N7 pulls these two opcodes forward from N9's
    // scope — see emitBuildList's own note: V1_fresh_cell/V3_loopvar, N7's
    // own checkpoint probes, both index a list of the closures under test).
    // vm.cpp's own operand order already matches LoxOps's parameter order
    // exactly ([collection, index] and [collection, index, value], bottom
    // to top), so — unlike SET_LOCAL/SET_GLOBAL/SET_UPVALUE — neither is a
    // P2 peek: vm.cpp pops SET_INDEX's operands whole and pushes a genuinely
    // new result cell (P2's own peek family list, abstract_stack.h, does not
    // include it), so a plain call with no dup is exactly right.
    //
    // GET_INDEX itself is also pulled out to its own function, emitGetIndex
    // (below emitSpillToArray) — same reason as NOT/PRINT above: its own
    // collection operand can be a folded `match` result too (R3), a case
    // SET_INDEX's own checkpoint programs never reach (see emitGetIndex's
    // own note for why SET_INDEX is not changed the same way).
    case Op::SET_INDEX:
        e.b.emit("invokestatic "
                 "lox/LoxOps/setIndex(Ljava/lang/Object;Ljava/lang/Object;"
                 "Ljava/lang/Object;)Ljava/lang/Object;",
                 -2);
        return true;
    // SLICE pops [seq, start, end] bottom-to-top (vm.cpp: peek(2), peek(1),
    // peek(0)) — LoxOps.slice's own parameter order already matches, so
    // this is a plain call, no shuffle (same P2 exemption as GET_INDEX).
    case Op::SLICE:
        e.b.emit("invokestatic "
                 "lox/LoxOps/slice(Ljava/lang/Object;Ljava/lang/Object;"
                 "Ljava/lang/Object;)Ljava/lang/Object;",
                 -2);
        return true;
    // IN pops [elem, seq] bottom-to-top (vm.cpp pops seq first, so seq sits
    // on top) — LoxOps.in's own doc comment already matches that order.
    case Op::IN:
        e.b.emit("invokestatic lox/LoxOps/in(Ljava/lang/Object;Ljava/lang/"
                 "Object;)Z",
                 -1);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                 "Boolean;",
                 0);
        return true;
    // ITER_HAS_NEXT/ITER_NEXT consume the copy a preceding GET_LOCAL already
    // loaded (P8 — the iterator lives in an ordinary chunk local, never a
    // dedicated backend slot); the local itself is untouched.
    case Op::ITER_HAS_NEXT:
        e.b.emit("invokestatic lox/LoxOps/iterHasNext(Ljava/lang/Object;)Z", 0);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                 "Boolean;",
                 0);
        return true;
    case Op::ITER_NEXT:
        e.b.emit("invokestatic lox/LoxOps/iterNext(Ljava/lang/Object;)"
                 "Ljava/lang/Object;",
                 0);
        return true;
    case Op::IS_SEQ:
        e.b.emit("invokestatic lox/LoxOps/isSeq(Ljava/lang/Object;)Z", 0);
        e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/"
                 "Boolean;",
                 0);
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

// A captured local's JVM slot is not one fixed representation for its whole
// life: before its first capture it holds the raw Lox value (node N7's
// contract with N3 — see ensureCapturedCell's own note); from there through
// CLOSE_UPVALUE it holds an Object[1] ref-cell instead. GET_LOCAL/SET_LOCAL
// on a captured slot cannot pick which one applies by this instruction's
// OWN offset, for the same reason ensureCapturedCell cannot: a `for` loop's
// condition/increment clauses sit, in byte order, BEFORE the body that
// captures the loop variable, yet the LOOP back-edge revisits them on every
// later iteration, by which point the slot IS a cell (V3_loopvar.lox) —
// program order and runtime order are not the same thing once a back-edge
// exists. `instanceof [Ljava/lang/Object;` asks the one question that is
// always true regardless of which trip this is: no Lox value is ever
// itself a bare Object[] (LoxList/LoxMap/LoxEnum all wrap their storage in
// a named class — runtime/jvm/src), so this test can never mistake a real
// Lox value for a cell or the reverse.
void emitCapturedGetLocal(Emitter& e, int slot, int offset) {
    std::string rawLbl = capLabel("gr", offset);
    std::string endLbl = capLabel("ge", offset);
    int d = e.b.depth;
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("instanceof [Ljava/lang/Object;", 0);
    e.b.emit("ifeq " + rawLbl, -1);
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("checkcast [Ljava/lang/Object;", 0);
    e.b.emit("iconst_0", +1);
    e.b.emit("aaload", -1);
    e.b.emit("goto " + endLbl, 0);
    e.b.label(rawLbl);
    e.b.resync(d);
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.label(endLbl);
    e.b.resync(d + 1);
}

// SET_LOCAL's captured-slot lowering: the value to store already sits on
// top of the stack (`peek` decides whether it must still be there after —
// P2), but deciding raw-vs-cell needs the SLOT's own current content, which
// is a second value this pass does not have a free stack cell to hold
// without disturbing the first — spilled to `scratchSlot` for the same
// reason globalsCall's peek variant spills there (P2 shuffle, no dup_x2
// chain to verify). Never a scratch-slot conflict with a CALL or another
// SET_LOCAL/SET_GLOBAL/SET_UPVALUE's own use of it: one instruction runs to
// completion before the next starts.
void emitCapturedStore(Emitter& e, int slot, int offset, bool peek) {
    std::string rawLbl = capLabel("sr", offset);
    std::string endLbl = capLabel("se", offset);
    std::string scratch = std::to_string(e.scratchSlot);
    int d = e.b.depth;
    e.b.emit("astore " + scratch, -1);
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("instanceof [Ljava/lang/Object;", 0);
    e.b.emit("ifeq " + rawLbl, -1);
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("checkcast [Ljava/lang/Object;", 0);
    e.b.emit("iconst_0", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("aastore", -3);
    e.b.emit("goto " + endLbl, 0);
    e.b.label(rawLbl);
    e.b.resync(d - 1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("astore " + std::to_string(slot), -1);
    e.b.label(endLbl);
    e.b.resync(d - 1);
    if (peek) {
        e.b.emit("aload " + scratch, +1);
    }
}

// The one mechanism every zero-operand-depth consumer shares — P1/P2 says
// the value to consume is a NAMED LOCAL, not a genuine JVM operand-stack
// temp, whenever `before[i].operandDepth() == 0`.
//
// Cross-check design (referee decision, PR #113 round 3): N2's own
// reconstructed `localCount` and this pass's own forward-walking
// `lastInvisibleVarSlot` tracker used to be two separate mechanisms for
// answering "which local holds it". Every consumer that trusted the tracker
// alone gave silently wrong output on a `match`, because the tracker names
// the most RECENTLY DECLARED slot, while `compileMatchBody` declares a
// match's own subject AFTER its own result — the result, not the subject,
// is what an enclosing consumer wants (PR #113 R6). T1/T2/T3
// (test_jvm_emit.cpp) prove the same defect for a PLAIN, unnested match, on
// `main`, so this was never only a nesting defect.
//
// `localCount - 1` is exact away from a CFG merge (abstract_stack.h). At a
// merge it is only an upper bound, so the tracker now serves as a second,
// independent estimate that must confirm it. Agreement emits exactly what
// the tracker path used to emit alone, so no green shape regresses.
// Disagreement means the old, unconditional tracker read gave silently
// wrong output (T1-T3) — so this throws at emit time instead, loud rather
// than silent. See the GAP entry in bytecode-translation-problems.md for
// the residual cases this still does not cover.
//
// The captured-slot check (R5, PR #113 round 2) applies to either estimate:
// `capturedSlots` holds slot INDEXES, not live ranges (isCaptured's own
// note) — a slot this chunk captured earlier, in a scope already closed,
// stays in the set once the compiler reuses the index for an unrelated
// later local, such as a match result or GET_ITER's own iterable.
// `emitCapturedGetLocal`'s runtime raw-or-cell test is correct either way,
// including a self-recursive closure's own seeded cell:
// storeClosureIntoSelfCell marks that slot captured the normal way, so no
// separate self-cell case is needed here (V5/V6 verify it).
int loadNamedLocalAtZeroDepth(Emitter& e, std::size_t i, int offset) {
    int loxSlot = e.analysis.before[i].localCount - 1;
    if (e.labelAtOffset.contains(offset) && e.lastInvisibleVarSlot != loxSlot) {
        throw std::runtime_error(
            "jvm_emitter: offset " + std::to_string(offset) +
            " is a CFG merge; localCount - 1 (" + std::to_string(loxSlot) +
            ") disagrees with the forward-walk tracker (" +
            std::to_string(e.lastInvisibleVarSlot) + ")");
    }
    int slot = e.jvmSlotForLocal(loxSlot);
    if (e.isCaptured(loxSlot)) {
        emitCapturedGetLocal(e, slot, offset);
    } else {
        e.b.emit("aload " + std::to_string(slot), +1);
    }
    return slot;
}

// GET_ITER replaces its own operand in place (vm.cpp: `stackTop[-1] = ...`).
// It carries no operand byte of its own. The iterable expression's own
// declaring push (11_for_in.lox: e.g. BUILD_LIST) is what put the value
// there, and N2/N3 already hand THAT instruction's own offset the
// invisible-var store for this slot (finishInstruction), one instruction
// earlier than GET_ITER itself. By the time GET_ITER runs, the JVM operand
// stack is therefore already empty at this position (`operandDepth() == 0`)
// — the value already lives in its own JVM local slot, not still sitting on
// the operand stack the way a plain "simple op" would assume. GET_ITER must
// reload that slot, transform it, and store the result straight back —
// `loadNamedLocalAtZeroDepth` names and loads it, with the same
// merge/captured-slot guards every other zero-depth consumer shares.
void emitGetIter(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() != 0) {
        throw std::runtime_error(
            "jvm_emitter: GET_ITER expected its iterable already folded "
            "into an invisible-var slot (operand depth 0), but the JVM "
            "operand stack was not empty here");
    }
    int loxSlot = e.analysis.before[i].localCount - 1;
    int slot = loadNamedLocalAtZeroDepth(e, i, in.offset);
    e.b.emit("invokestatic "
             "lox/LoxOps/getIter(Ljava/lang/Object;)Llox/LoxIterator;",
             0);
    if (e.isCaptured(loxSlot)) {
        emitCapturedStore(e, slot, in.offset, /*peek=*/false);
    } else {
        e.b.emit("astore " + std::to_string(slot), -1);
    }
}

void emitGetLocal(Emitter& e, const DecodedInstruction& in) {
    int slot = e.jvmSlotForLocal(in.byteOperand);
    if (e.isCaptured(in.byteOperand)) {
        emitCapturedGetLocal(e, slot, in.offset);
    } else {
        e.b.emit("aload " + std::to_string(slot), +1);
    }
}

void emitSetLocal(Emitter& e, std::size_t i, const DecodedInstruction& in,
                  bool& consumedFollowingPop) {
    int slot = e.jvmSlotForLocal(in.byteOperand);
    bool fuse = e.fusablePop(i);
    bool captured = e.isCaptured(in.byteOperand);
    // R1 fix (PR #107 round 1): before[i].operandDepth() == 0 means N2
    // already folded the peeked value into a named local (the eager
    // invisible-var materialization, abstract_stack.h) — nothing sits on the
    // JVM operand stack to `dup`. Load it back from its slot instead.
    //
    // R6 fix (PR #113 round 2): that slot is `loadNamedLocalAtZeroDepth`'s
    // `localCount - 1`, not `lastInvisibleVarSlot` — a nested match's own
    // result defeats the tracker (see that function's own note) with no
    // error, only a wrong value. This site is exactly where R6's
    // reproduction (a `match` arm whose value is itself a nested `match`)
    // surfaced the defect: the OUTER arm's own `SET_LOCAL` into its result
    // slot is this instruction.
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        if (captured) {
            emitCapturedStore(e, slot, in.offset, /*peek=*/false);
        } else {
            e.b.emit("astore " + std::to_string(slot), -1);
        }
    } else if (captured) {
        emitCapturedStore(e, slot, in.offset, /*peek=*/!fuse);
    } else if (fuse) {
        e.b.emit("astore " + std::to_string(slot), -1);
    } else {
        e.b.emit("dup", +1);
        e.b.emit("astore " + std::to_string(slot), -1);
    }
    consumedFollowingPop = fuse;
}

// R7 fix (N10's own residue, PR #113 round 3 referee decision): a `var` at
// script scope compiles to CONSTANT/expr then DEFINE_GLOBAL
// (compiler.cpp's varDeclaration, m_scopeDepth == 0), so `var n = match
// c {...};` at the top level — 13_enum_match.lox's own shape — reaches this
// opcode with a match's result still sitting only in its own JVM local slot,
// never on the real JVM operand stack (compileMatchBody's own comment: "the
// native VM's fused local/operand-stack model... leaves the already-stored
// result local as the new top of stack", which the JVM backend does not
// share). `loadNamedLocalAtZeroDepth` is the one shared mechanism every such
// consumer routes through (RETURN, SET_LOCAL, SET_GLOBAL, SET_UPVALUE,
// JUMP_IF_FALSE, GET_ITER already do); this opcode had no branch for it at
// all before this fix, so a bare `var x = match ...;` at script scope threw
// "operand stack underflow" on every enum/match example that assigns its
// result to a top-level variable.
void emitDefineGlobal(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    }
    e.globalsCall("define", e.constantString(in.constantIndex), /*peek=*/false);
}

// N10's own residue: same defect as emitDefineGlobal's own fix above, on the
// most common shape of all — `print match ...;` (match_http_status.lox,
// match_state_machine.lox, match_dispatch.lox all use exactly this). Pulled
// out of emitSimpleOp (which has no instruction index to give
// loadNamedLocalAtZeroDepth) rather than threading one through every case
// there — emitNot and emitGetIndex, below, are pulled out the same way.
//
// R3 fix (PR #115 round 1): ADD and CALL stay a documented, still-throwing
// gap, but not because no checkpoint reaches them with a folded operand — a
// program that sandwiches a `match` between another live operand and its own
// consumer (`1 + match ...`, `id(match ...)`) does reach this pass with
// operandDepth() == 0 there too. Both are ALSO broken on `build/loxpp`
// itself, with no JVM backend involved: `compileMatchBody`'s own
// resultSlot/subjectSlot allocation (compiler.cpp) uses `m_localCount` alone,
// blind to a sibling operand already live on the real VM stack, so the two
// collide at the same absolute slot. `1 + match 1 { case 1 => 2 case _ => 3
// };` is legal per spec/02-syntax.md (`match` is `primary`, so `term` can
// hold it directly either side of `+`) and native still raises "Operands
// must be numbers." — a pre-existing compiler defect, out of a backend
// node's charter (brief.md section 8: no compiler changes) and not owed a
// matching JVM answer, because no correct native answer exists to match (PR
// body has the full repro set and native output for all five).
void emitPrint(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    }
    e.b.emit("invokestatic lox/LoxOps/print(Ljava/lang/Object;)V", -1);
}

// R3 fix (PR #115 round 1): `!match ...` reaches operandDepth() == 0 the same
// way PRINT does — `print !(match 1 {...});` is one of R3's own repro
// programs (PR body). No checkpoint example uses this exact shape today; see
// emitPrint's own note for ADD/CALL, the two shapes this pass still does not
// attempt.
void emitNot(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    }
    e.b.emit("invokestatic lox/LoxOps/not(Ljava/lang/Object;)Ljava/lang/"
             "Object;",
             0);
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
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        e.globalsCall("set", e.constantString(in.constantIndex),
                      /*peek=*/false);
    } else {
        e.globalsCall("set", e.constantString(in.constantIndex),
                      /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
}

// GET_UPVALUE n reads upvals[n][0]: the field LoxClosure declares
// (runtime/jvm/src/lox/LoxClosure.java) is already typed Object[][], so
// `aaload` on it needs no checkcast — unlike a captured LOCAL slot, which
// jasmin only ever tracks as plain Object (see emitCapturedGetLocal).
void emitGetUpvalue(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("aload 0", +1);
    e.b.emit("getfield lox/LoxClosure/upvalues [[Ljava/lang/Object;", 0);
    e.b.emit(pushIntInstruction(in.byteOperand), +1);
    e.b.emit("aaload", -1);
    e.b.emit("iconst_0", +1);
    e.b.emit("aaload", -1);
}

// SET_UPVALUE's own P2 peek/fuse shuffle: the assigned value already sits
// on top of the stack, so it is spilled to `scratchSlot` (same reasoning as
// emitCapturedStore) while `upvals[index]` is fetched, then written back
// into that cell's slot 0.
void emitUpvalueStore(Emitter& e, int index, bool peek) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("astore " + scratch, -1);
    e.b.emit("aload 0", +1);
    e.b.emit("getfield lox/LoxClosure/upvalues [[Ljava/lang/Object;", 0);
    e.b.emit(pushIntInstruction(index), +1);
    e.b.emit("aaload", -1);
    e.b.emit("iconst_0", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("aastore", -3);
    if (peek) {
        e.b.emit("aload " + scratch, +1);
    }
}

void emitSetUpvalue(Emitter& e, std::size_t i, const DecodedInstruction& in,
                    bool& consumedFollowingPop) {
    bool fuse = e.fusablePop(i);
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        emitUpvalueStore(e, in.byteOperand, /*peek=*/false);
    } else {
        emitUpvalueStore(e, in.byteOperand, /*peek=*/!fuse);
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
// is not on the JVM operand stack to `dup`, because N2 already folded it into
// a named local. `loadNamedLocalAtZeroDepth` names and loads a fresh copy;
// `isFalsy`/`ifne` still only consume that one copy, so the depth-preserving
// contract holds on both edges (0 in, 0 out) exactly as the dup path holds it
// at (D, D) for D > 0.
void emitJumpIfFalse(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    } else {
        e.b.emit("dup", +1);
    }
    e.b.emit("invokestatic lox/LoxOps/isFalsy(Ljava/lang/Object;)Z", 0);
    e.b.emit("ifne " + e.labelFor(in.jumpTarget), -1);
}

// P5 (calling convention) + P7 (aggregate construction): CALL argCount finds
// [callee, arg0, ..., arg(argCount-1)] already loose on the operand stack
// (arg(argCount-1) on top; vm.cpp's own bottom-to-top push order) and must
// hand LoxOps.call one Object[]. argCount == 0 needs no reshaping at all —
// the callee is already the sole, topmost value, so the empty array builds
// directly on top of it. argCount >= 1 spills every value to a scratch
// local first: the elements are already on the stack *below* where a fresh
// array reference would land, so a dup-based build cannot reach them (P7).
// emitChunk reserves one scratch slot per argument the chunk's widest CALL
// needs, plus one for the callee, computed once before this pass starts —
// reused across every CALL site in the chunk, because calls run one at a
// time, never concurrently.
void emitCall(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    const char* callSig =
        "invokestatic "
        "lox/LoxOps/call(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/"
        "Object;";
    if (argCount == 0) {
        e.b.emit(pushIntInstruction(0), +1);
        e.b.emit("anewarray java/lang/Object", 0);
        e.b.emit(callSig, -1);
        return;
    }
    for (int i = argCount - 1; i >= 0; i--) {
        e.b.emit("astore " + std::to_string(e.argScratchBase + i), -1);
    }
    e.b.emit("astore " + std::to_string(e.calleeScratchSlot), -1);

    e.b.emit("aload " + std::to_string(e.calleeScratchSlot), +1);
    e.b.emit(pushIntInstruction(argCount), +1);
    e.b.emit("anewarray java/lang/Object", 0);
    for (int i = 0; i < argCount; i++) {
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aload " + std::to_string(e.argScratchBase + i), +1);
        e.b.emit("aastore", -3);
    }
    e.b.emit(callSig, -1);
}

// Shared P7 reshape for BUILD_LIST/BUILD_MAP (R4 fix, PR #112 round 1):
// spill `width` values, already on the stack BELOW where a fresh array
// reference would land, into `argScratchBase` (same reasoning as emitCall),
// build a fresh Object[width], refill it ascending, then hand it to
// `buildSig`. `width == 0` needs no scratch at all: the empty array builds
// directly, with nothing to spill or refill. `buildSig` fixes only the
// element count and the runtime call; the six-step shape is otherwise
// identical for a list of N elements and a map of N pairs (2N cells). Node
// N10 needs this same shape a third time, for an enum constructor's payload
// array.
void emitSpillToArray(Emitter& e, int width, const char* buildSig) {
    if (width == 0) {
        e.b.emit(pushIntInstruction(0), +1);
        e.b.emit("anewarray java/lang/Object", 0);
        e.b.emit(buildSig, 0);
        return;
    }
    for (int i = width - 1; i >= 0; i--) {
        e.b.emit("astore " + std::to_string(e.argScratchBase + i), -1);
    }
    e.b.emit(pushIntInstruction(width), +1);
    e.b.emit("anewarray java/lang/Object", 0);
    for (int i = 0; i < width; i++) {
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aload " + std::to_string(e.argScratchBase + i), +1);
        e.b.emit("aastore", -3);
    }
    e.b.emit(buildSig, 0);
}

// BUILD_LIST (node N7 pulls this one N9 opcode family forward — see
// notes/backend-implementation-dag.md's build-order note). N7's own
// checkpoint (nodes/N7.md) cannot run to completion without it:
// V1_fresh_cell.lox and V3_loopvar.lox — the two probes that prove or
// disprove the fresh-cell-per-declaration model this whole node exists to
// get right — both build a list of the closures under test and read it
// back by index (`fns[i] = f`, `fs[0]()`), so an emitter that still throws
// "not implemented" on BUILD_LIST/GET_INDEX/SET_INDEX cannot even reach the
// closure bug this node is named for. GET_INDEX/SET_INDEX (emitSimpleOp)
// need no shuffle of their own — LoxOps's parameter order already matches
// vm.cpp's own operand order — so BUILD_LIST's own spill-to-scratch is the
// only new shuffle this addition needs. LoxOps.buildList (runtime/jvm)
// copies the array into a fresh LoxList in the same order.
//
// R3 fix (PR #115 round 1): a one-element list whose sole element is a
// `match` (`print [match 1 {...}];`) reaches operandDepth() == 0 with
// nothing else live on the stack, the same shape emitPrint already folds —
// load it first, so emitSpillToArray finds a genuine value to spill instead
// of underflowing. A width above 1 with the LAST element folded is left
// alone: that is the same sandwiched shape emitPrint's own note rules ADD/
// CALL out of scope for (an earlier, un-counted sibling element collides
// with `compileMatchBody`'s own slot allocation on `build/loxpp` itself), so
// it is not a case this pass owes a matching answer either.
void emitBuildList(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (in.byteOperand == 1 && e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
    }
    emitSpillToArray(e, in.byteOperand,
                     "invokestatic lox/LoxOps/buildList([Ljava/lang/"
                     "Object;)Llox/LoxList;");
}

// BUILD_MAP n: n key/value pairs already on the stack, pushed in source
// order — key0, val0, key1, val1, ..., key_{n-1}, val_{n-1}
// (compiler.cpp's mapLiteral) — so the same P7 reshape as emitBuildList
// applies, just to 2n cells instead of n. vm.cpp validates every key
// before writing any pair ("Validate all keys before any allocation");
// LoxOps.buildMap (runtime/jvm) keeps that same two-pass shape.
void emitBuildMap(Emitter& e, const DecodedInstruction& in) {
    emitSpillToArray(e, 2 * in.byteOperand,
                     "invokestatic lox/LoxOps/buildMap([Ljava/lang/"
                     "Object;)Llox/LoxMap;");
}

// R3 fix (PR #115 round 1): GET_INDEX's own collection operand can be a
// `match`'s folded result while the index is a genuine, already-pushed
// value — `call`'s own subscript grammar always compiles the index after the
// collection, so `(match 1 {...})[0]` leaves the JVM operand stack holding
// only the index (before[i].operandDepth() == 1, not 0) by the time this
// instruction runs. loadNamedLocalAtZeroDepth's own operandDepth() == 0
// check therefore never fires here — but `localCount - 1` still names the
// right slot regardless, because nothing between the fold and here declares
// a new invisible var or crosses a CFG merge (CONSTANT's own emission never
// touches localCount or lastInvisibleVarSlot; finishInstruction only updates
// the latter at a declaring push's own offset), so the value the cross-check
// would guard is unchanged from whatever it was right after the fold.
//
// Spill the already-pushed index to e.scratchSlot (not argScratchBase: a
// chunk with a folded-collection GET_INDEX and no CALL/BUILD_LIST/BUILD_MAP
// anywhere never reserves that region — computeMaxSpillWidth does not scan
// for GET_INDEX — while scratchSlot is always reserved), load the collection
// by its named local, then restore the index on top: the same reorder
// emitCall/emitSpillToArray use to rebuild an out-of-order operand set.
//
// operandDepth() == 0 here (both operands folded, e.g. a match indexed by
// another match) is not reached by any checkpoint program; it is left as the
// pre-existing underflow, the same documented gap as ADD/CALL (emitPrint's
// own note). SET_INDEX is not changed the same way: its own checkpoint
// programs (V1_fresh_cell.lox, V3_loopvar.lox, and every example that
// mutates a list/map by index) never index-assign directly into a bare
// match result, so there is no reached shape to prove a fix against.
void emitGetIndex(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    const char* sig = "invokestatic "
                      "lox/LoxOps/getIndex(Ljava/lang/Object;Ljava/lang/"
                      "Object;)Ljava/lang/Object;";
    if (e.analysis.before[i].operandDepth() == 1) {
        std::string scratch = std::to_string(e.scratchSlot);
        e.b.emit("astore " + scratch, -1);
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        e.b.emit("aload " + scratch, +1);
        e.b.emit(sig, -1);
        return;
    }
    e.b.emit(sig, -1);
}

// Wraps `slot` in a fresh Object[1] ref-cell, seeded with the raw value
// already there, UNLESS `slot` already holds one — an idempotent seed, not
// an unconditional one, and that is the load-bearing choice of this whole
// node (nodes/N7.md, "the BUG GATE").
//
// N3 opens a captured local's live range at the CAPTURING CLOSURE, not at
// the declaration (capture_analysis.h) — reads/writes before that point use
// the raw slot, the range's own start seeds a cell from whatever the
// declaration already put there, and CLOSE_UPVALUE ends it. A static
// codegen pass COULD turn that into an unconditional seed here, if this
// CLOSURE offset ran at most once between the declaration and the close.
// It does not, in general: V3_loopvar.lox declares `i` once, OUTSIDE the
// loop, then captures it from a CLOSURE INSIDE the loop body — one static
// offset, reached once per iteration, with no CLOSE_UPVALUE between trips
// (the range spans the whole loop; N3 marks it `perIteration=false`). An
// unconditional seed here would hand every iteration's closure ITS OWN
// fresh cell instead of the one shared cell V3_loopvar's checkpoint (3,3,3)
// requires. The idempotent check is what makes ONE emitted instruction
// correct on both trips: seed on the first, no-op on every one after,
// because nothing else in this chunk ever turns a cell back into a raw
// value once created — CLOSE_UPVALUE is a compile-time bookkeeping fact
// here (mission brief 5c), not a JVM instruction (see the CLOSE_UPVALUE
// case in emitBody) — and a fresh DECLARATION always re-`astore`s the slot
// directly (finishInstruction, emitSetLocal), never through this check, so
// it is exactly what puts a slot back to raw for the NEXT incarnation
// (V1_fresh_cell.lox: `var snapshot` re-declares, hence re-seeds, on every
// trip of ITS loop).
//
// The same reasoning that forced GET_LOCAL/SET_LOCAL onto a runtime check
// (emitCapturedGetLocal's own note) forces one here too, for the identical
// reason: this is the SAME kind of point program order cannot resolve
// alone once a back-edge is in play.
void ensureCapturedCell(Emitter& e, int slot, int offset, int subIndex) {
    std::string readyLbl = capLabel("ok", offset, subIndex);
    int d = e.b.depth;
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("instanceof [Ljava/lang/Object;", 0);
    e.b.emit("ifne " + readyLbl, -1);
    e.b.emit("iconst_1", +1);
    e.b.emit("anewarray java/lang/Object", 0);
    e.b.emit("dup", +1);
    e.b.emit("iconst_0", +1);
    e.b.emit("aload " + std::to_string(slot), +1);
    e.b.emit("aastore", -3);
    e.b.emit("astore " + std::to_string(slot), -1);
    e.b.label(readyLbl);
    e.b.resync(d);
}

// CLOSURE (node N6 lowered the zero-upvalue construction; N7 wires the
// rest). `childClassNames[i]` names the class this chunk's own i-th nested
// function (chunk_decoder.h: DecodedInstruction::nestedIndex) was assigned
// by emitProgram's pre-order walk. emitScript (no nested functions in any
// pre-N6 caller) always passes an empty vector, so a CLOSURE reaching this
// from there is a real bug, caught below rather than silently mis-indexed.
//
// Every isLocal=1 entry's slot gets ensureCapturedCell's idempotent seed
// BEFORE the array-build loop below reads it — building the Object[][]
// upvals array must see a cell in that slot, never the raw value, whether
// this is the FIRST closure to capture that incarnation or a later one that
// only needs to share it (nodes/N7.md: "two closures that capture the same
// slot in the same live range must share one cell" — V2_shared.lox,
// 06_shared_upvalue.lox). `up.index` after the seed IS the cell, read with
// a plain `aload`; jasmin/the old verifier still only ever tracked that
// slot as generic Object, so `checkcast` narrows it before the `aastore`
// into the Object[]-typed upvals array (the seed's OWN aastore, above, does
// not need one: `anewarray` already gives it the exact array type).
//
// R2 fix (PR #111 round 1): `up.isLocal` and `e.capturedSlots` come from two
// different sources that agree today by construction, not by any check —
// the former is the CLOSURE instruction's own decoded operand bytes, the
// latter is N3's `liveRangesBySlot`. Nothing makes a future drift between
// the two impossible, and a silent one would seed a cell this pass never
// marks captured, so GET_LOCAL/SET_LOCAL elsewhere would keep reading the
// raw value while this closure reads the cell — a wrong VALUE, no verifier
// error, no exception. Fail loudly instead, before N8 adds `super` as a
// second path that can make this same claim.
void checkAllCapturesAreReported(const Emitter& e,
                                 const DecodedInstruction& in) {
    for (const ClosureUpvalue& up : in.upvalues) {
        if (up.isLocal && !e.isCaptured(up.index)) {
            throw std::runtime_error(
                "jvm_emitter: CLOSURE captures local slot " +
                std::to_string(up.index) +
                " that capture analysis does not report");
        }
    }
}

// The Lox slot THIS closure's own declaring push lands in, if any (only a
// named local `fun` has one — findInvisibleVarIndices records it as an
// invisible-var site at this very offset, never at any other, because
// closureIsConsumedImmediately is false for every local declaration), AND
// that same closure captures as an upvalue (direct recursion). -1 when this
// CLOSURE is not that shape.
int findSelfCaptureLoxSlot(const Emitter& e, const DecodedInstruction& in) {
    auto ownSlotIt = e.invisibleVarsByOffset.find(in.offset);
    if (ownSlotIt == e.invisibleVarsByOffset.end()) {
        return -1;
    }
    for (int candidate : ownSlotIt->second) {
        for (const ClosureUpvalue& up : in.upvalues) {
            if (up.isLocal && up.index == candidate) {
                return candidate;
            }
        }
    }
    return -1;
}

// Self-capture (PR #111 R1): a local `fun` that calls itself makes ONE of
// this CLOSURE's isLocal entries name the very slot it is declaring.
// `ensureCapturedCell` cannot run on that slot the normal way: at this
// offset nothing has ever written it, so its `aload` reads an uninitialized
// JVM register and the verifier rejects the class. The native VM sidesteps
// this because its "local" IS the value stack slot: vm.cpp pushes the
// closure first, and that push already IS the declaring store, so
// `captureUpvalue` always finds a real value there. The JVM prologue gives
// this pass no such order for free, so this seeds a fresh cell into the
// slot BEFORE anything reads it — always a FRESH one, unconditionally,
// since this offset IS the declaration, so whatever the slot currently
// holds is a dead incarnation regardless (mission brief 5c). `anewarray`
// default-initializes `[0]` to null; the real value lands there once the
// closure exists, in storeClosureIntoSelfCell below. After this call the
// array-build loop in emitClosure treats the slot exactly like any other
// already-a-cell capture — no special case needed there.
void seedSelfCaptureCell(Emitter& e, int selfJvmSlot) {
    e.b.emit("iconst_1", +1);
    e.b.emit("anewarray java/lang/Object", 0);
    e.b.emit("astore " + std::to_string(selfJvmSlot), -1);
}

// The declaring store, redirected: write the closure just built into the
// cell's own `[0]`, never into the JVM slot itself (which already holds
// that cell, from seedSelfCaptureCell) — finishInstruction's ordinary plain
// `astore` would undo the seed and hand every capturing sibling closure a
// stale cell (R1's second, separate defect). Spilled to `scratchSlot` first
// for the same reason emitCapturedStore does: building [cellRef, 0, value]
// for `aastore` needs the value parked somewhere while the cell reference
// is fetched.
void storeClosureIntoSelfCell(Emitter& e, const DecodedInstruction& in,
                              int selfJvmSlot, int selfLoxSlot) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("astore " + scratch, -1);
    e.b.emit("aload " + std::to_string(selfJvmSlot), +1);
    e.b.emit("iconst_0", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("aastore", -3);

    // finishInstruction must not ALSO store this offset's invisible var: it
    // would run its plain `astore` after the write above and put the raw
    // closure back into the slot, undoing this fix. Erase it here instead
    // of leaving finishInstruction to guess.
    auto& slots = e.invisibleVarsByOffset[in.offset];
    slots.erase(std::remove(slots.begin(), slots.end(), selfLoxSlot),
                slots.end());
    // `capturedSlots` already marks `selfLoxSlot` captured (it is one of
    // this CLOSURE's own upvalue entries), so `loadNamedLocalAtZeroDepth`'s
    // ordinary raw-or-cell test handles this cell like any other captured
    // slot — no separate self-cell flag or throw is needed here (referee
    // decision, PR #113 round 3; V5/V6 verify it).
    e.lastInvisibleVarSlot = selfLoxSlot;
}

void emitClosure(Emitter& e, const DecodedInstruction& in,
                 const std::vector<std::string>& childClassNames) {
    if (in.nestedIndex < 0 ||
        static_cast<std::size_t>(in.nestedIndex) >= childClassNames.size()) {
        throw std::runtime_error("jvm_emitter: CLOSURE nestedIndex " +
                                 std::to_string(in.nestedIndex) +
                                 " has no assigned class name");
    }
    const std::string& cls =
        childClassNames[static_cast<std::size_t>(in.nestedIndex)];

    checkAllCapturesAreReported(e, in);

    int selfLoxSlot = findSelfCaptureLoxSlot(e, in);
    int selfJvmSlot = -1;
    if (selfLoxSlot >= 0) {
        selfJvmSlot = e.jvmSlotForLocal(selfLoxSlot);
        seedSelfCaptureCell(e, selfJvmSlot);
    }

    for (std::size_t u = 0; u < in.upvalues.size(); u++) {
        const ClosureUpvalue& up = in.upvalues[u];
        if (up.isLocal && up.index != selfLoxSlot) {
            ensureCapturedCell(e, e.jvmSlotForLocal(up.index), in.offset,
                               static_cast<int>(u));
        }
    }

    e.b.emit("new " + cls, +1);
    e.b.emit("dup", +1);
    e.b.emit(pushIntInstruction(static_cast<int>(in.upvalues.size())), +1);
    e.b.emit("anewarray [Ljava/lang/Object;", 0);
    for (std::size_t u = 0; u < in.upvalues.size(); u++) {
        const ClosureUpvalue& up = in.upvalues[u];
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(static_cast<int>(u)), +1);
        if (up.isLocal) {
            // `selfLoxSlot` already holds a fresh cell (seeded above), so
            // this is the same lowering as any other already-a-cell
            // capture — no special case needed here.
            e.b.emit("aload " + std::to_string(e.jvmSlotForLocal(up.index)),
                     +1);
            e.b.emit("checkcast [Ljava/lang/Object;", 0);
        } else {
            // A grandparent's own upvalue, already a cell — copy the
            // reference straight through, no seed (nodes/N7.md: "isLocal =
            // 0 takes the parent's own upvalue at that index").
            e.b.emit("aload 0", +1);
            e.b.emit("getfield lox/LoxClosure/upvalues [[Ljava/lang/Object;",
                     0);
            e.b.emit(pushIntInstruction(up.index), +1);
            e.b.emit("aaload", -1);
        }
        e.b.emit("aastore", -3);
    }
    e.b.emit("invokespecial " + cls + "/<init>([[Ljava/lang/Object;)V", -2);

    if (selfJvmSlot >= 0) {
        storeClosureIntoSelfCell(e, in, selfJvmSlot, selfLoxSlot);
    }
}

// CLASS name (P5/P6, node N8): builds a fresh, still-superclass-less
// LoxClass — vm.cpp's own CLASS handler does the same (an empty methods
// table, no superclass yet); INHERIT (below) is what later fills either in,
// on the classes that have one. `new; dup; ...; invokespecial <init>` keeps
// the ORIGINAL, un-dup'd reference as this opcode's own pushed result — the
// same idiom emitClosure already uses to build a generated LoxFn$<n>.
void emitClass(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("new lox/LoxClass", +1);
    e.b.emit("dup", +1);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("aconst_null", +1);
    e.b.emit(
        "invokespecial lox/LoxClass/<init>(Ljava/lang/String;Llox/LoxClass;)V",
        -3);
}

// INHERIT (node N8): compiler.cpp's fixed shape —
// `namedVariable(superclass); beginScope(); addLocal(super);
// markInitialized(); namedVariable(className); INHERIT` — means the
// superclass value is ALWAYS already the "super" invisible var by the time
// this instruction runs (the eager-materialization rule every other peek
// site in this file already assumes: R1, PR #107). It is never a live
// operand-stack temp here, so abstract_stack.cpp's `{1,0}` for INHERIT
// counts only the ONE thing that genuinely is one: the subclass, pushed by
// the immediately preceding, non-declaring `namedVariable(className)`.
// vm.cpp mutates the subclass IN PLACE (`subclass->methods.
// addAll(superclass->methods); subclass->superclass = superclass;`) — the
// merge LoxOps.inheritInto performs must land on the exact object identity
// DEFINE_GLOBAL/markInitialized already stored, not a freshly reconstructed
// one (LoxClass.inheritFrom's own note) — and vm.cpp's own "superclass
// stays on the stack as the super local" is already satisfied for free
// here: the super local's JVM slot never changes, so nothing needs
// pushing back for it. `loadNamedLocalAtZeroDepth` names and loads the
// super local — the same mechanism every other zero-depth consumer shares
// (referee decision, PR #113 round 3).
void emitInherit(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    loadNamedLocalAtZeroDepth(e, i, in.offset);
    e.b.emit("invokestatic lox/LoxOps/inheritInto(Ljava/lang/Object;Ljava/lang/"
             "Object;)V",
             -2);
}

// GET_PROPERTY name (node N8): field-before-method order and the exact
// error text live in LoxOps.getProperty (runtime/jvm) — this pass only
// supplies the receiver (already on the stack) and the constant name.
void emitGetProperty(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("invokestatic lox/LoxOps/getProperty(Ljava/lang/Object;Ljava/lang/"
             "String;)Ljava/lang/Object;",
             -1);
}

// SET_PROPERTY name (P2, node N8): `[obj,v] -> [v]` — the assigned value
// must survive the call, but it already sits ON TOP of the instance (not
// beneath it, the way GET_PROPERTY's receiver does), so it is spilled to
// `e.scratchSlot` while the constant name is pushed between them — the same
// shuffle globalsCall's own peek path uses for the identical reason.
void emitSetProperty(Emitter& e, const DecodedInstruction& in) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("astore " + scratch, -1);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("invokestatic lox/LoxOps/setProperty(Ljava/lang/Object;Ljava/lang/"
             "String;Ljava/lang/Object;)Ljava/lang/Object;",
             -2);
}

// DEFINE_METHOD name (P2, node N8): `[cls,fn] -> [cls]` — the class value
// must survive (the next method in the same class body, or the class
// body's own trailing POP, reads it again), so `dup` keeps a copy while the
// closure spills to `e.scratchSlot`. LoxOps.defineMethod takes concrete
// types — an existing, already-tested signature
// (runtime/jvm/test/lox/ClassesTest.java calls it directly with real
// LoxClass/LoxClosure values) — so both operands need an explicit
// `checkcast` here: the compiler guarantees this exact shape (a CLASS's own
// value, a CLOSURE's own result) on every real program, so a mismatch can
// only be an emitter bug, and a raw ClassCastException is an acceptable way
// to fail loudly on one — the same choice emitClosure's own array-build
// already makes for its own capture invariant.
void emitDefineMethod(Emitter& e, const DecodedInstruction& in) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("astore " + scratch, -1);
    e.b.emit("dup", +1);
    e.b.emit("checkcast lox/LoxClass", 0);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("checkcast lox/LoxClosure", 0);
    e.b.emit("invokestatic lox/LoxOps/defineMethod(Llox/LoxClass;Ljava/lang/"
             "String;Llox/LoxClosure;)V",
             -3);
}

// GET_SUPER name (node N8): vm.cpp pops the superclass (top), then binds
// `this` (now on top) to the found method. `this` was pushed by a
// PRECEDING GET_LOCAL 0 — super_() in compiler.cpp always pushes `this`
// before `super` — so `swap` alone reorders [this,superclass] into
// [superclass,this] with no extra slot; `this` then spills to
// `e.scratchSlot` while the constant name is pushed between the two.
void emitGetSuper(Emitter& e, const DecodedInstruction& in) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("swap", 0);
    e.b.emit("astore " + scratch, -1);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("aload " + scratch, +1);
    e.b.emit("invokestatic lox/LoxOps/getSuper(Ljava/lang/Object;Ljava/lang/"
             "String;Ljava/lang/Object;)Ljava/lang/Object;",
             -2);
}

// INSTANCEOF name (node N8): vm.cpp looks the class up BY NAME in globals,
// not from a constant-pool class reference (`m_globals.get(className,
// classVal)`) — LoxOps.instanceOf mirrors that exactly, so this pass only
// supplies the already-open globals receiver (e.globalsSlot, never re-typed
// away from lox/LoxGlobals — see globalsCall) and the constant name.
void emitInstanceof(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("aload " + std::to_string(e.globalsSlot), +1);
    e.b.emit("ldc \"" + e.constantString(in.constantIndex) + "\"", +1);
    e.b.emit("invokestatic lox/LoxOps/instanceOf(Ljava/lang/Object;Llox/"
             "LoxGlobals;Ljava/lang/String;)Z",
             -2);
    e.b.emit("invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;", 0);
}

// INVOKE name argc (P5+P6, node N8): the fused "get property then call"
// fast path — LoxOps.invoke keeps the field-before-method order (a field
// holding a function is called, never treated as a method, matching
// vm.cpp lines 518-533). argCount == 0 needs no reshuffle at all, same as
// emitCall's own argCount == 0 path: the receiver is already the sole,
// topmost value, so the name and the empty array build directly on top of
// it. argCount >= 1 reuses the exact same scratch slots emitCall does
// (e.calleeScratchSlot for the receiver, e.argScratchBase for the args) —
// computeMaxSpillWidth counts this opcode's own argCount alongside CALL's
// and BUILD_LIST's, so those slots are always wide enough.
void emitInvoke(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    std::string name = e.constantString(in.constantIndex);
    const char* invokeSig =
        "invokestatic "
        "lox/LoxOps/invoke(Ljava/lang/Object;Ljava/lang/String;[Ljava/lang/"
        "Object;)Ljava/lang/Object;";
    if (argCount == 0) {
        e.b.emit("ldc \"" + name + "\"", +1);
        e.b.emit(pushIntInstruction(0), +1);
        e.b.emit("anewarray java/lang/Object", 0);
        e.b.emit(invokeSig, -2);
        return;
    }
    for (int i = argCount - 1; i >= 0; i--) {
        e.b.emit("astore " + std::to_string(e.argScratchBase + i), -1);
    }
    e.b.emit("astore " + std::to_string(e.calleeScratchSlot), -1);

    e.b.emit("aload " + std::to_string(e.calleeScratchSlot), +1);
    e.b.emit("ldc \"" + name + "\"", +1);
    e.b.emit(pushIntInstruction(argCount), +1);
    e.b.emit("anewarray java/lang/Object", 0);
    for (int i = 0; i < argCount; i++) {
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aload " + std::to_string(e.argScratchBase + i), +1);
        e.b.emit("aastore", -3);
    }
    e.b.emit(invokeSig, -2);
}

// SUPER_INVOKE name argc (node N8): `[self,arg0..argN-1,superclassVal] ->
// [result]` — vm.cpp pops the superclass first (top), then calls with
// self at its usual receiver position. argCount == 0 reduces to the same
// `swap` plus one-scratch shuffle emitGetSuper uses, with an empty array in
// place of a bound method. argCount >= 1 additionally spills self/args
// exactly as emitInvoke does, into its own scratch slots, plus
// `e.scratchSlot` for the superclass — three DISTINCT, already-existing
// slots, since one instruction's own shuffle never overlaps another's.
void emitSuperInvoke(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    std::string name = e.constantString(in.constantIndex);
    std::string scratch = std::to_string(e.scratchSlot);
    const char* superInvokeSig =
        "invokestatic "
        "lox/LoxOps/superInvoke(Ljava/lang/Object;Ljava/lang/String;Ljava/"
        "lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;";
    if (argCount == 0) {
        e.b.emit("swap", 0);
        e.b.emit("astore " + scratch, -1);
        e.b.emit("ldc \"" + name + "\"", +1);
        e.b.emit("aload " + scratch, +1);
        e.b.emit(pushIntInstruction(0), +1);
        e.b.emit("anewarray java/lang/Object", 0);
        e.b.emit(superInvokeSig, -3);
        return;
    }
    e.b.emit("astore " + scratch, -1); // superclassVal (top)
    for (int i = argCount - 1; i >= 0; i--) {
        e.b.emit("astore " + std::to_string(e.argScratchBase + i), -1);
    }
    e.b.emit("astore " + std::to_string(e.calleeScratchSlot), -1); // self

    e.b.emit("aload " + scratch, +1);
    e.b.emit("ldc \"" + name + "\"", +1);
    e.b.emit("aload " + std::to_string(e.calleeScratchSlot), +1);
    e.b.emit(pushIntInstruction(argCount), +1);
    e.b.emit("anewarray java/lang/Object", 0);
    for (int i = 0; i < argCount; i++) {
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aload " + std::to_string(e.argScratchBase + i), +1);
        e.b.emit("aastore", -3);
    }
    e.b.emit(superInvokeSig, -3);
}

// GET_TAG's own instruction, standalone (node N10): the sparse,
// compare-and-branch match form (compiler.cpp: GET_TAG, then CONSTANT, then
// EQUAL, once per arm), used when previewEnumArms rejects the table
// dispatch — a guard, an or-pattern, an @-binding, or a non-dense tag set
// all fall back to this shape (compiler.cpp:1536's own comment). LoxOps.
// getTag returns the tag as a primitive double, the same as vm.cpp's own
// Number result; box it exactly the way emitConstant boxes a number
// literal, so the CONSTANT/EQUAL pair right after sees the same
// Ljava/lang/Object; shape any other comparison operand does.
void emitGetTag(Emitter& e) {
    e.b.emit("invokestatic lox/LoxOps/getTag(Ljava/lang/Object;)D", +1);
    e.b.emit("invokestatic java/lang/Double/valueOf(D)Ljava/lang/Double;", -1);
}

// GET_TAG fused with an immediately following JUMP_TABLE (P8's own hazard:
// GET_TAG pushes a boxed double and JUMP_TABLE wants an int — a
// context-free lowering would box, then immediately unbox, for nothing).
// `getTag()D; d2i; tableswitch` keeps the tag a primitive the whole way.
// `min` is the switch base; one label per arm, in tag order — chunk_
// decoder.cpp already resolved every arm's own absolute target from the raw
// forward-offset bytes, so this reads that, not the bytes themselves.
// `default` targets the offset right after the table: compileMatchBody
// always places a real MATCH_ERROR there (emitMatchError, below), and the
// CFG (N1) already gave that offset its own label, the same as any other
// block leader — this pass does not special-case it.
void emitFusedGetTagJumpTable(Emitter& e, const DecodedInstruction& table) {
    e.b.emit("invokestatic lox/LoxOps/getTag(Ljava/lang/Object;)D", +1);
    e.b.emit("d2i", -1);
    std::ostringstream sw;
    sw << "tableswitch " << table.minTag << "\n";
    for (const JumpTableArm& arm : table.jumpTable) {
        sw << "        " << e.labelFor(arm.target) << "\n";
    }
    sw << "        default : " << e.labelFor(table.offset + table.length);
    e.b.emit(sw.str(), -1);
}

// GET_TAG's own dispatch case: fuse with a following JUMP_TABLE when one is
// there (`fusableJumpTable`'s own note), matching SET_LOCAL/SET_GLOBAL/
// SET_UPVALUE's own POP-fusion shape (`consumedFollowingPop`) — the caller
// skips the JUMP_TABLE's own array slot instead of dispatching it a second
// time.
void emitGetTagOrFused(Emitter& e, std::size_t i,
                       bool& consumedFollowingJumpTable) {
    if (e.fusableJumpTable(i)) {
        emitFusedGetTagJumpTable(e, e.fn.instructions[i + 1]);
        consumedFollowingJumpTable = true;
    } else {
        emitGetTag(e);
    }
}

// MATCH_ERROR (node N8 pulls this one N10 opcode forward — see
// jvm_emitter.h's own note: a `match` whose arms are all class patterns
// compiles a real, reachable MATCH_ERROR, because the compiler never
// proves a class pattern exhaustive over its own subclasses;
// examples/class_dispatch.lox's `area` function is exactly this shape).
// vm.cpp's own handler never returns, so this pass has no successor it
// needs to reach here either: like RETURN, nothing physically after it is
// entered by fall-through (finishInstruction already excludes both alike).
//
// R4 fix (PR #113 round 1): that claim must be true of the EMITTED bytecode,
// not only of this pass's own analysis. An earlier version called a plain
// void `matchError()V`; the JVM verifier does not know a void call always
// throws, so it still treats the next instruction as reachable from it,
// even though this pass never emits a physical edge there. `LoxOps.
// matchError` now BUILDS the error instead of throwing it, so the call
// leaves the error object on the stack, and `athrow` — a real terminal
// instruction, like `areturn` or `goto` — ends the block. The claim in this
// comment is now true of the bytecode too, which N10 needs when
// MATCH_ERROR becomes `tableswitch`'s default target.
void emitMatchError(Emitter& e) {
    e.b.emit("invokestatic lox/LoxOps/matchError()Llox/LoxError;", +1);
    e.b.emit("athrow", -1);
}

// RETURN's two roles (P5): a function's own RETURN hands its value back to
// the caller through `invoke`'s own return type; the script's ends `void
// main`, so the NIL;RETURN endCompiler() always appends there just drops
// its value on the floor.
//
// bytecode-translation-problems.md, "RETURN can return a named local, not
// only a temporary": a `match` expression whose arm ends in a plain
// expression (examples/class_dispatch.lox's area()/describe()) leaves its
// synthetic result sitting in a local slot, not a genuine operand-stack
// temp — measured at 33 sites across examples/ and
// bootstrap/loxpp_interpreter.lox, zero among the translation probes, so
// this node's checkpoint is the first to exercise it end-to-end.
// `before[i].operandDepth() == 0` names the shape; `loadNamedLocalAtZeroDepth`
// (this file, above emitGetLocal) names and loads the right slot — see its
// own note for why `lastInvisibleVarSlot` alone is the wrong tracker (R6, PR
// #113 round 2), and for the merge/captured-slot cross-check it now applies
// instead (R2/R5, same PR; redesigned in round 3).
void emitReturn(Emitter& e, std::size_t i, bool isScript) {
    if (isScript) {
        // vm.cpp: frameCount reaches 0, result discarded.
        e.b.emit("return", 0);
        return;
    }
    if (e.analysis.before[i].operandDepth() == 0) {
        loadNamedLocalAtZeroDepth(e, i, e.fn.instructions[i].offset);
    }
    e.b.emit("areturn", -1);
}

// The `<init>` every generated LoxFn$<n> needs (jvm_emitter.h hazard note):
// calls straight through to LoxClosure's own constructor with this
// function's compile-time name/arity as literals, so only the upvalues
// array is a real parameter. N7 fills that array with real cells later;
// this shape does not change.
std::string emitConstructorMethod(const DecodedFunction& fn) {
    std::ostringstream out;
    out << ".method public <init>([[Ljava/lang/Object;)V\n";
    out << "    .limit stack 4\n";
    out << "    .limit locals 2\n";
    out << "    aload 0\n";
    if (fn.function->name != nullptr) {
        out << "    ldc \""
            << escapeJasminString(std::string(fn.function->name->chars))
            << "\"\n";
    } else {
        out << "    aconst_null\n";
    }
    out << "    " << pushIntInstruction(fn.function->arity) << "\n";
    out << "    aload 1\n";
    out << "    invokespecial "
           "lox/LoxClosure/<init>(Ljava/lang/String;I[[Ljava/lang/Object;)V\n";
    out << "    return\n";
    out << ".end method\n\n";
    return out.str();
}

// High-water mark of concurrently-bound Lox frame slots (arity+1 at entry,
// growing with every `var`) — the JVM local array must hold all of them at
// their fixed positions, same as the abstract stack does.
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

// The widest N-element spill this chunk needs — CALL's/INVOKE's/
// SUPER_INVOKE's argCount, BUILD_LIST's element count (node N7 pulls
// BUILD_LIST forward from N9's scope; see emitBuildList's own note), or
// BUILD_MAP's own width, twice its pair count (emitBuildMap spills key and
// value separately) — ignoring a width of 0 (needs no scratch slot at all:
// emitCall's, emitInvoke's, and emitSuperInvoke's own argCount==0 paths, and
// emitBuildList's/emitBuildMap's own count==0 path, each build directly
// with no spill). 0 here means the chunk needs no scratch slots for any of
// these families, keeping `.limit locals` byte-identical to pre-N6 output
// on every chunk that makes no call, invokes no method, and builds no list
// or map.
int computeMaxSpillWidth(const DecodedFunction& fn) {
    int maxWidth = 0;
    for (const DecodedInstruction& instr : fn.instructions) {
        if (instr.op == Op::CALL || instr.op == Op::BUILD_LIST ||
            instr.op == Op::INVOKE || instr.op == Op::SUPER_INVOKE) {
            maxWidth = std::max(maxWidth, instr.byteOperand);
        } else if (instr.op == Op::BUILD_MAP) {
            maxWidth = std::max(maxWidth, 2 * instr.byteOperand);
        }
    }
    return maxWidth;
}

// Builds the Emitter for one chunk: the slot layout (jvm_emitter.h) plus
// every per-offset lookup table the opcode-family functions above read from.
Emitter buildEmitter(const DecodedFunction& fn,
                     const FunctionStackAnalysis& analysis, bool isScript,
                     int maxLocalCount, int maxSpillWidth,
                     const FunctionCaptureInfo& captureInfo) {
    Emitter e{fn, analysis, {}};
    e.baseSlot = isScript ? 2 : 4;
    e.globalsSlot = e.baseSlot - 1;
    e.scratchSlot = e.baseSlot + maxLocalCount;
    if (maxSpillWidth > 0) {
        // calleeScratchSlot is CALL's own extra slot (emitCall) —
        // emitBuildList spills only into argScratchBase, one slot per
        // element, and never touches calleeScratchSlot; reserving it
        // unconditionally here is simpler than tracking whether THIS
        // chunk's widest spill came from a CALL or a BUILD_LIST.
        e.calleeScratchSlot = e.scratchSlot + 1;
        e.argScratchBase = e.scratchSlot + 2;
    }

    for (const PopClassification& p : analysis.pops) {
        e.popKinds[p.offset] = p.kind;
    }
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        e.invisibleVarsByOffset[site.offset].push_back(site.slot);
    }
    // N7: which of this chunk's OWN local slots ever back an Object[1]
    // cell — every slot some reachable CLOSURE in this chunk captures
    // (capture_analysis.h). Membership only; see ensureCapturedCell's own
    // note for why the exact live range is not what GET_LOCAL/SET_LOCAL/
    // CLOSURE need from N3 here.
    for (const auto& entry : captureInfo.liveRangesBySlot) {
        e.capturedSlots.insert(entry.first);
    }

    // N5: one jasmin label per N1 block leader. A leader with no predecessor
    // (e.g. the fall-through after an unconditional JUMP) still gets a label;
    // an unreferenced jasmin label is harmless, so this pass does not bother
    // filtering to only-referenced offsets.
    Cfg cfg = buildCfg(fn.instructions);
    e.labelAtOffset.reserve(cfg.blocks.size());
    for (const BasicBlock& block : cfg.blocks) {
        e.labelAtOffset.emplace(block.leaderOffset, block.label);
    }
    return e;
}

// The globals reference and, for a function chunk only, the argument
// prologue (P5): `invoke`'s own JVM parameters are `self` (slot 1) and
// `args` (slot 2, an Object[]). Copies `self` into the Lox frame's own
// slot-0 mirror and unpacks args[i] into slot i+1's — the fixed mapping
// every opcode-family function above assumes. A script chunk has no
// argument prologue at all (frame slot 0 is the script's own never-read
// callee, same as before N6).
void emitPrologue(Emitter& e, const DecodedFunction& fn, bool isScript) {
    if (isScript) {
        e.b.emit("invokestatic lox/LoxRuntime/init()Llox/LoxGlobals;", +1);
        e.b.emit("astore " + std::to_string(e.globalsSlot), -1);
        return;
    }
    // A generated class has no field of its own for the shared globals
    // instance (jvm_emitter.h) — read the one instance init() built.
    e.b.emit("invokestatic lox/LoxRuntime/current()Llox/LoxGlobals;", +1);
    e.b.emit("astore " + std::to_string(e.globalsSlot), -1);
    e.b.emit("aload 1", +1);
    e.b.emit("astore " + std::to_string(e.jvmSlotForLocal(0)), -1);
    int arity = fn.function->arity;
    for (int i = 0; i < arity; i++) {
        e.b.emit("aload 2", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aaload", -1);
        e.b.emit("astore " + std::to_string(e.jvmSlotForLocal(i + 1)), -1);
    }
}

// The part of one instruction's handling that is not the opcode's own
// concern: storing any invisible-var slot this offset assigns, then
// computing the next walk index and the fall-through carry the *next*
// iteration's label-resync test (see emitBody) reads. Split out of emitBody
// itself (PR #110 R1) to keep that function's cognitive complexity below
// the threshold N7, N9, and N10 still need room under.
std::size_t finishInstruction(Emitter& e, std::size_t i,
                              const DecodedInstruction& in,
                              bool consumedFollowingPop,
                              bool consumedFollowingJumpTable) {
    auto varsIt = e.invisibleVarsByOffset.find(in.offset);
    if (varsIt != e.invisibleVarsByOffset.end()) {
        for (int slot : varsIt->second) {
            e.b.emit("astore " + std::to_string(e.jvmSlotForLocal(slot)), -1);
            e.lastInvisibleVarSlot = slot;
        }
    }

    std::size_t nextIndex =
        i + (consumedFollowingPop || consumedFollowingJumpTable ? 2 : 1);
    // R6 fix (PR #115 round 1): `in.op` reads GET_TAG here whenever this
    // instruction fused away a following JUMP_TABLE — GET_TAG alone falls
    // through, but the emitted `tableswitch` never does (every arm and the
    // default are real jump targets, same as JUMP/LOOP/MATCH_ERROR). Reading
    // `in.op` alone would call this a fall-through and let the next block
    // leader's `trustCarryForward` (emitBody) skip its resync for the wrong
    // reason — today it stays right only because a table's own entry depth
    // and this pass's post-fusion depth both happen to be equal (see
    // emitFusedGetTagJumpTable's own note), not because the check is sound.
    e.prevCanFallThrough = in.op != Op::JUMP && in.op != Op::LOOP &&
                           in.op != Op::RETURN && in.op != Op::MATCH_ERROR &&
                           !consumedFollowingJumpTable;
    e.prevNaturalSuccessorOffset = (nextIndex < e.fn.instructions.size())
                                       ? e.fn.instructions[nextIndex].offset
                                       : -1;
    return nextIndex;
}

// Walks every instruction once, in offset order, dispatching each to its
// opcode-family function. The parts that are not one opcode's own concern —
// labels, the R1 depth safety net, and the invisible-var/fall-through
// bookkeeping finishInstruction does — stay here rather than in any one
// case.
void emitBody(Emitter& e, bool isScript,
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
        bool consumedFollowingJumpTable = false;

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
                e.b.resync(e.analysis.before[i].operandDepth());
            }
        }

        // R1 safety net (PR #107 round 1): every correctly-lowered opcode in
        // this pass keeps the JVM operand stack's physical depth equal to
        // N2's own operandDepth() at the same offset — a temp this emitter
        // pushed is the only thing N2 counts as "on the stack". A mismatch
        // here means a peek is about to dup or pop a cell that is not
        // physically there, so abort loudly instead of letting jasmin or the
        // JVM verifier find it.
        if (e.b.depth != e.analysis.before[i].operandDepth()) {
            throw std::runtime_error(
                "jvm_emitter: simulated stack depth " +
                std::to_string(e.b.depth) + " disagrees with analysis depth " +
                std::to_string(e.analysis.before[i].operandDepth()) +
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
        case Op::PRINT:
            emitPrint(e, i, in);
            break;
        case Op::DEFINE_GLOBAL:
            emitDefineGlobal(e, i, in);
            break;
        case Op::GET_GLOBAL:
            emitGetGlobal(e, in);
            break;
        case Op::SET_GLOBAL:
            emitSetGlobal(e, i, in, consumedFollowingPop);
            break;
        case Op::GET_UPVALUE:
            emitGetUpvalue(e, in);
            break;
        case Op::SET_UPVALUE:
            emitSetUpvalue(e, i, in, consumedFollowingPop);
            break;
        case Op::CLOSE_UPVALUE:
            // A local reclaim (mission brief 5c: endScope/emitLoopCleanup
            // emit this exactly where a POP would otherwise retire a
            // captured local) — nothing on the JVM operand stack to pop,
            // same as emitPop's own LOCAL_RECLAIM case. The cell it ends
            // stays wherever it already sits; the NEXT declaration into
            // that same JVM slot always re-`astore`s it directly (never
            // through ensureCapturedCell's check), which is what actually
            // puts the slot back to raw for whatever comes after — see
            // ensureCapturedCell's own note.
            //
            // This one `break` covers every close alike — reachable,
            // unreachable, or statically dead (capture_analysis.h's own
            // fields for those). It never reads any of them: emitting no
            // bytecode at all already satisfies the "pop alone, no cell
            // operation" rule those fields describe, for every kind of
            // close, so there is nothing left for this case to branch on
            // (PR #111 R7).
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
        case Op::BUILD_LIST:
            emitBuildList(e, i, in);
            break;
        case Op::BUILD_MAP:
            emitBuildMap(e, in);
            break;
        case Op::NOT:
            emitNot(e, i, in);
            break;
        case Op::GET_INDEX:
            emitGetIndex(e, i, in);
            break;
        case Op::GET_ITER:
            emitGetIter(e, i, in);
            break;
        case Op::CLOSURE:
            emitClosure(e, in, childClassNames);
            break;
        case Op::RETURN:
            emitReturn(e, i, isScript);
            break;
        case Op::CLASS:
            emitClass(e, in);
            break;
        case Op::INHERIT:
            emitInherit(e, i, in);
            break;
        case Op::GET_PROPERTY:
            emitGetProperty(e, in);
            break;
        case Op::SET_PROPERTY:
            emitSetProperty(e, in);
            break;
        case Op::DEFINE_METHOD:
            emitDefineMethod(e, in);
            break;
        case Op::GET_SUPER:
            emitGetSuper(e, in);
            break;
        case Op::INSTANCEOF:
            emitInstanceof(e, in);
            break;
        case Op::INVOKE:
            emitInvoke(e, in);
            break;
        case Op::SUPER_INVOKE:
            emitSuperInvoke(e, in);
            break;
        case Op::MATCH_ERROR:
            emitMatchError(e);
            break;
        case Op::GET_TAG:
            emitGetTagOrFused(e, i, consumedFollowingJumpTable);
            break;
        default:
            if (!emitSimpleOp(e, in.op)) {
                notImplemented(in.op);
            }
        }

        i = finishInstruction(e, i, in, consumedFollowingPop,
                              consumedFollowingJumpTable);
    }
}

// The class header, the method this chunk becomes (`main` or `invoke`, plus
// `<init>` for a function chunk), and the `.limit` directives measured from
// what emitBody actually produced.
std::string assembleClass(const Emitter& e, const DecodedFunction& fn,
                          const std::string& className, bool isScript,
                          int extraSpillSlots) {
    std::ostringstream out;
    out << ".class public " << className << "\n";
    if (isScript) {
        out << ".super java/lang/Object\n\n";
        out << ".method public static main([Ljava/lang/String;)V\n";
    } else {
        out << ".super lox/LoxClosure\n\n";
        out << emitConstructorMethod(fn);
        out << ".method protected invoke(Ljava/lang/Object;[Ljava/lang/"
               "Object;)Ljava/lang/Object;\n";
    }
    out << "    .limit stack " << std::max(1, e.b.maxDepth) << "\n";
    out << "    .limit locals " << (e.scratchSlot + 1 + extraSpillSlots)
        << "\n\n";
    out << e.b.text.str();
    out << ".end method\n";
    return out.str();
}

// The shared lowering pass for one chunk, script or function alike (node
// N6 unifies what were two near-duplicate passes: see jvm_emitter.h's
// layout comment for the slot-mapping difference the two `isScript` values
// select). `childClassNames[i]` names the class this chunk's own i-th
// nested function was assigned — see emitProgram. `captureInfo` is this
// same chunk's own entry from analyzeCaptures (node N3) — see
// buildEmitter's use of it.
std::string emitChunk(const DecodedFunction& fn,
                      const FunctionStackAnalysis& analysis,
                      const std::string& className, bool isScript,
                      const std::vector<std::string>& childClassNames,
                      const FunctionCaptureInfo& captureInfo) {
    int maxLocalCount = computeMaxLocalCount(analysis);
    int maxSpillWidth = computeMaxSpillWidth(fn);
    int extraSpillSlots = maxSpillWidth > 0 ? maxSpillWidth + 1 : 0;

    Emitter e = buildEmitter(fn, analysis, isScript, maxLocalCount,
                             maxSpillWidth, captureInfo);
    emitPrologue(e, fn, isScript);
    emitBody(e, isScript, childClassNames);
    return assembleClass(e, fn, className, isScript, extraSpillSlots);
}

// Assigns every node in the decoded tree a stable class name, by one fixed
// pre-order walk (brief.md section 9: "deterministic naming"): the root
// becomes `scriptClassName`, and every other node becomes `LoxFn$<n>` in
// visit order, counted across the *whole* tree, not per parent — two
// sibling functions and a great-grandchild all draw from the same counter.
// Keyed by DecodedFunction::id (stable, name-independent) rather than a
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
             const CaptureAnalysis& captures, std::vector<EmittedClass>& out) {
    std::vector<std::string> childClassNames;
    childClassNames.reserve(fn.nested.size());
    for (const DecodedFunction& child : fn.nested) {
        childClassNames.push_back(names.at(child.id));
    }
    const std::string& className = names.at(fn.id);
    std::string source =
        emitChunk(fn, node.self, className, isRoot, childClassNames,
                  captures.functions.at(fn.id));
    out.push_back(EmittedClass{className, std::move(source)});

    for (std::size_t i = 0; i < fn.nested.size(); i++) {
        emitAll(fn.nested[i], node.nested[i], /*isRoot=*/false, names, captures,
                out);
    }
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

std::vector<EmittedClass> emitProgram(const DecodedFunction& root,
                                      const StackAnalysisTree& tree,
                                      const std::string& scriptClassName) {
    std::unordered_map<std::string, std::string> names;
    int counter = 0;
    assignClassNames(root, /*isRoot=*/true, scriptClassName, counter, names);

    CaptureAnalysis captures = analyzeCaptures(root);

    std::vector<EmittedClass> out;
    emitAll(root, tree, /*isRoot=*/true, names, captures, out);
    return out;
}

std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& className) {
    CaptureAnalysis captures = analyzeCaptures(fn);
    return emitChunk(fn, analysis, className, /*isScript=*/true, {},
                     captures.functions.at(fn.id));
}

} // namespace jvm
