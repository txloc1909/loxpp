#include "qbe_emitter.h"

#include "cfg.h"
#include "exec_objects.h" // ObjFunction (chunk/arity), for CONSTANT/global-name lookups
#include "native_pops.h" // opName(Op), for error messages
#include "object.h"
#include "value.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace qbe {

// Q6, notes/qbe-backend.md: --target qbe requires NaN tagging. Every bit
// pattern this file bakes into a .ssa literal (QNAN, VAL_NIL, VAL_TRUE,
// VAL_FALSE, OBJ_TAG) only means anything under that layout.
static_assert(sizeof(Value) == 8, "qbe_emitter requires LOXPP_NAN_TAGGING (Q6, "
                                  "notes/qbe-backend.md)");

namespace {

// Bit-pattern constants this file needs from value.h's NaN-boxing (detail
// namespace — reused, not duplicated, so a layout change there cannot drift
// silently from what this emitter bakes into .ssa literals).
using detail::OBJ_TAG;
using detail::QNAN;
using detail::VAL_FALSE;
using detail::VAL_NIL;
using detail::VAL_TRUE;

[[noreturn]] void unsupported(Op op) {
    throw std::runtime_error(
        "qbe_emitter: opcode not supported by this node (S3 straight-line "
        "code and jumps, notes/qbe-backend.md): " +
        opName(op));
}

// One function's own emission state: temp/data-symbol counters, the offset
// -> before/after StackState lookup (abstract_stack.h aligns `analysis`
// 1:1 with `fn.instructions`, not with a cfg BasicBlock's own copied
// instruction list, so this map is how a block's instruction finds its own
// heights), and the two output streams (`data` for embedded name literals,
// `body` for the function's own QBE text).
class Emitter {
  public:
    Emitter(const DecodedFunction& fn, const FunctionStackAnalysis& analysis,
            std::string qbeSymbol)
        : m_fn(fn), m_symbol(std::move(qbeSymbol)) {
        for (std::size_t i = 0; i < fn.instructions.size(); i++) {
            m_stateAt.emplace(fn.instructions[i].offset,
                              std::pair{analysis.before[i], analysis.after[i]});
            m_reachedAt.emplace(fn.instructions[i].offset, analysis.reached[i]);
            // Q3 (notes/qbe-backend.md): the high-water mark over every
            // REACHED instruction's raw height (locals and temporaries
            // together — StackState::height, not operandDepth()) is this
            // function's own worst-case slot usage. Skipping unreached
            // instructions matches leaderReached()'s own treatment of the
            // compiler's trailing NIL;RETURN below — its height is
            // meaningless dead-code bookkeeping, not a real bound.
            if (analysis.reached[i]) {
                m_maxHeight = std::max({m_maxHeight, analysis.before[i].height,
                                        analysis.after[i].height});
            }
        }
    }

    std::string run() {
        Cfg cfg = buildCfg(m_fn.instructions);
        if (!cfg.handlerEntries.empty()) {
            throw std::runtime_error(
                "qbe_emitter: PUSH_HANDLER not supported by this node (S3/S4 "
                "straight-line code, jumps, calls and closures) — see S6, "
                "#459");
        }
        if (cfg.blocks.empty()) {
            throw std::runtime_error(
                "qbe_emitter: function with no basic blocks — "
                "decoder/compiler drift");
        }

        m_body << "export function w $" << m_symbol << "(l %rt, l %base) {\n";
        emitStackCheck(cfg.blocks.front().label);
        for (std::size_t bi = 0; bi < cfg.blocks.size(); bi++) {
            emitBlock(cfg.blocks[bi], bi, cfg);
        }
        m_body << "}\n";

        std::ostringstream out;
        out << m_data.str() << m_body.str();
        return out.str();
    }

  private:
    const DecodedFunction& m_fn;
    std::string m_symbol;
    std::unordered_map<int, std::pair<StackState, StackState>> m_stateAt;
    std::unordered_map<int, bool> m_reachedAt;
    std::ostringstream m_data;
    std::ostringstream m_body;
    int m_tempCounter{0};
    int m_dataCounter{0};
    int m_maxHeight{0};

    std::string newTemp() { return "%t" + std::to_string(m_tempCounter++); }

    const std::pair<StackState, StackState>& stateOf(int offset) const {
        return m_stateAt.at(offset);
    }

    // A block whose leader is unreached (analyzeStack's `reached`, false
    // for the compiler's own trailing NIL;RETURN when every path already
    // returned explicitly — abstract_stack.h) has no live predecessor, but
    // QBE still requires every declared block to end in a valid
    // terminator. Emitting nothing but that terminator keeps the .ssa
    // structurally valid without running dead code.
    bool leaderReached(const BasicBlock& block) const {
        if (block.instructions.empty()) {
            return true;
        }
        return m_reachedAt.at(block.instructions.front().offset);
    }

