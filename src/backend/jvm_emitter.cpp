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

// Every Op enumerator's own spelling — for the "not implemented in N4:"
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

    constexpr int globalsSlot = 1;
    const int scratchSlot = 2 + maxLocalCount;
    auto jvmSlotForLocal = [](int loxSlot) { return 2 + loxSlot; };

    std::unordered_map<int, PopKind> popKinds;
    for (const PopClassification& p : analysis.pops) {
        popKinds[p.offset] = p.kind;
    }
    std::unordered_map<int, std::vector<int>> invisibleVarsByOffset;
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        invisibleVarsByOffset[site.offset].push_back(site.slot);
    }

    const std::vector<DecodedInstruction>& ins = fn.instructions;
    std::size_t n = ins.size();
    auto reached = [&](std::size_t idx) {
        return idx < analysis.reached.size() &&
               static_cast<bool>(analysis.reached[idx]);
    };

    // N5: one jasmin label per N1 block leader. A leader with no predecessor
    // (e.g. the fall-through after an unconditional JUMP) still gets a label;
    // an unreferenced jasmin label is harmless, so this pass does not bother
    // filtering to only-referenced offsets.
    Cfg cfg = buildCfg(ins);
    std::unordered_map<int, std::string> labelAtOffset;
    labelAtOffset.reserve(cfg.blocks.size());
    for (const BasicBlock& block : cfg.blocks) {
        labelAtOffset.emplace(block.leaderOffset, block.label);
    }
    auto labelFor = [&](int offset) -> const std::string& {
        auto it = labelAtOffset.find(offset);
        if (it == labelAtOffset.end()) {
            throw std::runtime_error(
                "jvm_emitter: no CFG label at jump target " +
                std::to_string(offset));
        }
        return it->second;
    };

    // SET_LOCAL/SET_GLOBAL peek (the value stays on the stack); a following
    // POP that N2 already proved TEMP is exactly that peeked value being
    // discarded as a statement result, so folding the pair into one plain
    // store — what javac emits for the same idiom — needs no dup and no
    // separate pop (bytecode-translation-problems.md P2).
    auto fusablePop = [&](std::size_t i) -> bool {
        std::size_t j = i + 1;
        if (j >= n || !reached(j) || ins[j].op != Op::POP) {
            return false;
        }
        // PR #109 R1 fix: a POP that is a CFG block leader must stay a real
        // instruction. Every edge into that leader needs its jasmin label
        // (fusing away the instruction fuses away the label with it — a
        // jasmin assemble error, not a wrong result), and the short-circuit
        // edge into this leader carries its own copy of the condition, which
        // needs a real `pop` of its own regardless of what the fall-through
        // edge does with its copy.
        if (labelAtOffset.find(ins[j].offset) != labelAtOffset.end()) {
            return false;
        }
        auto it = popKinds.find(ins[j].offset);
        return it != popKinds.end() && it->second == PopKind::TEMP;
    };

    // N5.md, "inherited from N4": `analysis.before[i].localCount` is only an
    // upper bound at a CFG merge (abstract_stack.h), so the SET_LOCAL/
    // SET_GLOBAL peek-of-a-named-local case (below) cannot use it to name the
    // slot a peek reads once JUMP/JUMP_IF_FALSE/LOOP exist. This tracks the
    // same fact a different, merge-safe way: the slot an invisible-var site
    // just bound, updated only by this pass's own forward walk in offset
    // order (below, alongside the existing invisible-var store), never by a
    // count aggregated across incoming edges.
    //
    // PR #109 R3: no program in this node's opcode set (if/else, while, for —
    // no functions, no match) has yet made `lastInvisibleVarSlot` disagree
    // with `jvmSlotForLocal(before[i].localCount - 1)` at the offsets that
    // read it. The reason is the scope-exit rule (brief 5c): `endScope` and
    // `emitLoopCleanup` retire every local a block declared, on every path
    // out of that block, before control ever reaches a point outside it. Two
    // edges into one merge can therefore disagree on `localCount` only
    // through a construct that leaves a *different* number of locals live
    // past that merge on each path — a function frame or a `match` arm,
    // neither in scope here (N6, N10). `lastInvisibleVarSlot` still stays,
    // not `localCount`, because it costs nothing today and reads correctly
    // the moment either later node breaks that premise.
    int lastInvisibleVarSlot = -1;
    auto constantString = [&](int idx) -> std::string {
        Value v = fn.function->chunk.getConstant(static_cast<uint16_t>(idx));
        if (!isString(v)) {
            throw std::runtime_error("jvm_emitter: DEFINE/GET/SET_GLOBAL "
                                     "constant is not a name string");
        }
        return escapeJasminString(std::string(asObjString(v)->chars));
    };

    Builder b;

    // LoxGlobals.define/get/set are instance methods (design decision A2),
    // so every global access needs a receiver in scope — held in a JVM-only
    // local the Lox++ frame never sees.
    auto globalsCall = [&](const char* method, const std::string& escapedName,
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
    };

    // R4 fix (PR #109 nit): `lastInvisibleVarSlot`'s initial value, -1, is a
    // sentinel for "no invisible-var site has run yet", not a real slot.
    // `jvmSlotForLocal(-1)` is 1, the globals-receiver slot (`globalsSlot`
    // above) — an unguarded read there would silently `aload` the
    // `LoxGlobals` instance as if it were a Lox value instead of failing
    // loudly, so every read of `lastInvisibleVarSlot` goes through here.
    auto loadLastInvisibleVar = [&]() -> int {
        if (lastInvisibleVarSlot < 0) {
            throw std::runtime_error(
                "jvm_emitter: peek of an invisible var before any "
                "invisible-var site ran");
        }
        int sourceSlot = jvmSlotForLocal(lastInvisibleVarSlot);
        b.emit("aload " + std::to_string(sourceSlot), +1);
        return sourceSlot;
    };

    b.emit("invokestatic lox/LoxRuntime/init()Llox/LoxGlobals;", +1);
    b.emit("astore " + std::to_string(globalsSlot), -1);

    // R5 fix (PR #109 nit): whether the position this walk is about to visit
    // can be reached by fall-through from the instruction this pass most
    // recently emitted — false at the very start, and reset every time a
    // JUMP/LOOP/RETURN/MATCH_ERROR is emitted (none of those fall through) or
    // dead code is skipped between two live instructions (nothing physical
    // bridges that gap). Only under this condition is `b.depth`'s
    // carry-forward not this block's real entry state, so only then does
    // trusting N2's number over it stop being a live check on this pass's own
    // arithmetic (see the R1 safety net right below).
    bool prevCanFallThrough = false;
    int prevNaturalSuccessorOffset = -1;

    for (std::size_t i = 0; i < n;) {
        if (!reached(i)) {
            i++;
            continue; // endCompiler()'s trailing NIL;RETURN can be dead code.
        }
        const DecodedInstruction& in = ins[i];
        bool consumedFollowingPop = false;

        auto labelIt = labelAtOffset.find(in.offset);
        if (labelIt != labelAtOffset.end()) {
            b.label(labelIt->second);
            bool trustCarryForward =
                prevCanFallThrough && prevNaturalSuccessorOffset == in.offset;
            if (!trustCarryForward) {
                // A block leader with no live fall-through predecessor: the
                // array position right before it is a JUMP/LOOP/RETURN whose
                // real successor is somewhere else entirely, or dead code
                // that never executed, so this pass's own carry-forward is
                // not this block's entry depth at all (the array is in
                // byte-offset order, not control-flow order). N2 already
                // proved operandDepth() exact at every merge (analyzeStack
                // itself throws on any incoming-edge disagreement, before
                // this pass ever runs), so resync to it here.
                b.resync(analysis.before[i].operandDepth());
            }
            // Every other leader — a real, live fall-through predecessor —
            // keeps this pass's own carry-forward instead, so the R1 check
            // right below compares two independently derived numbers there,
            // catching a real disagreement instead of N2's own number
            // trivially matching itself.
        }

        // R1 safety net (PR #107 round 1): every correctly-lowered opcode in
        // this pass keeps the JVM operand stack's physical depth equal to
        // N2's own operandDepth() at the same offset — a temp this emitter
        // pushed is the only thing N2 counts as "on the stack". A mismatch
        // here means a peek is about to dup or pop a cell that is not
        // physically there (the SET_LOCAL/SET_GLOBAL bug below), so abort
        // loudly instead of letting jasmin or the JVM verifier find it.
        if (b.depth != analysis.before[i].operandDepth()) {
            throw std::runtime_error(
                "jvm_emitter: simulated stack depth " +
                std::to_string(b.depth) + " disagrees with analysis depth " +
                std::to_string(analysis.before[i].operandDepth()) +
                " at offset " + std::to_string(in.offset));
        }

        switch (in.op) {
        case Op::CONSTANT: {
            Value v = fn.function->chunk.getConstant(
                static_cast<uint16_t>(in.constantIndex));
            if (is<Number>(v)) {
                // Long bits in, exact double out (PR #107 R6/R7): see
                // formatDoubleBitsLiteral. `ldc2_w` of a `long` pushes 2
                // words; `longBitsToDouble(J)D` consumes 2 (the long) and
                // produces 2 (the double) — net 0 words, so the 3-line net
                // effect (+2, 0, -1) is the same +1 as before this fix.
                b.emit("ldc2_w " + formatDoubleBitsLiteral(as<Number>(v)), +2);
                b.emit("invokestatic "
                       "java/lang/Double/longBitsToDouble(J)D",
                       0);
                b.emit("invokestatic "
                       "java/lang/Double/valueOf(D)Ljava/lang/Double;",
                       -1);
            } else if (isString(v)) {
                b.emit(
                    "ldc \"" +
                        escapeJasminString(std::string(asObjString(v)->chars)) +
                        "\"",
                    +1);
            } else {
                notImplemented(in.op);
            }
            break;
        }
        case Op::NIL:
            b.emit("aconst_null", +1);
            break;
        case Op::TRUE:
            b.emit("getstatic java/lang/Boolean/TRUE Ljava/lang/Boolean;", +1);
            break;
        case Op::FALSE:
            b.emit("getstatic java/lang/Boolean/FALSE Ljava/lang/Boolean;", +1);
            break;
        case Op::EQUAL:
            b.emit("invokestatic "
                   "lox/LoxOps/equal(Ljava/lang/Object;Ljava/lang/Object;)Z",
                   -1);
            b.emit(
                "invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                0);
            break;
        case Op::GREATER:
            b.emit("invokestatic "
                   "lox/LoxOps/greater(Ljava/lang/Object;Ljava/lang/Object;)Z",
                   -1);
            b.emit(
                "invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                0);
            break;
        case Op::LESS:
            b.emit("invokestatic "
                   "lox/LoxOps/less(Ljava/lang/Object;Ljava/lang/Object;)Z",
                   -1);
            b.emit(
                "invokestatic java/lang/Boolean/valueOf(Z)Ljava/lang/Boolean;",
                0);
            break;
        case Op::NEGATE:
            // Returns Object already (auto-boxed inside LoxOps.negate itself).
            b.emit("invokestatic "
                   "lox/LoxOps/negate(Ljava/lang/Object;)Ljava/lang/Object;",
                   0);
            break;
        case Op::ADD:
            b.emit("invokestatic "
                   "lox/LoxOps/add(Ljava/lang/Object;Ljava/lang/Object;)Ljava/"
                   "lang/Object;",
                   -1);
            break;
        case Op::SUBTRACT:
            b.emit("invokestatic "
                   "lox/LoxOps/subtract(Ljava/lang/Object;Ljava/lang/"
                   "Object;)Ljava/lang/Object;",
                   -1);
            break;
        case Op::MULTIPLY:
            b.emit("invokestatic "
                   "lox/LoxOps/multiply(Ljava/lang/Object;Ljava/lang/"
                   "Object;)Ljava/lang/Object;",
                   -1);
            break;
        case Op::DIVIDE:
            b.emit("invokestatic "
                   "lox/LoxOps/divide(Ljava/lang/Object;Ljava/lang/"
                   "Object;)Ljava/lang/Object;",
                   -1);
            break;
        case Op::MODULO:
            b.emit("invokestatic "
                   "lox/LoxOps/modulo(Ljava/lang/Object;Ljava/lang/"
                   "Object;)Ljava/lang/Object;",
                   -1);
            break;
        case Op::NOT:
            b.emit("invokestatic "
                   "lox/LoxOps/not(Ljava/lang/Object;)Ljava/lang/Object;",
                   0);
            break;
        case Op::PRINT:
            b.emit("invokestatic lox/LoxOps/print(Ljava/lang/Object;)V", -1);
            break;
        case Op::POP: {
            PopKind kind = PopKind::TEMP;
            auto it = popKinds.find(in.offset);
            if (it != popKinds.end()) {
                kind = it->second;
            }
            if (kind == PopKind::TEMP) {
                b.emit("pop", -1);
            }
            // LOCAL_RECLAIM: the slot lives in the local array, not on the
            // operand stack — this pass never put it there, so there is
            // nothing here to pop.
            break;
        }
        case Op::GET_LOCAL:
            b.emit("aload " + std::to_string(jvmSlotForLocal(in.byteOperand)),
                   +1);
            break;
        case Op::SET_LOCAL: {
            int slot = jvmSlotForLocal(in.byteOperand);
            bool fuse = fusablePop(i);
            // R1 fix (PR #107 round 1): before[i].operandDepth() == 0 means
            // N2 already folded the peeked value into a named local (the
            // eager invisible-var materialization, abstract_stack.h) —
            // nothing sits on the JVM operand stack to `dup`. Load it back
            // from its slot instead: `lastInvisibleVarSlot`, not
            // `before[i].localCount`, names it (R9, PR #107 round 2;
            // resolved for N5 — localCount is only an upper bound at a CFG
            // merge, but lastInvisibleVarSlot is this pass's own forward
            // walk, so it is exact regardless of merges upstream). No copy
            // needs to survive this store: a later invisible-var site always
            // attaches to the *producing* instruction, never to this peek,
            // and a further chained peek reloads the same persistent slot
            // fresh rather than depend on a leftover operand.
            if (analysis.before[i].operandDepth() == 0) {
                loadLastInvisibleVar();
                b.emit("astore " + std::to_string(slot), -1);
            } else if (fuse) {
                b.emit("astore " + std::to_string(slot), -1);
            } else {
                b.emit("dup", +1);
                b.emit("astore " + std::to_string(slot), -1);
            }
            consumedFollowingPop = fuse;
            break;
        }
        case Op::DEFINE_GLOBAL:
            globalsCall("define", constantString(in.constantIndex),
                        /*peek=*/false);
            break;
        case Op::GET_GLOBAL:
            b.emit("aload " + std::to_string(globalsSlot), +1);
            b.emit("ldc \"" + constantString(in.constantIndex) + "\"", +1);
            b.emit("invokevirtual "
                   "lox/LoxGlobals/get(Ljava/lang/String;)Ljava/lang/Object;",
                   -1);
            break;
        case Op::SET_GLOBAL: {
            bool fuse = fusablePop(i);
            // R1 fix (PR #107 round 1): same reasoning as SET_LOCAL above.
            // When the source is already a named local, load it explicitly
            // and always use the non-peek call: the plain store fully
            // consumes the loaded copy either way, so no separate
            // fuse/non-fuse split is needed on this branch.
            // R9 (PR #107 round 2, nit): resolved for N5 the same way as
            // SET_LOCAL above — `lastInvisibleVarSlot`, not `localCount`.
            if (analysis.before[i].operandDepth() == 0) {
                loadLastInvisibleVar();
                globalsCall("set", constantString(in.constantIndex),
                            /*peek=*/false);
            } else {
                globalsCall("set", constantString(in.constantIndex),
                            /*peek=*/!fuse);
            }
            consumedFollowingPop = fuse;
            break;
        }
        case Op::JUMP:
        case Op::LOOP:
            // Same lowering either direction: a `goto` carries no operand
            // budget of its own, so nothing distinguishes a forward skip
            // from a loop's back edge once N1 has resolved both to a label.
            b.emit("goto " + labelFor(in.jumpTarget), 0);
            break;
        case Op::JUMP_IF_FALSE:
            // P2/P3: JUMP_IF_FALSE peeks — the condition must still be
            // present, on *both* outgoing edges, for whatever follows to see
            // (03_and_or keeps it as the short-circuit result; 02/04/05
            // discard it with their own, ordinary POP right after — that POP
            // is not special-cased here). `dup` supplies that second,
            // independent copy so the taken edge does not lose its copy to
            // `isFalsy`'s pop; N2's abstract stack (this pass's own R1
            // safety net, above) already models JUMP_IF_FALSE as depth-
            // preserving on this assumption, so a leaner, fused lowering
            // that dropped the dup would have to carry its own, separate
            // bookkeeping to keep the two in step. Left as a follow-up: a
            // real optimization (one word, briefly, on two probes) not a
            // correctness requirement — every probe here is nowhere near
            // .limit stack pressure.
            //
            // R2 fix (PR #109 round 1): before[i].operandDepth() == 0 is the
            // same eager invisible-var materialization as the SET_LOCAL/
            // SET_GLOBAL peek above (P2/P3 initializer whose top-level
            // operator is `and`/`or`) — the condition is not on the JVM
            // operand stack to `dup`, because N2 already moved it into
            // `lastInvisibleVarSlot`. Load a fresh copy from there instead;
            // `isFalsy`/`ifne` still only consume that one copy, so the
            // depth-preserving contract holds on both edges (0 in, 0 out)
            // exactly as the dup path holds it at (D, D) for D > 0.
            if (analysis.before[i].operandDepth() == 0) {
                loadLastInvisibleVar();
            } else {
                b.emit("dup", +1);
            }
            b.emit("invokestatic lox/LoxOps/isFalsy(Ljava/lang/Object;)Z", 0);
            b.emit("ifne " + labelFor(in.jumpTarget), -1);
            break;
        case Op::RETURN:
            // Script form (vm.cpp: frameCount reaches 0, result discarded) —
            // a function's RETURN (areturn) is N6's responsibility.
            b.emit("return", 0);
            break;
        default:
            notImplemented(in.op);
        }

        auto varsIt = invisibleVarsByOffset.find(in.offset);
        if (varsIt != invisibleVarsByOffset.end()) {
            for (int slot : varsIt->second) {
                b.emit("astore " + std::to_string(jvmSlotForLocal(slot)), -1);
                lastInvisibleVarSlot = slot;
            }
        }

        std::size_t nextIndex = i + (consumedFollowingPop ? 2 : 1);
        prevCanFallThrough = in.op != Op::JUMP && in.op != Op::LOOP &&
                             in.op != Op::RETURN && in.op != Op::MATCH_ERROR;
        prevNaturalSuccessorOffset =
            (nextIndex < n) ? ins[nextIndex].offset : -1;
        i = nextIndex;
    }

    std::ostringstream out;
    out << ".class public " << className << "\n";
    out << ".super java/lang/Object\n\n";
    out << ".method public static main([Ljava/lang/String;)V\n";
    out << "    .limit stack " << std::max(1, b.maxDepth) << "\n";
    out << "    .limit locals " << (scratchSlot + 1) << "\n\n";
    out << b.text.str();
    out << ".end method\n";
    return out.str();
}

} // namespace jvm
