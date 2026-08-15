#include "jvm_emitter.h"

#include "exec_objects.h"
#include "object.h"
#include "value.h"

#include <algorithm>
#include <array>
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
    throw std::runtime_error("not implemented in N4: " + opName(op));
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

std::string formatJasminDouble(double value) {
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%.17g", value);
    std::string s(buf.data());
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos) {
        s += ".0";
    }
    return s;
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
        auto it = popKinds.find(ins[j].offset);
        return it != popKinds.end() && it->second == PopKind::TEMP;
    };
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

    b.emit("invokestatic lox/LoxRuntime/init()Llox/LoxGlobals;", +1);
    b.emit("astore " + std::to_string(globalsSlot), -1);

    for (std::size_t i = 0; i < n;) {
        if (!reached(i)) {
            i++;
            continue; // endCompiler()'s trailing NIL;RETURN can be dead code.
        }
        const DecodedInstruction& in = ins[i];
        bool consumedFollowingPop = false;

        switch (in.op) {
        case Op::CONSTANT: {
            Value v = fn.function->chunk.getConstant(
                static_cast<uint16_t>(in.constantIndex));
            if (is<Number>(v)) {
                b.emit("ldc2_w " + formatJasminDouble(as<Number>(v)), +2);
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
            if (fusablePop(i)) {
                b.emit("astore " + std::to_string(slot), -1);
                consumedFollowingPop = true;
            } else {
                b.emit("dup", +1);
                b.emit("astore " + std::to_string(slot), -1);
            }
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
            globalsCall("set", constantString(in.constantIndex),
                        /*peek=*/!fuse);
            consumedFollowingPop = fuse;
            break;
        }
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
            }
        }

        i += consumedFollowingPop ? 2 : 1;
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
