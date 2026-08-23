#include "clr_emitter.h"

#include "capture_analysis.h"
#include "cfg.h"
#include "container_objects.h"
#include "exec_objects.h"
#include "native_pops.h"
#include "object.h"
#include "value.h"
#include "zero_depth_local.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clr {

namespace {

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

// Smallest-encoding int push. Reused for CALL's array-size/index operands,
// BUILD_LIST's element count, BUILD_MAP's doubled width and element
// indices (2 * in.byteOperand, through emitSpillToArray/
// newObjectArrayFromScratch — up to 510 for the 255-pair compiler ceiling),
// GET_UPVALUE/SET_UPVALUE's array index, a generated constructor's own
// literal arity, and CLOSURE's upvalue-wiring loop. CALL argCount,
// BUILD_LIST's element count, ObjFunction::arity, and the UINT8_COUNT-
// bounded upvalue index all fit a byte, but BUILD_MAP's doubled width does
// not; the last branch below (a bare `ldc.i4 n`) covers any int, so every
// caller stays correct regardless of which range it falls in.
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

// A short, offset-anchored ilasm label for a micro-branch this emitter
// inserts on top of what the CFG pass (cfg.h) already labeled — see
// ensureCapturedCell and the captured GET_LOCAL/SET_LOCAL lowering below.
// cfg.cpp's own labels are "L_<offset>" (zero-padded decimal); the distinct
// prefix here can never collide with one, and `offset` (always unique in
// one chunk) plus an optional sub-index (a CLOSURE can open more than one
// cell in one instruction, one per upvalue) keeps every one of THESE
// labels unique too.
std::string captureLabel(const char* tag, int offset, int sub = -1) {
    std::string s = std::string("Ccap") + tag + std::to_string(offset);
    if (sub >= 0) {
        s += "_" + std::to_string(sub);
    }
    return s;
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

    // Every Lox local slot this chunk's OWN captures (capture_analysis.h's
    // FunctionCaptureInfo::liveRangesBySlot) ever backs with an object[1]
    // ref-cell. Membership only, not the live range: GET_LOCAL, SET_LOCAL,
    // and CLOSURE each settle raw-vs-cell at run time with an `isinst
    // object[]` test (this file's own top-of-file note), so all this set
    // needs to answer is "does this slot ever need that test at all".
    std::unordered_set<int> capturedSlots;

    [[nodiscard]] bool isCaptured(int loxSlot) const {
        return capturedSlots.contains(loxSlot);
    }

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

    // Set (to scratchSlot+1 / scratchSlot+2) whenever this chunk needs the
    // shared callee/self spill area (computeAggregateNeeds); -1 otherwise,
    // so a stray use before buildEmitter's own computation fails loudly
    // instead of silently aliasing scratchSlot.
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

    // GET_TAG followed immediately by JUMP_TABLE (P8): compileMatchBody's
    // only call site for JUMP_TABLE emits the pair back to back, with
    // nothing else between them, so this is the only shape a JUMP_TABLE is
    // ever found in — a GET_TAG not immediately followed by one is the
    // sparse, compare-and-branch match form instead, which needs no fusion.
    // Carries `fusablePop`'s own two guards (`reached(j)`, no CFG label at
    // the JUMP_TABLE's own offset) for the same reason: no program today
    // jumps into the middle of a match's own dispatch preamble, but should
    // one ever do so, these guards turn that into a loud emit-time abort
    // (the ordinary, unfused GET_TAG path has no case for `Op::JUMP_TABLE`
    // reaching the dispatch switch on its own) rather than a silently wrong
    // label.
    [[nodiscard]] bool fusableJumpTable(std::size_t i) const {
        std::size_t j = i + 1;
        if (j >= fn.instructions.size() || !reached(j) ||
            fn.instructions[j].op != Op::JUMP_TABLE) {
            return false;
        }
        return labelAtOffset.find(fn.instructions[j].offset) ==
               labelAtOffset.end();
    }

    // Every name-bearing opcode's 2-byte operand indexes the SAME constant
    // pool as CONSTANT itself; this fetches that constant, ensures it is a
    // string (a mismatch here is a compiler bug, not a runtime condition,
    // hence a thrown error rather than a checked cast), and formats it as a
    // complete ilasm string-literal operand. Shared by DEFINE_GLOBAL/
    // GET_GLOBAL/SET_GLOBAL's own global name and by every class/property/
    // method name (CLASS, GET_PROPERTY, SET_PROPERTY, DEFINE_METHOD,
    // GET_SUPER, INSTANCEOF, INVOKE, SUPER_INVOKE) — one opcode's constant
    // index means the same thing regardless of which of those reads it.
    [[nodiscard]] std::string constantStringLiteral(int constantIndex) const {
        Value v = fn.function->chunk.getConstant(
            static_cast<uint16_t>(constantIndex));
        if (!isString(v)) {
            throw std::runtime_error("clr_emitter: constant " +
                                     std::to_string(constantIndex) +
                                     " is not a name string");
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

// A captured local's CIL slot is not one fixed representation for its
// whole life: before its first capture it holds the raw Lox value (this
// file's own top-of-file note); from there through the matching
// CLOSE_UPVALUE it holds an `object[1]` ref-cell instead. GET_LOCAL/
// SET_LOCAL on a captured slot cannot pick which one applies by this
// instruction's OWN offset: a `for` loop's condition/increment clauses sit,
// in byte order, BEFORE the body that captures the loop variable, yet the
// LOOP back-edge revisits them on every later iteration, by which point
// the slot IS a cell (V3_loopvar.lox) — program order and runtime order
// are not the same thing once a back-edge exists. `isinst object[]` asks
// the one question that is always true regardless of which trip this is:
// no Lox value is ever itself a bare `object[]` (every aggregate the
// runtime exposes wraps its storage in a named class — LoxOps.GetIndex's
// own BINDING INVARIANT comment, runtime/clr/src), so this test can never
// mistake a real Lox value for a cell or the reverse. `isinst` (unlike the
// JVM's `instanceof`) leaves null-or-the-reference on the stack rather than
// a boolean, so `brfalse`/`brtrue` read that result directly with no
// separate comparison. `sub` disambiguates two calls at the SAME offset —
// `normalizeFoldedOperands`'s multi-slot repair loop reads several folded
// Lox slots for one instruction, and a shared offset with no sub index
// would hand two of those reads the same ilasm label pair.
void emitCapturedGetLocal(Emitter& e, int slot, int offset, int sub = -1) {
    std::string rawLbl = captureLabel("gr", offset, sub);
    std::string endLbl = captureLabel("ge", offset, sub);
    int d = e.b.depth;
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("isinst object[]", 1, 0);
    e.b.emit("brfalse " + rawLbl, 1, -1);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("castclass object[]", 1, 0);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldelem.ref", 2, -1);
    e.b.emit("br " + endLbl, 0, 0);
    e.b.label(rawLbl);
    e.b.resync(d);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.label(endLbl);
    e.b.resync(d + 1);
}

// SET_LOCAL's captured-slot lowering: the value to store already sits on
// top of the stack (`peek` decides whether it must still be there after —
// P2), but deciding raw-vs-cell needs the SLOT's own current content, a
// second value with nowhere free to sit without disturbing the first —
// spilled to `scratchSlot` for the same reason `emitGlobalsCall`'s peek
// variant spills there. Never a scratch-slot conflict with a CALL, a
// BUILD_LIST, or another SET_LOCAL/SET_GLOBAL/SET_UPVALUE's own use of it:
// one instruction runs to completion before the next starts.
void emitCapturedStore(Emitter& e, int slot, int offset, bool peek) {
    std::string rawLbl = captureLabel("sr", offset);
    std::string endLbl = captureLabel("se", offset);
    std::string scratch = std::to_string(e.scratchSlot);
    int d = e.b.depth;
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("isinst object[]", 1, 0);
    e.b.emit("brfalse " + rawLbl, 1, -1);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("castclass object[]", 1, 0);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("stelem.ref", 3, -3);
    e.b.emit("br " + endLbl, 0, 0);
    e.b.label(rawLbl);
    e.b.resync(d - 1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("stloc " + std::to_string(slot), 1, -1);
    e.b.label(endLbl);
    e.b.resync(d - 1);
    if (peek) {
        e.b.emit("ldloc " + scratch, 0, +1);
    }
}

// The Lox slot a peek-family consumer at offset `offset` reads once its own
// operand depth is zero — exact away from a CFG merge, and cross-checked by
// `resolveZeroDepthLocalSlot` (zero_depth_local.h, the one shared authority
// for this resolution) against this pass's own forward tracker at a merge
// (see this file's own top-of-file note). Split out from
// `loadNamedLocalAtZeroDepth` (below) so `normalizeFoldedOperands` can name
// this SAME topmost folded slot without also emitting its load: a deficit
// above 1 needs every folded slot from here down to
// `topLoxSlot - (deficit - 1)`, loaded bottom first, not only this one.
int resolveTopLoxSlot(const Emitter& e, std::size_t i, int offset) {
    return resolveZeroDepthLocalSlot(
        e.analysis.before[i].localCount - 1, e.labelAtOffset.contains(offset),
        e.lastInvisibleVarSlot, offset, "clr_emitter");
}

// Loads Lox frame slot `loxSlot` onto the CIL evaluation stack, routed
// through the same captured-slot test (`isinst object[]`) GET_LOCAL/
// SET_LOCAL use whenever the capture analysis marks that slot captured.
// Shared by `loadNamedLocalAtZeroDepth` (below) and
// `normalizeFoldedOperands`'s own multi-slot repair, both of which load a
// Lox slot they did not reach through an ordinary GET_LOCAL instruction.
// `sub` forwards to `emitCapturedGetLocal` unchanged — see its own note —
// so a caller that issues more than one load at the same offset gives each
// one a distinct value.
int loadLoxSlot(Emitter& e, int loxSlot, int offset, int sub = -1) {
    int slot = e.slotForLocal(loxSlot);
    if (e.isCaptured(loxSlot)) {
        emitCapturedGetLocal(e, slot, offset, sub);
    } else {
        e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    }
    return slot;
}

// The CLR twin of jvm_emitter.cpp's own `loadNamedLocalAtZeroDepth`: at a
// peek-family consumer whose operand depth is zero, the value to consume
// is not a genuine evaluation-stack temporary — the shared abstract-stack
// pass already folded it into a named local (P2). `outLoxSlot`, when given,
// receives the Lox slot this call resolved — so a caller that also needs
// the Lox-numbered slot (GET_ITER's own captured-slot test) reads it from
// here instead of re-deriving it.
int loadNamedLocalAtZeroDepth(Emitter& e, std::size_t i, int offset,
                              int* outLoxSlot = nullptr) {
    int loxSlot = resolveTopLoxSlot(e, i, offset);
    if (outLoxSlot != nullptr) {
        *outLoxSlot = loxSlot;
    }
    return loadLoxSlot(e, loxSlot, offset);
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
    } else if (isEnumCtor(v)) {
        // P8/P6: an enum declaration compiles each variant to a CONSTANT
        // (compiler.cpp's enumDeclaration) that names an ObjEnumCtor, then a
        // DEFINE_GLOBAL — the same shape a plain number or string literal
        // uses. This materialises a real LoxEnumCtor here, once, so a later
        // CALL of it (LoxOps.Call, since LoxEnumCtor already implements
        // ILoxCallable) needs no case of its own for "the callee came from a
        // CONSTANT, not a CLOSURE". `newobj` pops its four constructor
        // arguments and pushes the fresh reference itself, so this needs no
        // `dup` (unlike emitClosure's own array-building idiom).
        ObjEnumCtor* ctor = asObjEnumCtor(as<Obj*>(v));
        e.b.emit(pushIntInstruction(ctor->tag), 0, +1);
        e.b.emit(pushIntInstruction(ctor->arity), 0, +1);
        e.b.emit("ldstr " +
                     ilasmStringLiteral(std::string(ctor->ctorName->chars)),
                 0, +1);
        e.b.emit("ldstr " +
                     ilasmStringLiteral(std::string(ctor->enumName->chars)),
                 0, +1);
        e.b.emit("newobj instance void [LoxRuntime]Lox.LoxEnumCtor::.ctor"
                 "(int32, int32, string, string)",
                 4, -3);
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
    int slot = e.slotForLocal(in.byteOperand);
    if (e.isCaptured(in.byteOperand)) {
        emitCapturedGetLocal(e, slot, in.offset);
    } else {
        e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    }
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
    bool captured = e.isCaptured(in.byteOperand);
    if (isFoldedAtZeroDepth(e, i)) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        if (captured) {
            emitCapturedStore(e, slot, in.offset, /*peek=*/false);
        } else {
            e.b.emit("stloc " + std::to_string(slot), 1, -1);
        }
    } else if (captured) {
        emitCapturedStore(e, slot, in.offset, /*peek=*/!fuse);
    } else if (fuse) {
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    } else {
        e.b.emit("dup", 1, +1);
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    }
    consumedFollowingPop = fuse;
}

// A top-level `var n = match {...};` can hand DEFINE_GLOBAL a match result
// the shared abstract-stack pass already folded into a named local, with
// nothing physically on the CIL evaluation stack to pop — the same shape
// `normalizeFoldedOperands` (driven by `nativePops`'s own DEFINE_GLOBAL row)
// already repairs before this function runs, so a genuine value is on the
// stack either way by the time this runs.
void emitDefineGlobal(Emitter& e, const DecodedInstruction& in) {
    emitGlobalsCall(e, "Define", e.constantStringLiteral(in.constantIndex),
                    /*peek=*/false);
}

void emitGetGlobal(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), 0, +1);
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
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
        emitGlobalsCall(e, "Set", e.constantStringLiteral(in.constantIndex),
                        /*peek=*/false);
    } else {
        emitGlobalsCall(e, "Set", e.constantStringLiteral(in.constantIndex),
                        /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
}

// PRINT ordinarily consumes a genuine evaluation-stack value, but a `match`
// expression's own result can reach it already folded into a named local —
// `compileMatchBody`'s own closing POP retires only the synthetic "subject"
// local, exposing the arm's own result local as the new top with no
// separate value ever pushed for a genuine consumer to read. `print match 1
// { case 1 => "a" case _ => "b" };` is exactly this shape, with nothing in
// between to declare a real local first. `normalizeFoldedOperands` (driven
// by `nativePops`'s own PRINT row) already repairs it before this function
// runs, so a genuine value is on the stack either way by the time this
// runs.
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

// Spills `width` values, already loose on the stack (topmost = last
// index), into consecutive scratch slots starting at `scratchBase` — the
// one step every reshape below needs before it can build a fresh array
// reference on top of where those values used to sit.
void spillLooseValues(Emitter& e, int scratchBase, int width) {
    for (int i = width - 1; i >= 0; i--) {
        e.b.emit("stloc " + std::to_string(scratchBase + i), 1, -1);
    }
}

// Builds a fresh `object[width]` and refills it ascending from the
// scratch slots `spillLooseValues` (or an equivalent spill) already
// filled, leaving the array as the new top of stack. Shared by every
// P7-shaped reshape — BUILD_LIST's own spill-and-refill and CALL's
// argument array both need this exact loop, so a change to the element
// type or the index-push instruction updates once for both.
void newObjectArrayFromScratch(Emitter& e, int scratchBase, int width) {
    e.b.emit(pushIntInstruction(width), 0, +1);
    e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
    for (int i = 0; i < width; i++) {
        e.b.emit("dup", 1, +1);
        e.b.emit(pushIntInstruction(i), 0, +1);
        e.b.emit("ldloc " + std::to_string(scratchBase + i), 0, +1);
        e.b.emit("stelem.ref", 3, -3);
    }
}

// The P7 reshape BUILD_LIST needs: spill `width` values, already loose on
// the stack, into `argScratchBase`, build a fresh `object[width]`, refill
// it ascending, then hand it to `call` (a complete ilasm `call` instruction
// text; `callCellsRead`/`callNetDelta` describe its own stack effect).
// `width == 0` needs no scratch at all: the empty array builds directly,
// with nothing to spill or refill. Not CALL's own helper too — see
// emitCall's own note for why a value sitting BENEATH the ones this
// function spills (CALL's callee) does not fit this shape.
void emitSpillToArray(Emitter& e, int width, const std::string& call,
                      int callCellsRead, int callNetDelta) {
    if (width == 0) {
        e.b.emit(pushIntInstruction(0), 0, +1);
        e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
        e.b.emit(call, callCellsRead, callNetDelta);
        return;
    }
    spillLooseValues(e, e.argScratchBase, width);
    newObjectArrayFromScratch(e, e.argScratchBase, width);
    e.b.emit(call, callCellsRead, callNetDelta);
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
// CALL/BUILD_LIST needs, plus one for CALL's own callee, computed once
// before this pass starts — reused across every CALL/BUILD_LIST site in
// the chunk, because they run one at a time, never concurrently.
//
// Not routed through `emitSpillToArray`: the callee sits UNDERNEATH the
// arguments on the stack, so it must be popped to its own scratch slot
// AFTER the argument-spill loop, then reloaded BEFORE the array is built —
// one more step sandwiched inside the shape `emitSpillToArray` gives
// BUILD_LIST whole, with no element beneath the ones it spills. Forcing
// CALL through that helper would either spill the callee as if it were
// argument 0 (wrong value in the array) or re-spill the arguments a second
// time once they no longer sit on the stack (evaluation stack underflow).
// The spill and the array-build themselves are the same two loops
// `emitSpillToArray` runs, factored into `spillLooseValues`/
// `newObjectArrayFromScratch` so both call sites share one copy.
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
    spillLooseValues(e, e.argScratchBase, argCount);
    e.b.emit("stloc " + std::to_string(e.calleeScratchSlot), 1, -1);

    e.b.emit("ldloc " + std::to_string(e.calleeScratchSlot), 0, +1);
    newObjectArrayFromScratch(e, e.argScratchBase, argCount);
    e.b.emit(callSig, 2, -1);
}

// BUILD_LIST (pulled forward from the aggregates scope this opcode
// conceptually belongs to: V1_fresh_cell.lox and V3_loopvar.lox, the two
// probes that prove or disprove the fresh-cell-per-declaration model this
// node exists to get right, each build a list of the closures under test
// and read it back by index, so an emitter that still throws "not
// implemented" on BUILD_LIST/GET_INDEX/SET_INDEX cannot even reach the
// closure bug being tested for). The same P7 shuffle CALL's own argument
// array needs (spill loose values below where a fresh array would land,
// build the array, refill it) — reusing CALL's own SCRATCH-SLOT
// allocation (computeAggregateNeeds), not a second one, even though the
// two opcodes' own emitted shapes stay distinct (emitCall's own note).
// LoxOps.BuildList (runtime/clr) copies the array into a fresh LoxList in
// the same order.
void emitBuildList(Emitter& e, const DecodedInstruction& in) {
    emitSpillToArray(e, in.byteOperand,
                     "call class [LoxRuntime]Lox.LoxList "
                     "[LoxRuntime]Lox.LoxOps::BuildList(object[])",
                     /*callCellsRead=*/1, /*callNetDelta=*/0);
}

// BUILD_MAP n: n key/value pairs already loose on the stack, pushed in
// source order (key0, val0, key1, val1, ..., matching compiler.cpp's map
// literal) — the same P7 reshape as BUILD_LIST, over 2n cells instead of n.
// `12_list_map_index` — this node's own checkpoint — indexes a map literal
// as well as a list, so this opcode is pulled forward here for the same
// reason BUILD_LIST/GET_INDEX/SET_INDEX are: the probe cannot compile
// without it. LoxOps.BuildMap (runtime/clr) validates every key before
// writing any pair, matching vm.cpp's own two-pass shape.
void emitBuildMap(Emitter& e, const DecodedInstruction& in) {
    emitSpillToArray(e, 2 * in.byteOperand,
                     "call class [LoxRuntime]Lox.LoxMap "
                     "[LoxRuntime]Lox.LoxOps::BuildMap(object[])",
                     /*callCellsRead=*/1, /*callNetDelta=*/0);
}

// GET_INDEX/SET_INDEX: vm.cpp's own operand order already matches
// LoxOps's parameter order ([collection, index] and [collection, index,
// value], bottom to top), so neither needs a shuffle — unlike SET_LOCAL/
// SET_GLOBAL/SET_UPVALUE, SET_INDEX is not a P2 peek: vm.cpp pops its
// operands whole and pushes a genuinely new result cell (LoxOps.SetIndex's
// own return), so a plain call with no dup is exactly right.
void emitGetIndex(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::GetIndex(object, object)", 2,
             -1);
}

void emitSetIndex(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::SetIndex(object, object, "
             "object)",
             3, -2);
}

// SLICE pops [seq, start, end] bottom-to-top (vm.cpp: peek(2), peek(1),
// peek(0)) — LoxOps.Slice's own parameter order already matches that, so
// this is a plain call, no shuffle (the same P2 exemption GET_INDEX/
// SET_INDEX get, for the same reason: vm.cpp pops these operands whole and
// pushes a genuinely new result).
void emitSlice(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::Slice(object, object, "
             "object)",
             3, -2);
}

// IN pops [elem, seq] bottom-to-top (vm.cpp pops seq first, so seq sits on
// top) — LoxOps.In's own doc comment already matches that order. Returns
// bool; boxed the same way emitComparisonOp boxes EQUAL/GREATER/LESS,
// since every Lox value on the CIL evaluation stack is `object`.
void emitIn(Emitter& e) {
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::In(object, object)", 2, -1);
    e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
}

// IS_SEQ: a match sequence pattern's own type check (compiler.cpp). Matches
// Op::IS_SEQ exactly (vm.cpp) — List and String only, Map is excluded.
void emitIsSeq(Emitter& e) {
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::IsSeq(object)", 1, 0);
    e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
}

// ITER_HAS_NEXT/ITER_NEXT operate on the copy a preceding GET_LOCAL already
// loaded (P8 — the for-in iterator lives in an ordinary chunk local, never
// a dedicated backend slot); the local itself is untouched.
void emitIterHasNext(Emitter& e) {
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::IterHasNext(object)", 1, 0);
    e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
}

void emitIterNext(Emitter& e) {
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::IterNext(object)", 1, 0);
}

// GET_ITER replaces its own operand in place (vm.cpp: `stackTop[-1] =
// ...`). It carries no operand byte of its own. The iterable expression's
// own declaring push (11_for_in.lox: e.g. BUILD_LIST) is what put the
// value there, and the shared abstract-stack analysis already attributes
// THAT instruction's own offset the invisible-var store for this stack
// position (finishInstruction), one instruction earlier than GET_ITER
// itself. By the time GET_ITER runs, the CIL evaluation stack is therefore
// already empty at this position (`operandDepth() == 0`) — the value
// already lives in its own local slot, not still sitting on the evaluation
// stack the way a plain "one call, no reload" lowering would assume.
// GET_ITER must reload that slot, transform it, and store the result
// straight back — `loadNamedLocalAtZeroDepth` names and loads it, with the
// same merge/captured-slot guards every other zero-depth consumer shares.
void emitGetIter(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    if (!isFoldedAtZeroDepth(e, i)) {
        throw std::runtime_error(
            "clr_emitter: GET_ITER expected its iterable already folded "
            "into an invisible-var slot (operand depth 0), but the CIL "
            "evaluation stack was not empty here");
    }
    int loxSlot = 0;
    int slot = loadNamedLocalAtZeroDepth(e, i, in.offset, &loxSlot);
    e.b.emit("call class [LoxRuntime]Lox.LoxIterator "
             "[LoxRuntime]Lox.LoxOps::GetIter(object)",
             1, 0);
    if (e.isCaptured(loxSlot)) {
        emitCapturedStore(e, slot, in.offset, /*peek=*/false);
    } else {
        e.b.emit("stloc " + std::to_string(slot), 1, -1);
    }
}

// CLASS name (P5/P6): builds a fresh, still-superclass-less LoxClass —
// vm.cpp's own CLASS handler does the same (an empty method table, no
// superclass yet); INHERIT (below) is what later fills either in, on the
// classes that have one. CIL's `newobj` pops its constructor arguments and
// pushes the fresh reference itself (unlike the JVM backend's own `new;
// dup; ...; invokespecial <init>` idiom), so this needs no `dup` to keep an
// extra reference around.
void emitClass(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("ldnull", 0, +1);
    e.b.emit("newobj instance void [LoxRuntime]Lox.LoxClass::.ctor(string, "
             "class [LoxRuntime]Lox.LoxClass)",
             2, -1);
}

// INHERIT: compiler.cpp's fixed shape — `namedVariable(superclass);
// beginScope(); addLocal(super); markInitialized(); namedVariable(className);
// INHERIT` — means the superclass value is ALWAYS already the "super"
// invisible var by the time this instruction runs (the eager-materialization
// rule every other peek site in this file already assumes). It is never a
// live evaluation-stack temporary here, so the shared abstract-stack pass
// counts INHERIT as consuming only the ONE thing that genuinely is one: the
// subclass, pushed by the immediately preceding, non-declaring
// `namedVariable(className)`. vm.cpp mutates the subclass IN PLACE
// (`subclass->methods.addAll(superclass->methods); subclass->superclass =
// superclass;`) — the merge LoxOps.InheritInto performs must land on the
// exact object identity DEFINE_GLOBAL/markInitialized already stored, not a
// freshly reconstructed one (LoxClass.InheritFrom's own note) — and vm.cpp's
// own "superclass stays on the stack as the super local" is already
// satisfied for free here: the super local's CIL slot never changes, so
// nothing needs pushing back for it. `loadNamedLocalAtZeroDepth` names and
// loads the super local — the same mechanism every other zero-depth
// consumer shares.
void emitInherit(Emitter& e, std::size_t i, const DecodedInstruction& in) {
    loadNamedLocalAtZeroDepth(e, i, in.offset);
    e.b.emit("call void [LoxRuntime]Lox.LoxOps::InheritInto(object, object)", 2,
             -2);
}

// GET_PROPERTY name: field-before-method order and the exact error text
// live in LoxOps.GetProperty (runtime/clr) — this pass only supplies the
// receiver (already on the stack) and the constant name.
void emitGetProperty(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::GetProperty(object, string)",
             2, -1);
}

// SET_PROPERTY name (P2): `[obj,v] -> [v]` — the assigned value must
// survive the call, but it already sits ON TOP of the instance (not beneath
// it, the way GET_PROPERTY's receiver does), so it is spilled to
// `e.scratchSlot` while the constant name is pushed between them — the same
// shuffle `emitGlobalsCall`'s own peek path uses for the identical reason.
void emitSetProperty(Emitter& e, const DecodedInstruction& in) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::SetProperty(object, "
             "string, object)",
             3, -2);
}