    // Address of the stack cell at `height` (clox's fused-stack model,
    // notes/qbe-backend.md's central design choice): base + 8*height.
    std::string addr(int height) {
        std::string t = newTemp();
        m_body << "\t" << t << " =l add %base, " << (height * 8) << "\n";
        return t;
    }

    std::string loadl(const std::string& address) {
        std::string t = newTemp();
        m_body << "\t" << t << " =l loadl " << address << "\n";
        return t;
    }

    void storel(const std::string& value, const std::string& address) {
        m_body << "\tstorel " << value << ", " << address << "\n";
    }

    // Interns `name`'s literal text as a `data` C-string and returns a
    // fresh temp holding the ObjString* the interned Value carries — never
    // the raw Value bits, since a caller of an op*_global wrapper needs an
    // ObjString* (rt_capi.h). MemoryManager::makeString interns by content
    // (memory_manager.cpp), so calling rt_new_string on this same literal
    // text at every dynamic execution always resolves to the SAME
    // ObjString* the startup recompile's own DEFINE_GLOBAL/GET_GLOBAL/
    // SET_GLOBAL used — this emitter never sees, and does not need, the
    // pointer value itself.
    std::string internedNamePtr(const std::string& text) {
        std::string sym =
            "$" + m_symbol + "_name" + std::to_string(m_dataCounter++);
        m_data << "data " << sym << " = { b \"" << escapeForQbeData(text)
               << "\", b 0 }\n";
        std::string bits = newTemp();
        m_body << "\t" << bits << " =l call $rt_new_string(l %rt, l " << sym
               << ")\n";
        std::string ptr = newTemp();
        // Value carrying an Obj*: OBJ_TAG | ptr (value.h). Mask it back off.
        m_body << "\t" << ptr << " =l and " << bits << ", " << (~OBJ_TAG)
               << "\n";
        return ptr;
    }

    // The currently executing function's own closure, as a Value — never
    // read out of the frame's own stack window (addr(0)): base[0] is that
    // closure only when this frame was entered by a direct CALL on a
    // closure Value. A method's own frame (init(), INVOKE, SUPER_INVOKE —
    // S5, #458) has the receiver ("this") at base[0] instead, so reading
    // addr(0) there hands rt_constant_at/rt_get_upvalue/rt_set_upvalue an
    // ObjInstance* it then misreads as an ObjClosure*. rt_current_closure
    // (rt_capi.h) asks the Runtime's own current CallFrame instead, which
    // is correct regardless of how this frame was entered.
    std::string ownClosureBits() {
        std::string bits = newTemp();
        m_body << "\t" << bits << " =l call $rt_current_closure(l %rt)\n";
        return bits;
    }

    // Reads constant `constantIndex` from the CURRENTLY EXECUTING function's
    // own constant pool, via rt_constant_at (rt_capi.h) — the same
    // mechanism CLOSURE already uses for its own FUNCTION constant. Unlike
    // internedNamePtr above, this never re-interns anything at runtime: the
    // constant already exists as a real object, built by rt_startup's own
    // embed-and-recompile step, so this just reads the SAME Value the
    // native VM's own CONSTANT/GET_PROPERTY/etc. read from this function's
    // chunk (S5, #458 — "materialise", bytecode-translation-problems.md
    // P6).
    std::string constantValueBits(int constantIndex) {
        // ownClosureBits() itself emits an instruction as a side effect
        // (its own `call`), so — like addr() — it must be computed as its
        // own statement first, never inlined into this ongoing m_body <<
        // chain: doing so interleaves the two instructions' text (see
        // Op::CLOSURE's own comment on this same hazard).
        std::string ownClosure = ownClosureBits();
        std::string bits = newTemp();
        m_body << "\t" << bits << " =l call $rt_constant_at(l %rt, l "
               << ownClosure << ", w " << constantIndex << ")\n";
        return bits;
    }

    // Same as constantValueBits, but masks off OBJ_TAG (value.h) to return
    // the raw ObjString* every rt_op_* wrapper taking a `name` argument
    // expects (rt_capi.h) — the constant at this index must be a String.
    std::string constantStringPtr(int constantIndex) {
        std::string bits = constantValueBits(constantIndex);
        std::string ptr = newTemp();
        m_body << "\t" << ptr << " =l and " << bits << ", " << (~OBJ_TAG)
               << "\n";
        return ptr;
    }

    // QBE `data` string bodies take a quoted C string; backslash and quote
    // are the only bytes that need escaping to stay inside one `b "..."`
    // item. A global-variable name is always a valid Lox++ identifier
    // (scanner.cpp), so neither byte can actually appear here — this guards
    // the general case anyway rather than assuming it.
    static std::string escapeForQbeData(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out;
    }

    // is<Number>(v): (bits & QNAN) != QNAN (value.h). Returns a `w` 0/1.
    std::string isNumberCheck(const std::string& bits) {
        std::string masked = newTemp();
        m_body << "\t" << masked << " =l and " << bits << ", " << QNAN << "\n";
        std::string t = newTemp();
        m_body << "\t" << t << " =w cnel " << masked << ", " << QNAN << "\n";
        return t;
    }

