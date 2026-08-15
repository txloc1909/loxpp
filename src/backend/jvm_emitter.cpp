#include "jvm_emitter.h"

#include "capture_analysis.h"
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
    // CALL with at least one argument or a BUILD_LIST with at least one
    // element; -1 otherwise, so a stray use before emitChunk's own prologue
    // computes them fails loudly instead of silently aliasing scratchSlot.
    // emitBuildList only ever uses argScratchBase, never calleeScratchSlot —
    // see computeMaxSpillWidth's own note.
    int calleeScratchSlot{-1};
    int argScratchBase{-1};

    // R4 fix (PR #109 nit): -1 is a sentinel for "no invisible-var site has
    // run yet", not a real slot — see loadLastInvisibleVar.
    int lastInvisibleVarSlot{-1};

    // R1 fix (PR #111 round 2): true only right after emitClosure's
    // self-capture path stores a CELL into `lastInvisibleVarSlot` instead of
    // the raw closure a plain declaration always stores. No probe or
    // example reaches a peek of a self-recursive local `fun`'s own value
    // (the grammar makes such a declaration a statement, never an operand),
    // so loadLastInvisibleVar throws there rather than silently handing out
    // a cell where every other caller expects a raw value.
    bool lastInvisibleVarIsSelfCell{false};

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
        if (lastInvisibleVarIsSelfCell) {
            throw std::runtime_error(
                "jvm_emitter: peek of a self-capturing closure's own "
                "invisible var is unsupported — the slot holds a cell, not "
                "the raw closure a plain aload here would assume");
        }
        int sourceSlot = jvmSlotForLocal(lastInvisibleVarSlot);
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
// N6 tried to build a program where the two disagree (nodes/N6.md, "the
// merge-divergence test becomes writable here") and could not: RETURN, like
// JUMP/LOOP/MATCH_ERROR, never falls through (see prevCanFallThrough below
// and abstract_stack.cpp's own terminator list), so a path that returns
// contributes no edge to any later merge in this chunk at all — it is a
// dead end, not one side of a join. CALL and CLOSURE are straight-line, no
// new edges either. So a function frame's *own* body still merges only
// through if/else/while/for, exactly the shapes N5 already showed agree.
// See the PR body for the two programs tried and N10 (match arms) as the
// next candidate construct.
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
    // GET_INDEX/SET_INDEX (node N7 pulls these two opcodes forward from N9's
    // scope — see emitBuildList's own note: V1_fresh_cell/V3_loopvar, N7's
    // own checkpoint probes, both index a list of the closures under test).
    // vm.cpp's own operand order already matches LoxOps's parameter order
    // exactly ([collection, index] and [collection, index, value], bottom
    // to top), so — unlike SET_LOCAL/SET_GLOBAL/SET_UPVALUE — neither is a
    // P2 peek: vm.cpp pops SET_INDEX's operands whole and pushes a genuinely
    // new result cell (P2's own peek family list, abstract_stack.h, does not
    // include it), so a plain call with no dup is exactly right.
    case Op::GET_INDEX:
        e.b.emit("invokestatic "
                 "lox/LoxOps/getIndex(Ljava/lang/Object;Ljava/lang/Object;)"
                 "Ljava/lang/Object;",
                 -1);
        return true;
    case Op::SET_INDEX:
        e.b.emit("invokestatic "
                 "lox/LoxOps/setIndex(Ljava/lang/Object;Ljava/lang/Object;"
                 "Ljava/lang/Object;)Ljava/lang/Object;",
                 -2);
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
    // JVM operand stack to `dup`. Load it back from its slot instead:
    // `lastInvisibleVarSlot`, not `before[i].localCount`, names it (R9,
    // resolved for N5 — localCount is only an upper bound at a CFG merge,
    // but lastInvisibleVarSlot is this pass's own forward walk, so it is
    // exact regardless of merges upstream). That declaring push reloads the
    // raw value, whether or not THIS slot is captured somewhere else in the
    // chunk — EXCEPT one case (PR #111 R1/R4): a local `fun` that captures
    // itself stores a CELL there instead, through emitClosure's own
    // self-capture path, not a plain astore. loadLastInvisibleVar throws in
    // that one case rather than handing back a cell where every caller here
    // expects the raw value (see the self-capture note above
    // ensureCapturedCell, and lastInvisibleVarIsSelfCell's own comment).
    if (e.analysis.before[i].operandDepth() == 0) {
        e.loadLastInvisibleVar();
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
        e.loadLastInvisibleVar();
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
// vm.cpp's own operand order — so BUILD_LIST's own spill-to-scratch (same
// P7 reasoning as emitCall: the elements sit on the stack BELOW where a
// fresh array reference would land, count == 0 needs no scratch at all) is
// the only new shuffle this addition needs. LoxOps.buildList (runtime/jvm)
// copies the array into a fresh LoxList in the same order.
void emitBuildList(Emitter& e, const DecodedInstruction& in) {
    int count = in.byteOperand;
    const char* buildSig =
        "invokestatic lox/LoxOps/buildList([Ljava/lang/Object;)Llox/"
        "LoxList;";
    if (count == 0) {
        e.b.emit(pushIntInstruction(0), +1);
        e.b.emit("anewarray java/lang/Object", 0);
        e.b.emit(buildSig, 0);
        return;
    }
    for (int i = count - 1; i >= 0; i--) {
        e.b.emit("astore " + std::to_string(e.argScratchBase + i), -1);
    }
    e.b.emit(pushIntInstruction(count), +1);
    e.b.emit("anewarray java/lang/Object", 0);
    for (int i = 0; i < count; i++) {
        e.b.emit("dup", +1);
        e.b.emit(pushIntInstruction(i), +1);
        e.b.emit("aload " + std::to_string(e.argScratchBase + i), +1);
        e.b.emit("aastore", -3);
    }
    e.b.emit(buildSig, 0);
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
    e.lastInvisibleVarSlot = selfLoxSlot;
    e.lastInvisibleVarIsSelfCell = true;
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

// RETURN's two roles (P5): a function's own RETURN hands its value back to
// the caller through `invoke`'s own return type; the script's ends `void
// main`, so the NIL;RETURN endCompiler() always appends there just drops
// its value on the floor.
void emitReturn(Emitter& e, bool isScript) {
    if (isScript) {
        // vm.cpp: frameCount reaches 0, result discarded.
        e.b.emit("return", 0);
    } else {
        e.b.emit("areturn", -1);
    }
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

// The widest N-element spill this chunk needs — CALL's argCount, or
// BUILD_LIST's element count (node N7 pulls BUILD_LIST forward from N9's
// scope; see emitBuildList's own note) — ignoring a width of 0 (needs no
// scratch slot at all: emitCall's argCount==0 path, and emitBuildList's own
// count==0 path, each build directly with no spill). 0 here means the
// chunk needs no scratch slots for either family, keeping `.limit locals`
// byte-identical to pre-N6 output on every chunk that makes no call and
// builds no list.
int computeMaxSpillWidth(const DecodedFunction& fn) {
    int maxWidth = 0;
    for (const DecodedInstruction& instr : fn.instructions) {
        if (instr.op == Op::CALL || instr.op == Op::BUILD_LIST) {
            maxWidth = std::max(maxWidth, instr.byteOperand);
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
                              bool consumedFollowingPop) {
    auto varsIt = e.invisibleVarsByOffset.find(in.offset);
    if (varsIt != e.invisibleVarsByOffset.end()) {
        for (int slot : varsIt->second) {
            e.b.emit("astore " + std::to_string(e.jvmSlotForLocal(slot)), -1);
            e.lastInvisibleVarSlot = slot;
            // A plain declaration always stores the raw value here — the
            // self-capture flag only ever means something right after
            // emitClosure's own special-cased store (below), which erases
            // its slot from this map before finishInstruction runs, so a
            // normal site reaching this loop is never that case.
            e.lastInvisibleVarIsSelfCell = false;
        }
    }

    std::size_t nextIndex = i + (consumedFollowingPop ? 2 : 1);
    e.prevCanFallThrough = in.op != Op::JUMP && in.op != Op::LOOP &&
                           in.op != Op::RETURN && in.op != Op::MATCH_ERROR;
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
        case Op::DEFINE_GLOBAL:
            emitDefineGlobal(e, in);
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
            emitBuildList(e, in);
            break;
        case Op::CLOSURE:
            emitClosure(e, in, childClassNames);
            break;
        case Op::RETURN:
            emitReturn(e, isScript);
            break;
        default:
            if (!emitSimpleOp(e, in.op)) {
                notImplemented(in.op);
            }
        }

        i = finishInstruction(e, i, in, consumedFollowingPop);
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