// DEFINE_METHOD name (P2): `[cls,fn] -> [cls]` — the class value must
// survive (the next method in the same class body, or the class body's own
// trailing POP, reads it again), so `dup` keeps a copy while the closure
// spills to `e.scratchSlot`. LoxOps.DefineMethod takes concrete types, so
// both operands need an explicit `castclass` here: the compiler guarantees
// this exact shape (a CLASS's own value, a CLOSURE's own result) on every
// real program, so a mismatch can only be an emitter bug, and a raw
// InvalidCastException is an acceptable way to fail loudly on one.
void emitDefineMethod(Emitter& e, const DecodedInstruction& in) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("dup", 1, +1);
    e.b.emit("castclass [LoxRuntime]Lox.LoxClass", 1, 0);
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("castclass [LoxRuntime]Lox.LoxClosure", 1, 0);
    e.b.emit("call void [LoxRuntime]Lox.LoxOps::DefineMethod(class "
             "[LoxRuntime]Lox.LoxClass, string, class "
             "[LoxRuntime]Lox.LoxClosure)",
             3, -3);
}

// GET_SUPER name: vm.cpp pops the superclass (top), then binds `this` (now
// on top) to the found method. `this` was pushed by a PRECEDING GET_LOCAL 0
// — `super_()` in compiler.cpp always pushes `this` before `super` — so
// both values are already genuine evaluation-stack operands, never a
// zero-depth fold. CIL has no `swap` (unlike the JVM backend's own
// `emitGetSuper`, which reorders with one `swap` plus a single scratch
// slot), so both values are spilled to their own scratch slot instead, then
// reloaded in `LoxOps.GetSuper`'s own parameter order (superclassVal, name,
// self).
void emitGetSuper(Emitter& e, const DecodedInstruction& in) {
    std::string superScratch = std::to_string(e.scratchSlot);
    std::string selfScratch = std::to_string(e.calleeScratchSlot);
    e.b.emit("stloc " + superScratch, 1, -1);
    e.b.emit("stloc " + selfScratch, 1, -1);
    e.b.emit("ldloc " + superScratch, 0, +1);
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("ldloc " + selfScratch, 0, +1);
    e.b.emit("call object [LoxRuntime]Lox.LoxOps::GetSuper(object, string, "
             "object)",
             3, -2);
}