    // Runs a runtime call whose arguments (beyond `l %rt`) are already typed
    // (each entry is a full "l %x" / "w 5" token) and, on a nonzero status,
    // returns 1 immediately — the simplified fatal-only contract this node
    // uses for every runtime call (see qbe_emitter.h's file comment on the
    // gap this leaves for S6).
    void callSlowPathTyped(const std::string& fnName, int topHeight,
                           const std::vector<std::string>& typedArgs) {
        std::string topAddr = addr(topHeight);
        m_body << "\tcall $rt_set_top(l %rt, l " << topAddr << ")\n";
        std::string status = newTemp();
        m_body << "\t" << status << " =w call $" << fnName << "(l %rt";
        for (const std::string& a : typedArgs) {
            m_body << ", " << a;
        }
        m_body << ")\n";
        std::string bad = newTemp();
        m_body << "\t" << bad << " =w cnew " << status << ", 0\n";
        std::string failLabel =
            "@" + m_symbol + "_fail" + std::to_string(m_tempCounter);
        std::string okLabel =
            "@" + m_symbol + "_ok" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << bad << ", " << failLabel << ", " << okLabel
               << "\n";
        m_body << failLabel << "\n\tret 1\n";
        m_body << okLabel << "\n";
    }

    // `topHeight` is set via rt_set_top before the call (Q1: every value
    // must already live in its own stack slot before a call that can
    // allocate or unwind). `stopAtFrameCount` is nullopt for the two
    // wrappers whose C signature has no such trailing parameter
    // (rt_op_print, rt_op_define_global — rt_capi.h): QBE's `call` performs
    // no prototype check, so passing an extra argument a callee's own body
    // never reads would go unnoticed here but is still undefined behavior
    // against the real C signature — this must match rt_capi.h exactly, not
    // "whatever happens to work" on one ABI.
    void callSlowPath(const std::string& fnName, int topHeight,
                      const std::vector<std::string>& extraArgs,
                      std::optional<int> stopAtFrameCount) {
        std::vector<std::string> typedArgs;
        typedArgs.reserve(extraArgs.size() + 1);
        for (const std::string& a : extraArgs) {
            typedArgs.push_back("l " + a);
        }
        if (stopAtFrameCount.has_value()) {
            typedArgs.push_back("w " + std::to_string(*stopAtFrameCount));
        }
        callSlowPathTyped(fnName, topHeight, typedArgs);
    }

    // Emitted once, at the very top of every compiled function, before its
    // first cfg block: checks this function's own analyzed max stack height
    // (m_maxHeight) against STACK_MAX (Q3, notes/qbe-backend.md —
    // Runtime::checkStackOverflow's own comment, runtime.h, explains why
    // compiled code needs a check push() never gives it). QBE requires
    // every block to end in an explicit terminator — no implicit
    // fallthrough between labels — so this own prologue "block" needs its
    // own label and an explicit jmp into the function's real entry block.
    void emitStackCheck(const std::string& firstBlockLabel) {
        m_body << "@" << m_symbol << "_entry\n";
        std::string neededAddr = addr(m_maxHeight);
        std::string status = newTemp();
        m_body << "\t" << status << " =w call $rt_check_stack(l %rt, l "
               << neededAddr << ", w 0)\n";
        std::string bad = newTemp();
        m_body << "\t" << bad << " =w cnew " << status << ", 0\n";
        std::string failLabel = "@" + m_symbol + "_entry_fail";
        std::string okLabel = "@" + m_symbol + "_entry_ok";
        m_body << "\tjnz " << bad << ", " << failLabel << ", " << okLabel
               << "\n";
        m_body << failLabel << "\n\tret 1\n";
        m_body << okLabel << "\n";
        m_body << "\tjmp @" << firstBlockLabel << "\n";
    }