// INSTANCEOF name: vm.cpp looks the class up BY NAME in globals, not from a
// constant-pool class reference (`m_globals.get(className, classVal)`) —
// LoxOps.InstanceOf mirrors that exactly, so this pass only supplies the
// already-open globals receiver and the constant name. INVARIANT:
// e.globalsSlot holds the one LoxGlobals instance LoxRuntime::Init()
// returned, and nothing else ever writes it — even though every local,
// slot 0 included, is declared `object` in `.locals init`. CoreCLR does
// not check reference assignability at an unverified call site, so the
// declared CIL type here is not evidence of the invariant; the emitter's
// own write discipline is.
void emitInstanceof(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldloc " + std::to_string(e.globalsSlot), 0, +1);
    e.b.emit("ldstr " + e.constantStringLiteral(in.constantIndex), 0, +1);
    e.b.emit("call bool [LoxRuntime]Lox.LoxOps::InstanceOf(object, class "
             "[LoxRuntime]Lox.LoxGlobals, string)",
             3, -2);
    e.b.emit("box [System.Runtime]System.Boolean", 1, 0);
}

// INVOKE name argc (P5+P6): the fused "get property then call" fast path —
// LoxOps.Invoke keeps the field-before-method order (a field holding a
// function is called, never treated as a method, matching vm.cpp). argCount
// == 0 needs no reshuffle at all, same as `emitCall`'s own argCount == 0
// path: the receiver is already the sole, topmost value, so the name and
// the empty array build directly on top of it. argCount >= 1 reuses the
// exact same scratch slots `emitCall` does (e.calleeScratchSlot for the
// receiver, e.argScratchBase for the args) — `computeAggregateNeeds` counts
// this opcode's own argCount alongside CALL's and BUILD_LIST's, so those
// slots are always wide enough.
void emitInvoke(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    std::string name = e.constantStringLiteral(in.constantIndex);
    const char* invokeSig = "call object [LoxRuntime]Lox.LoxOps::Invoke("
                            "object, string, object[])";
    if (argCount == 0) {
        e.b.emit("ldstr " + name, 0, +1);
        e.b.emit(pushIntInstruction(0), 0, +1);
        e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
        e.b.emit(invokeSig, 3, -2);
        return;
    }
    spillLooseValues(e, e.argScratchBase, argCount);
    e.b.emit("stloc " + std::to_string(e.calleeScratchSlot), 1, -1);

    e.b.emit("ldloc " + std::to_string(e.calleeScratchSlot), 0, +1);
    e.b.emit("ldstr " + name, 0, +1);
    newObjectArrayFromScratch(e, e.argScratchBase, argCount);
    e.b.emit(invokeSig, 3, -2);
}

// SUPER_INVOKE name argc: `[self,arg0..argN-1,superclassVal] -> [result]` —
// vm.cpp pops the superclass first (top), then calls with self at its usual
// receiver position. Unlike the JVM backend's own `emitSuperInvoke` (which
// reduces the argCount == 0 case to one `swap` plus a single scratch slot),
// CIL's missing `swap` means both self and the superclass always need their
// own scratch slot here — `e.scratchSlot` for the superclass,
// `e.calleeScratchSlot` for self (the exact slot `emitInvoke`'s own
// argCount >= 1 path uses for a receiver) — plus `e.argScratchBase` for the
// arguments when argCount >= 1: three DISTINCT, already-existing slots,
// since one instruction runs to completion before the next starts.
void emitSuperInvoke(Emitter& e, const DecodedInstruction& in) {
    int argCount = in.byteOperand;
    std::string name = e.constantStringLiteral(in.constantIndex);
    std::string superScratch = std::to_string(e.scratchSlot);
    std::string selfScratch = std::to_string(e.calleeScratchSlot);
    const char* superInvokeSig =
        "call object [LoxRuntime]Lox.LoxOps::SuperInvoke(object, string, "
        "object, object[])";
    if (argCount == 0) {
        e.b.emit("stloc " + superScratch, 1, -1); // superclassVal (top)
        e.b.emit("stloc " + selfScratch, 1, -1);  // self
        e.b.emit("ldloc " + superScratch, 0, +1);
        e.b.emit("ldstr " + name, 0, +1);
        e.b.emit("ldloc " + selfScratch, 0, +1);
        e.b.emit(pushIntInstruction(0), 0, +1);
        e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
        e.b.emit(superInvokeSig, 4, -3);
        return;
    }
    e.b.emit("stloc " + superScratch, 1, -1); // superclassVal (top)
    spillLooseValues(e, e.argScratchBase, argCount);
    e.b.emit("stloc " + selfScratch, 1, -1); // self

    e.b.emit("ldloc " + superScratch, 0, +1);
    e.b.emit("ldstr " + name, 0, +1);
    e.b.emit("ldloc " + selfScratch, 0, +1);
    newObjectArrayFromScratch(e, e.argScratchBase, argCount);
    e.b.emit(superInvokeSig, 4, -3);
}