    // Binary numeric op with an inline plain-double fast path (Q5): `a OP
    // b`, operands at heights (in-2, in-1), result at height in-2 (out =
    // in-1). `fastOp` is the QBE double instruction (`add`/`sub`/`mul`/
    // `div`); `slowFn` is the rt_capi wrapper for the whole opcode
    // (including its non-number cases: string concatenation for ADD,
    // operator-overload dispatch for the rest).
    void emitBinaryArith(const std::string& fastOp, const std::string& slowFn,
                         int heightIn, int stopAtFrameCount) {
        std::string aAddr = addr(heightIn - 2);
        std::string bAddr = addr(heightIn - 1);
        std::string aBits = loadl(aAddr);
        std::string bBits = loadl(bAddr);
        std::string aNum = isNumberCheck(aBits);
        std::string bNum = isNumberCheck(bBits);
        std::string both = newTemp();
        m_body << "\t" << both << " =w and " << aNum << ", " << bNum << "\n";
        std::string fastLabel =
            "@" + m_symbol + "_fast" + std::to_string(m_tempCounter);
        std::string slowLabel =
            "@" + m_symbol + "_slow" + std::to_string(m_tempCounter);
        std::string doneLabel =
            "@" + m_symbol + "_done" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << both << ", " << fastLabel << ", " << slowLabel
               << "\n";
        m_body << slowLabel << "\n";
        callSlowPath(slowFn, heightIn, {}, stopAtFrameCount);
        m_body << "\tjmp " << doneLabel << "\n";
        m_body << fastLabel << "\n";
        std::string aD = newTemp();
        m_body << "\t" << aD << " =d cast " << aBits << "\n";
        std::string bD = newTemp();
        m_body << "\t" << bD << " =d cast " << bBits << "\n";
        std::string rD = newTemp();
        m_body << "\t" << rD << " =d " << fastOp << " " << aD << ", " << bD
               << "\n";
        std::string rBits = newTemp();
        m_body << "\t" << rBits << " =l cast " << rD << "\n";
        storel(rBits, aAddr); // heightIn - 2 == heightIn(out) - 1
        m_body << "\tjmp " << doneLabel << "\n";
        m_body << doneLabel << "\n";
    }

    // Binary comparison (LESS/GREATER/EQUAL): same fast/slow split, but the
    // fast path's result is a Boolean Value, not a raw double — bits =
    // VAL_FALSE + (0|1), since VAL_TRUE == VAL_FALSE + 1 (value.h).
    void emitBinaryCompare(const std::string& qbeCmp, const std::string& slowFn,
                           int heightIn, int stopAtFrameCount) {
        std::string aAddr = addr(heightIn - 2);
        std::string bAddr = addr(heightIn - 1);
        std::string aBits = loadl(aAddr);
        std::string bBits = loadl(bAddr);
        std::string aNum = isNumberCheck(aBits);
        std::string bNum = isNumberCheck(bBits);
        std::string both = newTemp();
        m_body << "\t" << both << " =w and " << aNum << ", " << bNum << "\n";
        std::string fastLabel =
            "@" + m_symbol + "_fast" + std::to_string(m_tempCounter);
        std::string slowLabel =
            "@" + m_symbol + "_slow" + std::to_string(m_tempCounter);
        std::string doneLabel =
            "@" + m_symbol + "_done" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << both << ", " << fastLabel << ", " << slowLabel
               << "\n";
        m_body << slowLabel << "\n";
        callSlowPath(slowFn, heightIn, {}, stopAtFrameCount);
        m_body << "\tjmp " << doneLabel << "\n";
        m_body << fastLabel << "\n";
        std::string aD = newTemp();
        m_body << "\t" << aD << " =d cast " << aBits << "\n";
        std::string bD = newTemp();
        m_body << "\t" << bD << " =d cast " << bBits << "\n";
        std::string cmp = newTemp();
        m_body << "\t" << cmp << " =w " << qbeCmp << " " << aD << ", " << bD
               << "\n";
        std::string cmpL = newTemp();
        m_body << "\t" << cmpL << " =l extuw " << cmp << "\n";
        std::string rBits = newTemp();
        m_body << "\t" << rBits << " =l add " << VAL_FALSE << ", " << cmpL
               << "\n";
        storel(rBits, aAddr);
        m_body << "\tjmp " << doneLabel << "\n";
        m_body << doneLabel << "\n";
    }