// MATCH_ERROR: a match with no accepting arm raises a real, reachable
// runtime error (compiler.cpp emits this whenever no arm is an unguarded
// catch-all). `LoxOps.MatchError` BUILDS the error rather than throwing it,
// so the call leaves a real value on the stack and `throw` — a genuine
// terminal instruction — ends the block; a plain void-returning call here
// would leave the CLR JIT unable to prove this path never falls through.
void emitMatchError(Emitter& e) {
    e.b.emit("call class [LoxRuntime]Lox.LoxError [LoxRuntime]Lox.LoxOps::"
             "MatchError()",
             0, +1);
    e.b.emit("throw", 1, -1);
}

// GET_TAG's own instruction, standalone — the sparse, compare-and-branch
// match form (compiler.cpp emits GET_LOCAL(subjectSlot); GET_TAG; CONSTANT;
// EQUAL, once per arm, whenever a guard, an or-pattern, an @-binding, or a
// non-dense tag set defeats table dispatch). `LoxOps.GetTag` returns the tag
// as a primitive `float64`, the same shape `LoxOps.Add` and friends already
// expect from a boxed number operand once boxed — box it exactly the way
// `emitConstant` boxes a number literal, so the CONSTANT/EQUAL pair right
// after sees the same `object` shape any other comparison operand does.
void emitGetTag(Emitter& e) {
    e.b.emit("call float64 [LoxRuntime]Lox.LoxOps::GetTag(object)", 1, 0);
    e.b.emit("box [System.Runtime]System.Double", 1, 0);
}

// GET_TAG fused with an immediately following JUMP_TABLE (P8's own hazard,
// clr_emitter.h's own top-of-file note): keeps the tag a primitive `float64`
// the whole way, never boxing it only to unbox it again for the dispatch.
// CIL's `switch` is base-0 and takes no base argument of its own (unlike
// jasmin's `tableswitch <low>`), so the base subtraction is explicit here:
// `conv.i4`, push `min`, `sub`, then `switch`. `min` is the table's own
// lower tag bound; one label per arm, in ascending tag order —
// chunk_decoder.cpp already resolved every arm's own absolute target from
// the raw forward-offset bytes, so this reads that, not the bytes
// themselves. An index outside the table's own width falls through to the
// very next instruction: compileMatchBody always places a real MATCH_ERROR
// there, and the CFG (cfg.h) already gives that offset its own label, the
// same as any other block leader — this pass does not special-case it.
void emitFusedGetTagJumpTable(Emitter& e, const DecodedInstruction& table) {
    e.b.emit("call float64 [LoxRuntime]Lox.LoxOps::GetTag(object)", 1, 0);
    e.b.emit("conv.i4", 1, 0);
    e.b.emit(pushIntInstruction(table.minTag), 0, +1);
    e.b.emit("sub", 2, -1);
    std::ostringstream sw;
    sw << "switch (";
    for (std::size_t k = 0; k < table.jumpTable.size(); k++) {
        if (k > 0) {
            sw << ",\n        ";
        }
        sw << e.labelFor(table.jumpTable[k].target);
    }
    sw << ")";
    e.b.emit(sw.str(), 1, -1);
}

// GET_TAG's own dispatch case: fuse with a following JUMP_TABLE when one is
// there (`fusableJumpTable`'s own note), matching SET_LOCAL/SET_GLOBAL/
// SET_UPVALUE's own POP-fusion shape — the caller skips the JUMP_TABLE's
// own array slot instead of dispatching it a second time.
void emitGetTagOrFused(Emitter& e, std::size_t i,
                       bool& consumedFollowingJumpTable) {
    if (e.fusableJumpTable(i)) {
        emitFusedGetTagJumpTable(e, e.fn.instructions[i + 1]);
        consumedFollowingJumpTable = true;
    } else {
        emitGetTag(e);
    }
}

// Wraps `slot` in a fresh `object[1]` ref-cell, seeded with the raw value
// already there, UNLESS `slot` already holds one — an idempotent seed, not
// an unconditional one, and that is the load-bearing choice of this whole
// design (the bug gate). See this file's own top-of-file note and
// `emitCapturedGetLocal`'s: V3_loopvar.lox declares its captured local
// ONCE, outside the loop, while the CLOSURE that captures it sits inside
// the loop body — one static offset, reached once per iteration, with no
// CLOSE_UPVALUE between trips (the range spans the whole loop). An
// unconditional seed here would hand every iteration's closure its OWN
// fresh cell instead of the one shared cell V3_loopvar's checkpoint
// (3, 3, 3) requires. The idempotent check is what makes ONE emitted
// instruction correct on both trips: seed on the first, no-op on every one
// after, because nothing else in this chunk ever turns a cell back into a
// raw value once created — CLOSE_UPVALUE emits no CIL at all (see its own
// case in emitBody) — and a fresh DECLARATION always re-`stloc`s the slot
// directly (finishInstruction), never through this check, so it is exactly
// what puts a slot back to raw for the NEXT incarnation
// (V1_fresh_cell.lox: `var snapshot` re-declares, hence re-seeds, on every
// trip of ITS loop).
void ensureCapturedCell(Emitter& e, int slot, int offset, int subIndex) {
    std::string readyLbl = captureLabel("ok", offset, subIndex);
    int d = e.b.depth;
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("isinst object[]", 1, 0);
    e.b.emit("brtrue " + readyLbl, 1, -1);
    e.b.emit(pushIntInstruction(1), 0, +1);
    e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
    e.b.emit("dup", 1, +1);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldloc " + std::to_string(slot), 0, +1);
    e.b.emit("stelem.ref", 3, -3);
    e.b.emit("stloc " + std::to_string(slot), 1, -1);
    e.b.label(readyLbl);
    e.b.resync(d);
}

// CLOSURE's decoded `isLocal=1` entries name only slots the capture
// analysis (capture_analysis.h) also reports captured — `up.isLocal` comes
// from the CLOSURE instruction's own decoded operand bytes, `e.capturedSlots`
// from the capture analysis's `liveRangesBySlot`; nothing makes a future
// drift between the two impossible, and a silent one would seed a cell
// this pass never marks captured, so GET_LOCAL/SET_LOCAL elsewhere would
// keep reading the raw value while this closure reads the cell — a wrong
// VALUE, no assembler error, no exception. Fail loudly instead. Both sides
// read the same decoded CLOSURE operand bytes today (capture_analysis.cpp's
// own dataflow walks `in.upvalues` directly), so no input through
// emitScript/emitProgram can make this check fire; it stands ready for the
// day one of the two derivations changes to read a different source.
void checkAllCapturesAreReported(const Emitter& e,
                                 const DecodedInstruction& in) {
    for (const ClosureUpvalue& up : in.upvalues) {
        if (up.isLocal && !e.isCaptured(up.index)) {
            throw std::runtime_error(
                "clr_emitter: CLOSURE captures local slot " +
                std::to_string(up.index) +
                " that capture analysis does not report");
        }
    }
}

// The Lox slot THIS closure's own declaring push lands in, if any (only a
// named local `fun` has one — an invisible-var site at this very offset),
// AND that same closure captures as an upvalue (direct recursion). -1 when
// this CLOSURE is not that shape.
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