    void emitInstruction(const DecodedInstruction& ins) {
        const auto& [before, after] = stateOf(ins.offset);
        switch (ins.op) {
        case Op::CONSTANT: {
            Value v = m_fn.function->chunk.getConstant(
                static_cast<uint16_t>(ins.constantIndex));
            if (is<Number>(v)) {
                auto bits = std::bit_cast<uint64_t>(as<Number>(v));
                storel(std::to_string(bits), addr(before.height));
            } else {
                // Any other constant type (String, enum-ctor object, ...)
                // already exists as a real object, built by rt_startup's
                // embed-and-recompile step — read it back rather than
                // re-encode it (S5, #458; bytecode-translation-problems.md
                // P6, "materialise").
                std::string bits = constantValueBits(ins.constantIndex);
                storel(bits, addr(before.height));
            }
            break;
        }
        case Op::NIL:
            storel(std::to_string(VAL_NIL), addr(before.height));
            break;
        case Op::TRUE:
            storel(std::to_string(VAL_TRUE), addr(before.height));
            break;
        case Op::FALSE:
            storel(std::to_string(VAL_FALSE), addr(before.height));
            break;
        case Op::POP:
            break; // fused stack: the slot is simply abandoned (no locals/
                   // temporaries split to repair — see qbe_emitter.h).
        case Op::GET_LOCAL: {
            std::string v = loadl(addr(ins.byteOperand));
            storel(v, addr(before.height));
            break;
        }
        case Op::SET_LOCAL: {
            std::string v = loadl(addr(before.height - 1));
            storel(v, addr(ins.byteOperand));
            break;
        }
        case Op::DEFINE_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_define_global", before.height, {name},
                         std::nullopt); // rt_op_define_global has no
                                        // stopAtFrameCount parameter
            break;
        }
        case Op::GET_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_get_global", before.height, {name},
                         /*stopAtFrameCount=*/0);
            break;
        }
        case Op::SET_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_set_global", before.height, {name},
                         /*stopAtFrameCount=*/0);
            break;
        }
        case Op::PRINT:
            callSlowPath("rt_op_print", before.height, {},
                         std::nullopt); // rt_op_print has no
                                        // stopAtFrameCount parameter
            break;
        case Op::ADD:
            // The slow path (rt_op_add) also covers string concatenation —
            // the fast path here only ever fires once both operands already
            // passed isNumberCheck, so it can never misfire on two strings.
            emitBinaryArith("add", "rt_op_add", before.height, 0);
            break;
        case Op::SUBTRACT:
            emitBinaryArith("sub", "rt_op_subtract", before.height, 0);
            break;
        case Op::MULTIPLY:
            emitBinaryArith("mul", "rt_op_multiply", before.height, 0);
            break;
        case Op::DIVIDE:
            emitBinaryArith("div", "rt_op_divide", before.height, 0);
            break;
        case Op::MODULO:
            // No inline fast path: MODULO's floor-division number case is
            // not a plain double-double op (Q5) — always the slow path.
            callSlowPath("rt_op_modulo", before.height, {}, 0);
            break;
        case Op::NEGATE: {
            std::string aAddr = addr(before.height - 1);
            std::string aBits = loadl(aAddr);
            std::string aNum = isNumberCheck(aBits);
            std::string fastLabel =
                "@" + m_symbol + "_fast" + std::to_string(m_tempCounter);
            std::string slowLabel =
                "@" + m_symbol + "_slow" + std::to_string(m_tempCounter);
            std::string doneLabel =
                "@" + m_symbol + "_done" + std::to_string(m_tempCounter);
            m_body << "\tjnz " << aNum << ", " << fastLabel << ", " << slowLabel
                   << "\n";
            m_body << slowLabel << "\n";
            callSlowPath("rt_op_negate", before.height, {}, 0);
            m_body << "\tjmp " << doneLabel << "\n";
            m_body << fastLabel << "\n";
            std::string aD = newTemp();
            m_body << "\t" << aD << " =d cast " << aBits << "\n";
            std::string rD = newTemp();
            m_body << "\t" << rD << " =d neg " << aD << "\n";
            std::string rBits = newTemp();
            m_body << "\t" << rBits << " =l cast " << rD << "\n";
            storel(rBits, aAddr);
            m_body << "\tjmp " << doneLabel << "\n";
            m_body << doneLabel << "\n";
            break;
        }
        case Op::LESS:
            emitBinaryCompare("cltd", "rt_op_less", before.height, 0);
            break;
        case Op::GREATER:
            emitBinaryCompare("cgtd", "rt_op_greater", before.height, 0);
            break;
        case Op::EQUAL:
            emitBinaryCompare("ceqd", "rt_op_equal", before.height, 0);
            break;
        case Op::JUMP:
        case Op::LOOP:
        case Op::JUMP_IF_FALSE:
        case Op::JUMP_TABLE:
            break; // control transfer is emitted once, after the block's
                   // last instruction — see emitBlock's own terminator step.
                   // JUMP_TABLE's own tag value (pushed by the preceding
                   // GET_TAG) is read there, at its own `before.height`, the
                   // same way JUMP_IF_FALSE's own falsyCheck reads its
                   // condition — see emitJumpTableTerminator.
        case Op::CALL: {
            // rt_call dispatches on the callee's kind exactly as VM::run()'s
            // Op::CALL does (Runtime::opCall) — closure, native, bound
            // method, class, enum ctor. On success, opCall() has already
            // replaced [callee, args...] with the one result value directly
            // on this same physical stack (the fused-stack model means
            // there is nothing left for this emitter to load/store: the
            // next instruction's own `addr(after.height - 1)` already
            // points at it).
            callSlowPathTyped("rt_call", before.height,
                              {"w " + std::to_string(ins.byteOperand)});
            break;
        }
        case Op::CLOSURE: {
            Value fnConst = m_fn.function->chunk.getConstant(
                static_cast<uint16_t>(ins.constantIndex));
            if (!isFunction(fnConst)) {
                throw std::runtime_error(
                    "qbe_emitter: CLOSURE constant is not a function — "
                    "decoder/compiler drift");
            }
            // ownClosureBits() (never addr(0) — S5, #458: base[0] is the
            // receiver, not the closure, inside a method's own frame) is
            // the only way compiled code can reach its own function's
            // constant pool (rt_capi.h's own file comment: compiled code
            // has no constant pool of its own).
            std::string ownClosure = ownClosureBits();
            std::string fnConstBits = newTemp();
            m_body << "\t" << fnConstBits
                   << " =l call $rt_constant_at(l %rt, "
                      "l "
                   << ownClosure << ", w " << ins.constantIndex << ")\n";
            // Q1: set top to cover every existing local before the
            // allocating rt_new_closure call. addr() itself emits an
            // instruction as a side effect, so it must be computed as its
            // own statement — never inlined into an ongoing m_body <<
            // chain, which would interleave the two instructions' text.
            std::string preClosureTop = addr(before.height);
            m_body << "\tcall $rt_set_top(l %rt, l " << preClosureTop << ")\n";
            std::string newClosureBits = newTemp();
            m_body << "\t" << newClosureBits
                   << " =l call $rt_new_closure(l %rt, l " << fnConstBits
                   << ")\n";
            storel(newClosureBits, addr(before.height));
            // Root the new closure (Q1) before any upvalue-capturing call
            // below can itself allocate an ObjUpvalue — mirrors vm.cpp's
            // own CLOSURE case: push the closure, THEN capture upvalues.
            std::string postClosureTop = addr(before.height + 1);
            m_body << "\tcall $rt_set_top(l %rt, l " << postClosureTop << ")\n";
            for (std::size_t i = 0; i < ins.upvalues.size(); i++) {
                const ClosureUpvalue& uv = ins.upvalues[i];
                if (uv.isLocal) {
                    std::string localAddr = addr(uv.index);
                    m_body << "\tcall $rt_capture_local_upvalue(l %rt, l "
                           << newClosureBits << ", w " << i << ", l "
                           << localAddr << ")\n";
                } else {
                    m_body << "\tcall $rt_forward_upvalue(l %rt, l "
                           << newClosureBits << ", w " << i << ", l "
                           << ownClosure << ", w " << static_cast<int>(uv.index)
                           << ")\n";
                }
            }
            break;
        }
        case Op::GET_UPVALUE: {
            std::string ownClosure = ownClosureBits();
            std::string v = newTemp();
            m_body << "\t" << v << " =l call $rt_get_upvalue(l %rt, l "
                   << ownClosure << ", w " << ins.byteOperand << ")\n";
            storel(v, addr(before.height));
            break;
        }
        case Op::SET_UPVALUE: {
            // Peek family (P2): leaves `v` on the stack — SET_UPVALUE is an
            // assignment expression, same as SET_LOCAL/SET_GLOBAL.
            std::string ownClosure = ownClosureBits();
            std::string v = loadl(addr(before.height - 1));
            m_body << "\tcall $rt_set_upvalue(l %rt, l " << ownClosure << ", w "
                   << ins.byteOperand << ", l " << v << ")\n";
            break;
        }
        case Op::CLOSE_UPVALUE: {
            // Mirrors vm.cpp: closeUpvalues(stackTop - 1); pop(). The pop is
            // a no-op here, same as the plain POP case above — the fused
            // stack has no locals/temporaries split to repair. addr() must
            // be computed as its own statement (see CLOSURE's own comment
            // above on why it cannot be inlined into an m_body << chain).
            std::string closeAddr = addr(before.height - 1);
            m_body << "\tcall $rt_close_upvalues(l %rt, l " << closeAddr
                   << ")\n";
            break;
        }
        case Op::RETURN: {
            std::string topAddr = addr(before.height);
            m_body << "\tcall $rt_set_top(l %rt, l " << topAddr << ")\n";
            m_body << "\tret 0\n";
            break;
        }
        // --- S5 (#458): classes, methods, aggregates, iterators, slicing,
        // membership, and match dispatch. Every wrapper below already
        // mutates the real Runtime stack the same way push()/pop() do
        // (Runtime::op*(), runtime.cpp), so — exactly like CALL/GET_INDEX
        // above — nothing here needs to load/store a result itself: the
        // next instruction's own addr(after.height - 1) already points at
        // it once rt_set_top (Q1) and the call have run.
        case Op::CLASS: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_class", before.height, {name}, std::nullopt);
            break;
        }
        case Op::GET_PROPERTY: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_get_property", before.height, {name},
                         /*stopAtFrameCount=*/0);
            break;
        }
        case Op::SET_PROPERTY: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_set_property", before.height, {name},
                         std::nullopt); // fatal-only, no stopAtFrameCount
            break;
        }
        case Op::DEFINE_METHOD: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_define_method", before.height, {name},
                         std::nullopt);
            break;
        }
        case Op::INVOKE: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPathTyped(
                "rt_op_invoke", before.height,
                {"l " + name, "w " + std::to_string(ins.byteOperand), "w 0"});
            break;
        }
        case Op::INHERIT:
            callSlowPath("rt_op_inherit", before.height, {},
                         std::nullopt); // no stopAtFrameCount
            break;
        case Op::GET_SUPER: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_get_super", before.height, {name},
                         std::nullopt); // no stopAtFrameCount
            break;
        }
        case Op::SUPER_INVOKE: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPathTyped(
                "rt_op_super_invoke", before.height,
                {"l " + name, "w " + std::to_string(ins.byteOperand), "w 0"});
            break;
        }
        case Op::BUILD_LIST:
            callSlowPathTyped("rt_op_build_list", before.height,
                              {"w " + std::to_string(ins.byteOperand)});
            break;
        case Op::BUILD_MAP:
            callSlowPathTyped("rt_op_build_map", before.height,
                              {"w " + std::to_string(ins.byteOperand), "w 0"});
            break;
        case Op::GET_INDEX:
            callSlowPath("rt_op_get_index", before.height, {},
                         /*stopAtFrameCount=*/0);
            break;
        case Op::SET_INDEX:
            callSlowPath("rt_op_set_index", before.height, {},
                         /*stopAtFrameCount=*/0);
            break;
        case Op::SLICE:
            callSlowPath("rt_op_slice", before.height, {},
                         std::nullopt); // fatal-only, no stopAtFrameCount
            break;
        case Op::IN:
            callSlowPath("rt_op_in", before.height, {},
                         /*stopAtFrameCount=*/0);
            break;
        case Op::LEN:
            callSlowPath("rt_op_len", before.height, {},
                         /*stopAtFrameCount=*/0);
            break;
        case Op::GET_ITER:
            callSlowPath("rt_op_get_iter", before.height, {},
                         /*stopAtFrameCount=*/0);
            break;
        case Op::ITER_HAS_NEXT:
            callSlowPath("rt_op_iter_has_next", before.height, {},
                         std::nullopt); // no stopAtFrameCount
            break;
        case Op::ITER_NEXT:
            callSlowPath("rt_op_iter_next", before.height, {},
                         std::nullopt); // no stopAtFrameCount
            break;
        case Op::GET_TAG:
            callSlowPath("rt_op_get_tag", before.height, {},
                         std::nullopt); // fatal-only, no stopAtFrameCount
            break;
        case Op::MATCH_ERROR:
            // Always throws (there is no non-error stack effect for this
            // opcode — see vm.cpp). This node supports no PUSH_HANDLER
            // (qbe_emitter.h's file comment), so any raise here is Uncaught
            // by construction; callSlowPathTyped's own generic fatal-only
            // handling already `ret 1`s on that status. The unreachable
            // "ok" fallthrough it also emits (mirrored so this call site
            // matches every other wrapper's shape) still needs its own
            // terminator, since MATCH_ERROR ends its own cfg block with no
            // successor (S6, #459 owns real catchability here).
            callSlowPathTyped("rt_op_match_error", before.height, {"w 0"});
            m_body << "\tret 1\n";
            break;
        default:
            unsupported(ins.op);
        }
    }

    std::string nameConstant(int constantIndex) const {
        Value v = m_fn.function->chunk.getConstant(
            static_cast<uint16_t>(constantIndex));
        if (!isString(v)) {
            throw std::runtime_error(
                "qbe_emitter: global-variable name constant is not a "
                "string — decoder/compiler drift");
        }
        return std::string(asObjString(v)->chars.data(),
                           asObjString(v)->chars.size());
    }

    // JUMP_IF_FALSE's own condition register: isFalsy(v) == (v == VAL_FALSE
    // || v == VAL_NIL) (value.cpp's operator!). Computed once per block,
    // right before the terminator step, over the value already sitting at
    // the block's own final "before" height (JUMP_IF_FALSE peeks, it never
    // pops — abstract_stack.h's peeksInsteadOfPops).
    std::string falsyCheck(int height) {
        std::string bits = loadl(addr(height - 1));
        std::string isFalse = newTemp();
        m_body << "\t" << isFalse << " =w ceql " << bits << ", " << VAL_FALSE
               << "\n";
        std::string isNil = newTemp();
        m_body << "\t" << isNil << " =w ceql " << bits << ", " << VAL_NIL
               << "\n";
        std::string t = newTemp();
        m_body << "\t" << t << " =w or " << isFalse << ", " << isNil << "\n";
        return t;
    }

    void emitBlock(const BasicBlock& block, std::size_t blockIndex,
                   const Cfg& cfg) {
        m_body << "@" << block.label << "\n";
        if (!leaderReached(block)) {
            m_body << "\tret 0\n";
            return;
        }
        for (const DecodedInstruction& ins : block.instructions) {
            emitInstruction(ins);
        }
        emitTerminator(block, blockIndex, cfg);
    }

    void emitTerminator(const BasicBlock& block, std::size_t blockIndex,
                        const Cfg& cfg) {
        if (block.successors.empty()) {
            // RETURN's own emitInstruction() already emitted `ret 0`;
            // MATCH_ERROR's already emitted `ret 1` (it always throws, and
            // this node supports no PUSH_HANDLER — see its own
            // emitInstruction case). Anything else reaching here is a
            // decoder/cfg-analysis mismatch this node cannot recover from
            // (THROW is out of scope — S6, #459).
            Op lastOp = block.instructions.empty()
                            ? Op{}
                            : block.instructions.back().op;
            if (lastOp != Op::RETURN && lastOp != Op::MATCH_ERROR) {
                throw std::runtime_error(
                    "qbe_emitter: terminal block with no cfg successor and "
                    "no RETURN/MATCH_ERROR — an out-of-scope terminal "
                    "opcode (THROW — see S6, #459)");
            }
            return;
        }
        if (block.instructions.back().op == Op::JUMP_TABLE) {
            emitJumpTableTerminator(block, cfg);
            return;
        }
        if (block.successors.size() == 1) {
            m_body << "\tjmp @"
                   << cfg.blocks[block.successors[0].targetBlock].label << "\n";
            return;
        }
        if (block.successors.size() == 2 &&
            block.instructions.back().op == Op::JUMP_IF_FALSE) {
            const auto& [before, after] =
                stateOf(block.instructions.back().offset);
            (void)after;
            std::string falsy = falsyCheck(before.height);
            std::string branchLabel;
            std::string fallLabel;
            for (const CfgEdge& e : block.successors) {
                const std::string& label = cfg.blocks[e.targetBlock].label;
                if (e.kind == EdgeKind::FORWARD_BRANCH) {
                    branchLabel = label;
                } else {
                    fallLabel = label;
                }
            }
            m_body << "\tjnz " << falsy << ", @" << branchLabel << ", @"
                   << fallLabel << "\n";
            return;
        }
        (void)blockIndex;
        throw std::runtime_error(
            "qbe_emitter: block with an unsupported branch shape");
    }

    // P8 (bytecode-translation-problems.md, notes/qbe-backend.md hazard
    // Q8/P8): QBE's only terminators are jmp/jnz/ret — no switch, no
    // indirect jump. Lowers JUMP_TABLE to a compare chain: one `ceqw` +
    // `jnz` per arm, in `ins.jumpTable` order, falling through to the next
    // comparison on a miss and finally to the fallthrough/default block
    // (cfg.cpp's own wireSuccessors: one FORWARD_BRANCH edge per arm, in
    // arm order, then one optional trailing FALL_THROUGH edge — that
    // ordering is exactly `block.successors`' own layout, checked below
    // rather than assumed).
    //
    // The tag was pushed by the preceding GET_TAG as a Number Value (a
    // plain double bit pattern, never boxed — Q5/Q6); JUMP_TABLE pops it
    // (abstract_stack.cpp: "no push") and compares as a word integer, so
    // this reads it here at the instruction's own `before.height`, exactly
    // as JUMP_IF_FALSE's own falsyCheck reads its condition.
    void emitJumpTableTerminator(const BasicBlock& block, const Cfg& cfg) {
        const DecodedInstruction& jt = block.instructions.back();
        const auto& [before, after] = stateOf(jt.offset);
        (void)after;
        if (block.successors.size() != jt.jumpTable.size() &&
            block.successors.size() != jt.jumpTable.size() + 1) {
            throw std::runtime_error(
                "qbe_emitter: JUMP_TABLE successor count does not match its "
                "own arm count plus an optional fallthrough — cfg/decoder "
                "drift");
        }
        std::string tagBits = loadl(addr(before.height - 1));
        std::string tagD = newTemp();
        m_body << "\t" << tagD << " =d cast " << tagBits << "\n";
        std::string tagW = newTemp();
        m_body << "\t" << tagW << " =w dtosi " << tagD << "\n";

        std::optional<std::string> fallthroughLabel;
        if (block.successors.size() == jt.jumpTable.size() + 1) {
            fallthroughLabel =
                cfg.blocks[block.successors.back().targetBlock].label;
        }

        for (std::size_t i = 0; i < jt.jumpTable.size(); i++) {
            const JumpTableArm& arm = jt.jumpTable[i];
            const std::string& armLabel =
                cfg.blocks[block.successors[i].targetBlock].label;
            std::string hit = newTemp();
            m_body << "\t" << hit << " =w ceqw " << tagW << ", " << arm.tag
                   << "\n";
            std::string missLabel =
                "@" + m_symbol + "_jt" + std::to_string(m_tempCounter);
            m_body << "\tjnz " << hit << ", @" << armLabel << ", " << missLabel
                   << "\n";
            m_body << missLabel << "\n";
        }
        if (fallthroughLabel.has_value()) {
            m_body << "\tjmp @" << *fallthroughLabel << "\n";
        } else {
            // No arm matched and no fallthrough exists (the table covers
            // every reachable tag) — unreachable in practice, but QBE still
            // requires an explicit terminator for this block.
            m_body << "\tret 1\n";
        }
    }
};

} // namespace

std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const std::string& qbeSymbol) {
    Emitter emitter(fn, analysis, qbeSymbol);
    return emitter.run();
}

} // namespace qbe