// Self-capture: a local `fun` that calls itself makes ONE of this CLOSURE's
// isLocal entries name the very slot it is declaring. `ensureCapturedCell`
// cannot run on that slot: its idempotent check keeps whatever cell is
// already in the slot, and that is wrong here for two reasons at once. On
// this method's very first pass through this declaration, the slot's CIL
// local has never been stored to; `.locals init` zero-initializes it, so a
// read there gives a silent `null`, not a build or run failure — CoreCLR
// does not fault an unwritten local, it only zero-initializes it. On any
// later pass (this declaration sits in a loop body), the slot instead holds
// the PREVIOUS trip's cell, because `storeClosureIntoSelfCell` below
// permanently redirects this slot's declaring store into `cell[0]` and
// never falls back to the plain raw `stloc` that would let
// `ensureCapturedCell` see a fresh raw value to wrap. Either way, the
// idempotent check would keep a stale or empty cell instead of starting
// fresh, so this seeds a brand-new cell into the slot unconditionally,
// bypassing that check, on every pass regardless of what the slot
// currently holds. `newarr` default-initializes `[0]` to null; the real
// value lands there once the closure exists, in `storeClosureIntoSelfCell`
// below. After this call the array-build loop in `emitClosure` treats the
// slot exactly like any other already-a-cell capture — no special case
// needed there.
void seedSelfCaptureCell(Emitter& e, int selfSlot) {
    e.b.emit(pushIntInstruction(1), 0, +1);
    e.b.emit("newarr [System.Runtime]System.Object", 1, 0);
    e.b.emit("stloc " + std::to_string(selfSlot), 1, -1);
}

// The declaring store, redirected: write the closure just built into the
// cell's own `[0]`, never into the CIL slot itself (which already holds
// that cell, from `seedSelfCaptureCell`) — `finishInstruction`'s ordinary
// plain `stloc` would undo the seed and hand every capturing sibling
// closure a stale cell. Spilled to `scratchSlot` first for the same reason
// `emitCapturedStore` does: building [cellRef, 0, value] for `stelem.ref`
// needs the value parked somewhere while the cell reference is fetched.
void storeClosureIntoSelfCell(Emitter& e, const DecodedInstruction& in,
                              int selfSlot, int selfLoxSlot) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("ldloc " + std::to_string(selfSlot), 0, +1);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("stelem.ref", 3, -3);

    // finishInstruction must not ALSO store this offset's invisible var: it
    // would run its plain `stloc` after the write above and put the raw
    // closure back into the slot, undoing this fix. Erase it here instead
    // of leaving finishInstruction to guess.
    auto& slots = e.invisibleVarsByOffset[in.offset];
    slots.erase(std::remove(slots.begin(), slots.end(), selfLoxSlot),
                slots.end());
    // `capturedSlots` already marks `selfLoxSlot` captured (it is one of
    // this CLOSURE's own upvalue entries), so `loadNamedLocalAtZeroDepth`'s
    // ordinary raw-or-cell test handles this cell like any other captured
    // slot — no separate self-cell flag or throw is needed here (V5/V6
    // verify it).
    e.lastInvisibleVarSlot = selfLoxSlot;
}

// `childClassNames[i]` names the class this chunk's own i-th nested
// function (chunk_decoder.h: DecodedInstruction::nestedIndex) was assigned
// by emitProgram's pre-order walk. emitScript (no nested functions in any
// caller) always passes an empty vector, so a CLOSURE reaching this from
// there is a real bug, caught below rather than silently mis-indexed.
//
// Every isLocal=1 entry's slot gets `ensureCapturedCell`'s idempotent seed
// BEFORE the array-build loop below reads it — building the `object[][]`
// upvals array must see a cell in that slot, never the raw value, whether
// this is the FIRST closure to capture that incarnation or a later one
// that only needs to share it: two closures that capture the same slot in
// the same live range must share one cell (V2_shared.lox,
// 06_shared_upvalue.lox). `up.index`, after the seed, IS the cell, read
// with a plain `ldloc`; this pass only ever tracks that slot as generic
// `object`, so `castclass` narrows it before the `stelem.ref` into the
// `object[]`-typed upvals array. `newobj` builds the whole array and calls
// the generated class's own constructor in one instruction — unlike the
// JVM backend's `new; dup; ...; invokespecial <init>` idiom, CIL's `newobj`
// pops its constructor arguments and pushes the fresh reference itself, so
// no `dup` is needed to keep a copy of the still-uninitialized object
// around.
void emitClosure(Emitter& e, const DecodedInstruction& in,
                 const std::vector<std::string>& childClassNames) {
    if (in.nestedIndex < 0 ||
        static_cast<std::size_t>(in.nestedIndex) >= childClassNames.size()) {
        throw std::runtime_error("clr_emitter: CLOSURE nestedIndex " +
                                 std::to_string(in.nestedIndex) +
                                 " has no assigned class name");
    }
    const std::string& cls =
        childClassNames[static_cast<std::size_t>(in.nestedIndex)];

    checkAllCapturesAreReported(e, in);

    int selfLoxSlot = findSelfCaptureLoxSlot(e, in);
    int selfSlot = -1;
    if (selfLoxSlot >= 0) {
        selfSlot = e.slotForLocal(selfLoxSlot);
        seedSelfCaptureCell(e, selfSlot);
    }

    for (std::size_t u = 0; u < in.upvalues.size(); u++) {
        const ClosureUpvalue& up = in.upvalues[u];
        if (up.isLocal && up.index != selfLoxSlot) {
            ensureCapturedCell(e, e.slotForLocal(up.index), in.offset,
                               static_cast<int>(u));
        }
    }

    e.b.emit(pushIntInstruction(static_cast<int>(in.upvalues.size())), 0, +1);
    e.b.emit("newarr object[]", 1, 0);
    for (std::size_t u = 0; u < in.upvalues.size(); u++) {
        const ClosureUpvalue& up = in.upvalues[u];
        e.b.emit("dup", 1, +1);
        e.b.emit(pushIntInstruction(static_cast<int>(u)), 0, +1);
        if (up.isLocal) {
            // `selfLoxSlot` already holds a fresh cell (seeded above), so
            // this is the same lowering as any other already-a-cell
            // capture — no special case needed here.
            e.b.emit("ldloc " + std::to_string(e.slotForLocal(up.index)), 0,
                     +1);
            e.b.emit("castclass object[]", 1, 0);
        } else {
            // A grandparent's own upvalue, already a cell — copy the
            // reference straight through, no seed: isLocal = 0 takes the
            // parent's own upvalue at that index.
            e.b.emit("ldarg.0", 0, +1);
            e.b.emit("ldfld object[][] [LoxRuntime]Lox.LoxClosure::Upvalues", 1,
                     0);
            e.b.emit(pushIntInstruction(up.index), 0, +1);
            e.b.emit("ldelem.ref", 2, -1);
        }
        e.b.emit("stelem.ref", 3, -3);
    }
    e.b.emit("newobj instance void " + cls + "::.ctor(object[][])", 1, 0);

    if (selfSlot >= 0) {
        storeClosureIntoSelfCell(e, in, selfSlot, selfLoxSlot);
    }
}

// GET_UPVALUE n reads upvals[n][0]: the `Upvalues` field LoxClosure
// declares (runtime/clr/src/LoxClosure.cs) is already typed `object[][]`,
// so `ldelem.ref` on it needs no cast — unlike a captured LOCAL slot, which
// this pass only ever tracks as plain `object` (see
// `emitCapturedGetLocal`). `ldarg.0` is `this` — the LoxClosure instance
// itself, distinct from `Invoke`'s own `self` parameter (`ldarg.1`).
void emitGetUpvalue(Emitter& e, const DecodedInstruction& in) {
    e.b.emit("ldarg.0", 0, +1);
    e.b.emit("ldfld object[][] [LoxRuntime]Lox.LoxClosure::Upvalues", 1, 0);
    e.b.emit(pushIntInstruction(in.byteOperand), 0, +1);
    e.b.emit("ldelem.ref", 2, -1);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldelem.ref", 2, -1);
}

// SET_UPVALUE's own P2 peek/fuse shuffle: the assigned value already sits
// on top of the stack, so it is spilled to `scratchSlot` (same reasoning as
// `emitCapturedStore`) while `upvals[index]` is fetched, then written back
// into that cell's slot 0.
void emitUpvalueStore(Emitter& e, int index, bool peek) {
    std::string scratch = std::to_string(e.scratchSlot);
    e.b.emit("stloc " + scratch, 1, -1);
    e.b.emit("ldarg.0", 0, +1);
    e.b.emit("ldfld object[][] [LoxRuntime]Lox.LoxClosure::Upvalues", 1, 0);
    e.b.emit(pushIntInstruction(index), 0, +1);
    e.b.emit("ldelem.ref", 2, -1);
    e.b.emit("ldc.i4.0", 0, +1);
    e.b.emit("ldloc " + scratch, 0, +1);
    e.b.emit("stelem.ref", 3, -3);
    if (peek) {
        e.b.emit("ldloc " + scratch, 0, +1);
    }
}

// SET_UPVALUE is a peek-family consumer (an assignment expression), so it
// asks `isFoldedAtZeroDepth` rather than re-deriving
// `before[i].operandDepth() == 0` inline — the one shared guard SET_LOCAL/
// SET_GLOBAL/JUMP_IF_FALSE already use (clr_emitter.h's own top-of-file
// note).
void emitSetUpvalue(Emitter& e, std::size_t i, const DecodedInstruction& in,
                    bool& consumedFollowingPop) {
    bool fuse = e.fusablePop(i);
    if (isFoldedAtZeroDepth(e, i)) {
        loadNamedLocalAtZeroDepth(e, i, in.offset);
        emitUpvalueStore(e, in.byteOperand, /*peek=*/false);
    } else {
        emitUpvalueStore(e, in.byteOperand, /*peek=*/!fuse);
    }
    consumedFollowingPop = fuse;
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
// value into a named local before RETURN runs, with no separate load in the
// bytecode — the same eager invisible-var materialization SET_LOCAL/
// SET_GLOBAL/JUMP_IF_FALSE already handle. `normalizeFoldedOperands`
// (driven by `nativePops`'s own RETURN row) already repairs that fold
// before this function runs, the same as it does for every other
// `nativePops`-covered consumer, so this function no longer inspects depth
// to decide whether to load anything — only to confirm the repair left the
// stack in the shape `ret` needs.
void emitReturn(Emitter& e, bool isFunction) {
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
                              std::size_t i, bool consumedFollowingPop,
                              bool consumedFollowingJumpTable) {
    auto varsIt = e.invisibleVarsByOffset.find(in.offset);
    if (varsIt != e.invisibleVarsByOffset.end()) {
        for (int slot : varsIt->second) {
            e.b.emit("stloc " + std::to_string(e.slotForLocal(slot)), 1, -1);
            e.lastInvisibleVarSlot = slot;
        }
    }

    std::size_t nextIndex =
        i + (consumedFollowingPop || consumedFollowingJumpTable ? 2 : 1);
    // `in.op` reads GET_TAG here whenever this instruction fused away a
    // following JUMP_TABLE — GET_TAG alone falls through, but the emitted
    // `switch` never does (every arm, and the out-of-range case, are real
    // jump targets, same as JUMP/LOOP/MATCH_ERROR): CIL's `switch` does
    // physically fall through into the next instruction when the index is
    // out of range, but that next instruction is MATCH_ERROR's own CFG
    // label, which the label-resync test below re-derives from the shared
    // abstract-stack analysis regardless of this flag — so treating the
    // fused pair as "does not fall through" costs nothing and avoids
    // depending on that physical coincidence.
    e.prevCanFallThrough = in.op != Op::JUMP && in.op != Op::LOOP &&
                           in.op != Op::RETURN && in.op != Op::MATCH_ERROR &&
                           !consumedFollowingJumpTable;
    e.prevNaturalSuccessorOffset = (nextIndex < e.fn.instructions.size())
                                       ? e.fn.instructions[nextIndex].offset
                                       : -1;
    return nextIndex;
}

// The one place every `nativePops`-covered consumer (native_pops.h) gets its
// folded bottom operand(s) repaired, sharing that one target-independent
// table with jvm_emitter.cpp's own mechanism of the same name and purpose.
// `deficit` is how many of this instruction's own `nativePops` cells the
// shared abstract-stack analysis has already folded into named locals
// (compileMatchBody's own "fused local/operand-stack model") instead of
// leaving on the real evaluation stack. A folded operand is always the
// BOTTOM-most block of an instruction's own operands — `compileMatchBody`
// folds a `match` expression's result into its own named local (and,
// starting with the fix that reserves a phantom local per live sibling
// operand, folds every OTHER live sibling of that same consumer right along
// with it, once anything inside the match references a slot number above
// them) before any later sibling operand is even parsed — so this repairs
// every `nativePops`-covered opcode's own folded-operand shape uniformly,
// regardless of how many of its operands are folded at once.
//
// deficit <= 0: every operand this instruction needs is already, physically,
// on the stack (any extra depth below belongs to an outer expression and is
// untouched) — the ordinary case, and what every CUSTOM row gets
// unconditionally too; this function never computes a deficit for one.
//
// deficit >= 1: `genuineCount = *pops - deficit` values are still genuinely
// on the stack, on top of where the folded ones belong. Zero or one genuine
// value spills to `e.scratchSlot` (the same single slot SET_PROPERTY/
// DEFINE_METHOD/GET_SUPER already spill their own second operand into); two
// or more spill into `e.argScratchBase`, `computeAggregateNeeds`'s own spill
// area, sized for exactly this by its own scan. Once spilled, the `deficit`
// folded locals load in ascending slot order — `resolveTopLoxSlot` names
// the topmost one, cross-checked the same way every other zero-depth
// consumer's slot is; the rest sit contiguously beneath it, LIFO, per
// abstract_stack.h's own local/temporary boundary — then the genuine values
// reload on top, in their original order.
//
// This does not stop at a deficit of 1 the way jvm_emitter.cpp's own
// `normalizeFoldedOperands` still does: that ceiling guarded against a
// `compileMatchBody` slot-allocation defect that made two-or-more-deep
// folding collide with a live sibling operand on `build/loxpp` itself, with
// no correct native answer to reproduce. That defect is fixed in the
// compiler this pass reads its bytecode from — measured directly, not
// assumed — so a deeper fold now has a real native answer, and this
// generalizes to `deficit` of any size to match it, rather than refusing a
// case the JVM backend still refuses out of a now-stale caution.
void normalizeFoldedOperands(Emitter& e, std::size_t i,
                             const DecodedInstruction& in) {
    std::optional<int> pops = nativePops(in.op, in);
    if (!pops.has_value()) {
        return;
    }
    int deficit = *pops - e.analysis.before[i].operandDepth();
    if (deficit <= 0) {
        return;
    }
    // A fold can only ever explain a deficit up to how many locals are
    // currently bound: `compileMatchBody` folds a value INTO a local, never
    // out of thin air. A deficit wider than that is not a fold at all — a
    // genuine evaluation-stack underflow the compiler itself would never
    // produce — so this leaves it for the instruction's own `Builder::emit`
    // read-count check to name, against the real CIL instruction being
    // assembled, rather than resolving a Lox slot that does not exist.
    if (e.analysis.before[i].localCount < deficit) {
        return;
    }
    int genuineCount = *pops - deficit;
    if (genuineCount == 1) {
        e.b.emit("stloc " + std::to_string(e.scratchSlot), 1, -1);
    } else if (genuineCount >= 2) {
        for (int k = 0; k < genuineCount; k++) {
            e.b.emit("stloc " + std::to_string(e.argScratchBase + k), 1, -1);
        }
    }

    int topLoxSlot = resolveTopLoxSlot(e, i, in.offset);
    // `k` also serves as the captured-slot label sub-index: this loop can
    // call loadLoxSlot more than once at this SAME offset, and two of those
    // calls landing on a captured slot would otherwise both ask
    // emitCapturedGetLocal for the offset-only label pair ("gr"+offset /
    // "ge"+offset), which ilasm rejects as a duplicate the second time.
    for (int k = deficit - 1; k >= 0; k--) {
        loadLoxSlot(e, topLoxSlot - k, in.offset, k);
    }

    if (genuineCount == 1) {
        e.b.emit("ldloc " + std::to_string(e.scratchSlot), 0, +1);
    } else if (genuineCount >= 2) {
        for (int k = genuineCount - 1; k >= 0; k--) {
            e.b.emit("ldloc " + std::to_string(e.argScratchBase + k), 0, +1);
        }
    }
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
        bool consumedFollowingJumpTable = false;

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

        normalizeFoldedOperands(e, i, in);

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
        case Op::BUILD_LIST:
            emitBuildList(e, in);
            break;
        case Op::BUILD_MAP:
            emitBuildMap(e, in);
            break;
        case Op::GET_INDEX:
            emitGetIndex(e);
            break;
        case Op::SET_INDEX:
            emitSetIndex(e);
            break;
        case Op::SLICE:
            emitSlice(e);
            break;
        case Op::IN:
            emitIn(e);
            break;
        case Op::IS_SEQ:
            emitIsSeq(e);
            break;
        case Op::GET_ITER:
            emitGetIter(e, i, in);
            break;
        case Op::ITER_HAS_NEXT:
            emitIterHasNext(e);
            break;
        case Op::ITER_NEXT:
            emitIterNext(e);
            break;
        case Op::CLOSURE:
            emitClosure(e, in, childClassNames);
            break;
        case Op::GET_UPVALUE:
            emitGetUpvalue(e, in);
            break;
        case Op::SET_UPVALUE:
            emitSetUpvalue(e, i, in, consumedFollowingPop);
            break;
        case Op::CLOSE_UPVALUE:
            // A local reclaim (notes/jvm-emission-contract.md's
            // scope-exit rule: endScope emits this exactly where a POP
            // would otherwise retire a captured local) — nothing on the
            // CIL evaluation stack to pop, same as emitPop's own
            // LOCAL_RECLAIM case. The cell it ends stays wherever it
            // already sits; the NEXT declaration into that same slot
            // always re-`stloc`s it directly (never through
            // ensureCapturedCell's check), which is what actually puts the
            // slot back to raw for whatever comes after — see
            // ensureCapturedCell's own note.
            break;
        case Op::RETURN:
            emitReturn(e, isFunction);
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
            notImplemented(in.op);
        }

        i = finishInstruction(e, in, i, consumedFollowingPop,
                              consumedFollowingJumpTable);
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

// What this chunk needs from the shared scratch area: `maxWidth` is the
// widest aggregate it builds by spilling loose operand-stack values to
// scratch locals before assembling one aggregate object — CALL's/INVOKE's/
// SUPER_INVOKE's own argument count, BUILD_LIST's own element count,
// BUILD_MAP's own pair count doubled (key, value per pair), or
// `normalizeFoldedOperands`'s own deficit-vs-genuine spill (two or more
// genuine operands still live above a folded bottom block), whichever is
// wider (jvm_emitter.cpp's own computeMaxSpillWidth is the JVM twin of this
// scan). `needsCalleeSlot` is true whenever some instruction in the chunk
// needs `e.calleeScratchSlot` itself even at width 0: GET_SUPER and
// SUPER_INVOKE always spill self into it (CIL has no `swap`, so even their
// own zero-argument shape needs a second scratch slot alongside
// `e.scratchSlot`'s superclass hold — the JVM backend's own `swap` avoids
// this second slot, so this flag has no JVM twin). A plain CALL/INVOKE only
// needs it once its own argCount is at least 1 (its own argCount == 0 path
// never touches it), which `maxWidth > 0` already captures below.
struct AggregateNeeds {
    int maxWidth{0};
    bool needsCalleeSlot{false};
};

AggregateNeeds computeAggregateNeeds(const DecodedFunction& fn,
                                     const FunctionStackAnalysis& analysis) {
    AggregateNeeds needs;
    for (std::size_t i = 0; i < fn.instructions.size(); i++) {
        const DecodedInstruction& instr = fn.instructions[i];
        switch (instr.op) {
        case Op::CALL:
        case Op::BUILD_LIST:
        case Op::INVOKE:
            needs.maxWidth = std::max(needs.maxWidth, instr.byteOperand);
            break;
        case Op::BUILD_MAP:
            needs.maxWidth = std::max(needs.maxWidth, 2 * instr.byteOperand);
            break;
        case Op::SUPER_INVOKE:
            needs.maxWidth = std::max(needs.maxWidth, instr.byteOperand);
            needs.needsCalleeSlot = true;
            break;
        case Op::GET_SUPER:
            needs.needsCalleeSlot = true;
            break;
        default:
            break;
        }
        std::optional<int> pops = nativePops(instr.op, instr);
        if (pops.has_value()) {
            int deficit = *pops - analysis.before[i].operandDepth();
            if (deficit >= 1) {
                int genuineCount = *pops - deficit;
                if (genuineCount >= 2) {
                    needs.maxWidth = std::max(needs.maxWidth, genuineCount);
                }
            }
        }
    }
    return needs;
}

// `captureInfo` is this chunk's own entry from `analyzeCaptures`
// (capture_analysis.h) — see emitChunk's callers.
Emitter buildEmitter(const DecodedFunction& fn,
                     const FunctionStackAnalysis& analysis, int maxLocalCount,
                     const AggregateNeeds& aggregateNeeds,
                     const FunctionCaptureInfo& captureInfo) {
    Emitter e{fn, analysis, {}};
    e.scratchSlot = e.baseSlot + maxLocalCount;
    if (aggregateNeeds.needsCalleeSlot || aggregateNeeds.maxWidth > 0) {
        // emitBuildList spills only into argScratchBase, one slot per
        // element, and never touches calleeScratchSlot; reserving it
        // unconditionally here is simpler than tracking exactly which
        // opcode in THIS chunk is the one that needs it.
        e.calleeScratchSlot = e.scratchSlot + 1;
        e.argScratchBase = e.scratchSlot + 2;
    }

    for (const PopClassification& p : analysis.pops) {
        e.popKinds[p.offset] = p.kind;
    }
    for (const InvisibleVarSite& site : analysis.invisibleVars) {
        e.invisibleVarsByOffset[site.offset].push_back(site.slot);
    }
    // Which of this chunk's OWN local slots ever back an object[1] cell —
    // every slot some reachable CLOSURE in this chunk captures
    // (capture_analysis.h). Membership only; see ensureCapturedCell's own
    // note for why the exact live range is not what GET_LOCAL/SET_LOCAL/
    // CLOSURE need from the capture analysis here.
    for (const auto& entry : captureInfo.liveRangesBySlot) {
        e.capturedSlots.insert(entry.first);
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
// so only the upvalues array is a real parameter — each entry either a
// freshly-seeded cell or a parent's own cell/upvalue, wired by
// emitClosure; this shape does not change with the array's contents.
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
// i-th nested function was assigned — see emitProgram. `captureInfo` is
// this same chunk's own entry from `analyzeCaptures` (capture_analysis.h)
// — see buildEmitter's use of it.
std::string emitChunk(const DecodedFunction& fn,
                      const FunctionStackAnalysis& analysis,
                      const std::string& className, bool isFunction,
                      const std::vector<std::string>& childClassNames,
                      const FunctionCaptureInfo& captureInfo) {
    int maxLocalCount = computeMaxLocalCount(analysis);
    AggregateNeeds aggregateNeeds = computeAggregateNeeds(fn, analysis);
    bool needsScratchArea =
        aggregateNeeds.needsCalleeSlot || aggregateNeeds.maxWidth > 0;
    int extraSpillSlots = needsScratchArea ? aggregateNeeds.maxWidth + 1 : 0;

    Emitter e =
        buildEmitter(fn, analysis, maxLocalCount, aggregateNeeds, captureInfo);
    emitPrologue(e, fn, isFunction);
    emitBody(e, isFunction, childClassNames);

    // globals (1) + the Lox frame's own slots + the shuffle scratch (1) +
    // the aggregate spill area, if this chunk needs one.
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
             const CaptureAnalysis& captures, std::ostringstream& out) {
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
                     childClassNames, captures.functions.at(fn.id));

    for (std::size_t i = 0; i < fn.nested.size(); i++) {
        emitAll(fn.nested[i], node.nested[i], /*isRoot=*/false, names, captures,
                out);
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
    CaptureAnalysis captures = analyzeCaptures(fn);
    return emitHeader(className) + emitChunk(fn, analysis, className,
                                             /*isFunction=*/false, {},
                                             captures.functions.at(fn.id));
}

std::string emitProgram(const DecodedFunction& root,
                        const StackAnalysisTree& tree,
                        const std::string& scriptClassName) {
    std::unordered_map<std::string, std::string> names;
    int counter = 0;
    assignClassNames(root, /*isRoot=*/true, scriptClassName, counter, names);

    CaptureAnalysis captures = analyzeCaptures(root);

    std::ostringstream out;
    out << emitHeader(scriptClassName);
    emitAll(root, tree, /*isRoot=*/true, names, captures, out);
    return out.str();
}

} // namespace clr
