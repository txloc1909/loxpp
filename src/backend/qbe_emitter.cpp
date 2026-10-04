#include "qbe_emitter.h"

#include "cfg.h"
#include "exec_objects.h" // ObjFunction (chunk/arity), for CONSTANT/global-name lookups
#include "handler_depth.h" // activeHandler (S6, #459)
#include "native_pops.h"   // opName(Op), for error messages
#include "object.h"
#include "qbe_promotion.h" // register promotion + safe points (S8, #461)
#include "rt_abi.h"        // kRtOk/kRtThrow/kRtFatal (S6, #459)
#include "value.h"

#include <algorithm>
#include <bit>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace qbe {

// Every bit pattern this file bakes into a .ssa literal (QNAN, VAL_NIL,
// VAL_TRUE, VAL_FALSE, OBJ_TAG) is the NaN-boxed Value layout.
static_assert(sizeof(Value) == 8, "qbe_emitter requires the 8-byte NaN-boxed "
                                  "Value (notes/qbe-backend.md)");

namespace {

// Bit-pattern constants this file needs from value.h's NaN-boxing (detail
// namespace — reused, not duplicated, so a layout change there cannot drift
// silently from what this emitter bakes into .ssa literals).
using detail::OBJ_TAG;
using detail::QNAN;
using detail::VAL_FALSE;
using detail::VAL_NIL;
using detail::VAL_TRUE;

// Wire values of Runtime::OpResult (runtime.h) — a rt_op_*/rt_call wrapper's
// own raw status, distinct from the compiled function's own kRtOk/kRtThrow/
// kRtFatal (rt_abi.h). This file emits only .ssa text, never links against
// Runtime itself, so these are plain literals rather than an include — kept
// in one place, next to their only two uses, rather than repeated inline.
constexpr int kOpResultResumed = 1;
constexpr int kOpResultFatal = 3;

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
            const FunctionCaptureInfo& captures, const EmitOptions& options,
            std::string qbeSymbol)
        : m_fn(fn), m_analysis(analysis), m_captures(captures),
          m_options(options), m_symbol(std::move(qbeSymbol)),
          m_handlerDepth(analyzeHandlerDepth(fn)) {
        for (std::size_t i = 0; i < fn.instructions.size(); i++) {
            m_stateAt.emplace(fn.instructions[i].offset,
                              std::pair{analysis.before[i], analysis.after[i]});
            m_reachedAt.emplace(fn.instructions[i].offset, analysis.reached[i]);
            m_activeHandlerAt.emplace(fn.instructions[i].offset,
                                      m_handlerDepth.activeHandler[i]);
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
        if (cfg.blocks.empty()) {
            throw std::runtime_error(
                "qbe_emitter: function with no basic blocks — "
                "decoder/compiler drift");
        }
        // S6 (#459): maps a PUSH_HANDLER's own bytecode offset to its catch
        // block's cfg label — m_activeHandlerAt (constructor, above) names
        // the PUSH_HANDLER instruction active at a given offset by ITS OWN
        // offset (via m_fn.instructions' own layout, handler_depth.h); this
        // resolves that to the compiled label a local catch must jump to.
        for (const HandlerEntry& he : cfg.handlerEntries) {
            m_pushOffsetToCatchLabel[he.pushHandlerOffset] =
                cfg.blocks[static_cast<std::size_t>(he.catchBlock)].label;
        }
        if (m_options.promoteRegisters) {
            m_plan = planPromotion(m_fn, m_analysis, m_captures, cfg);
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
    const FunctionStackAnalysis& m_analysis;
    const FunctionCaptureInfo& m_captures;
    EmitOptions m_options;
    std::string m_symbol;
    HandlerDepthAnalysis m_handlerDepth;
    // S8 (#461): the promotion plan (null when register promotion is off) and
    // the SSA value currently held in a register for each promoted slot, in
    // the block being emitted. A slot absent from m_cur falls back to its
    // stack slot.
    std::optional<PromotionPlan> m_plan;
    std::map<int, std::string> m_cur;
    std::unordered_map<int, std::pair<StackState, StackState>> m_stateAt;
    std::unordered_map<int, bool> m_reachedAt;
    // Offset -> index (into m_fn.instructions) of the innermost active
    // PUSH_HANDLER at that offset, -1 when none (S6, #459).
    std::unordered_map<int, int> m_activeHandlerAt;
    // A PUSH_HANDLER's own offset -> its catch block's cfg label, built once
    // cfg is available (run(), above).
    std::unordered_map<int, std::string> m_pushOffsetToCatchLabel;
    // S8 (#461): a JUMP_TABLE offset -> the word temp holding its own
    // GET_TAG's tag, when the two were fused (emitBlock). Absent for a
    // JUMP_TABLE whose tag came through the unfused rt_op_get_tag path.
    std::unordered_map<int, std::string> m_fusedTagWordByJumpOffset;
    std::ostringstream m_data;
    std::ostringstream m_body;
    int m_tempCounter{0};
    int m_dataCounter{0};
    int m_maxHeight{0};
    // This function's own frame depth minus one, computed once in the
    // prologue (emitStackCheck) — every fallible call this function makes
    // passes this as its own stopAtFrameCount, so OpResult::Resumed from
    // that call means exactly "resolved at MY OWN frame" (rt_abi.h's own
    // comment on RtCompiledFn explains why). Empty until emitStackCheck
    // runs; every other emission happens after it (run()'s own order).
    std::string m_stopTemp;

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

    // S8 (#461): is `slot` one the promotion plan may keep in a register?
    bool isPromoted(int slot) const {
        return m_plan.has_value() && slot > 0 &&
               slot < static_cast<int>(m_plan->candidate.size()) &&
               m_plan->candidate[static_cast<std::size_t>(slot)];
    }

    // A load whose destination is a plan-chosen SSA name rather than a fresh
    // `%t` temp, so the name the plan reports is the one this code defines.
    void loadlInto(const std::string& dest, const std::string& address) {
        m_body << "\t" << dest << " =l loadl " << address << "\n";
    }

    // Q1 (notes/qbe-backend.md): write every promoted slot currently in a
    // register back to its stack cell before an allocating or unwinding
    // call. The GC scans stack..stackTop only, so a value held just in a QBE
    // SSA temp would otherwise be invisible at the safe point. m_cur holds
    // only in-scope slots, so each store lands in that slot's own cell. It
    // is ordered by slot so the emitted store order (and the .ssa) does not
    // depend on a hash.
    void spillPromotedLocals() {
        for (const auto& [slot, value] : m_cur) {
            std::string a = addr(slot);
            storel(value, a);
        }
    }

    // Emits this block's promotion entry (S8, #461): the SSA phis at a merge
    // (QBE requires phis first, before any other instruction), then the
    // catch-entry reloads and entry-block parameter loads, then seeds m_cur
    // from the plan. A block the plan carries no entry for starts empty and
    // falls back to the stack slots.
    void emitPromotionEntry(std::size_t blockIndex, const Cfg& cfg) {
        m_cur.clear();
        if (!m_plan.has_value()) {
            return;
        }
        for (const PromoPhi& phi : m_plan->phis[blockIndex]) {
            m_body << "\t" << phi.name << " =l phi";
            for (std::size_t i = 0; i < phi.args.size(); i++) {
                const auto& [pred, value] = phi.args[i];
                if (i > 0) {
                    m_body << ",";
                }
                // The predecessor's own branch is emitted in its `_tail`
                // block (emitTerminator), which is the QBE block a phi must
                // name — a cfg label alone can end mid-block. The emitter's
                // prologue is block 0's other predecessor (kProloguePred).
                std::string label =
                    pred == kProloguePred
                        ? m_symbol + "_entry_ok"
                        : cfg.blocks[static_cast<std::size_t>(pred)].label +
                              "_tail";
                m_body << " @" << label << " " << value;
            }
            m_body << "\n";
        }
        for (const auto& [slot, name] : m_plan->catchReload[blockIndex]) {
            std::string a = addr(slot);
            loadlInto(name, a);
        }
        for (const auto& [slot, name] : m_plan->entryValue[blockIndex]) {
            m_cur[slot] = name;
        }
    }

    // Applies the plan's own non-SET_LOCAL promotion events at `offset`: a
    // declaring push (invisible var) defines the slot's register value right
    // after its stack store; a POP that reclaims the slot drops it. Mirrors
    // qbe_promotion.cpp's advance() exactly.
    void applyPromotionEvents(int offset) {
        if (!m_plan.has_value()) {
            return;
        }
        auto d = m_plan->declareAt.find(offset);
        if (d != m_plan->declareAt.end()) {
            int slot = d->second.first;
            std::string a = addr(slot);
            loadlInto(d->second.second, a);
            m_cur[slot] = d->second.second;
        }
        auto r = m_plan->reclaimAt.find(offset);
        if (r != m_plan->reclaimAt.end()) {
            m_cur.erase(r->second);
        }
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
        // rt_new_string allocates (makeString), so this is a safe point: a
        // promoted local live across it must be visible to the GC.
        spillPromotedLocals();
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

    // How a runtime call's nonzero status is handled (S6, #459, Q2):
    //   Fatal:     the wrapper's own C signature has no stopAtFrameCount
    //              parameter (rt_capi.h) — its OWN implementation only ever
    //              calls runtimeError() directly, never raiseThrowableError
    //              (verified against runtime.cpp for every such wrapper),
    //              so any nonzero status is unconditionally fatal. Simple
    //              2-way: 0 continues, nonzero propagates kRtFatal.
    //   Local:     the wrapper takes stopAtFrameCount, and this call site
    //              sits inside a try whose catch block IS a legal resume
    //              target here (an ordinary fallible op). 3-way: 0
    //              continues; kRtFatal(2) propagates as kRtFatal;
    //              OpResult::Resumed(1) — which, since this call was passed
    //              m_stopTemp (this function's own frame depth minus one —
    //              see m_stopTemp's own comment), means exactly "resolved
    //              at MY OWN frame" — jumps to the statically active catch
    //              block (m_activeHandlerAt); OpResult::Stop(2) propagates
    //              as kRtThrow.
    //   Propagate: like Local, but NEVER attempts a local jump even on
    //              Resumed — RUN_DEFERS' own case (its own comment):
    //              this frame's handlers are already closed by the time it
    //              runs, so any handler that resolves a deferred call's own
    //              throw belongs to an ancestor, never to this frame.
    enum class Catchability : std::uint8_t { Fatal, Local, Propagate };

    // Runs a runtime call whose arguments (beyond `l %rt`) are already
    // typed (each entry is a full "l %x" / "w 5" token). `offset` is this
    // instruction's own static bytecode offset — stored into the frame
    // before the call (Q4: rt_set_frame_offset) so a stack trace built from
    // a fault inside this call reports the real line, not this function's
    // first one.
    void callSlowPathTyped(const std::string& fnName, int topHeight,
                           const std::vector<std::string>& typedArgs,
                           int offset, Catchability catchability) {
        // Every op*()/rt_* call can allocate or unwind, so spill promoted
        // locals first (S8, #461).
        spillPromotedLocals();
        std::string topAddr = addr(topHeight);
        m_body << "\tcall $rt_set_top(l %rt, l " << topAddr << ")\n";
        m_body << "\tcall $rt_set_frame_offset(l %rt, w " << offset << ")\n";
        std::string status = newTemp();
        m_body << "\t" << status << " =w call $" << fnName << "(l %rt";
        for (const std::string& a : typedArgs) {
            m_body << ", " << a;
        }
        m_body << ")\n";
        std::string okLabel =
            "@" + m_symbol + "_ok" + std::to_string(m_tempCounter);
        std::string badLabel =
            "@" + m_symbol + "_bad" + std::to_string(m_tempCounter);
        std::string bad = newTemp();
        m_body << "\t" << bad << " =w cnew " << status << ", 0\n";
        m_body << "\tjnz " << bad << ", " << badLabel << ", " << okLabel
               << "\n";
        m_body << badLabel << "\n";
        if (catchability == Catchability::Fatal) {
            m_body << "\tret " << kRtFatal << "\n";
            m_body << okLabel << "\n";
            return;
        }
        std::string isFatal = newTemp();
        m_body << "\t" << isFatal << " =w ceqw " << status << ", "
               << kOpResultFatal << "\n";
        std::string fatalLabel =
            "@" + m_symbol + "_fatal" + std::to_string(m_tempCounter);
        std::string resolvedLabel =
            "@" + m_symbol + "_resolved" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << isFatal << ", " << fatalLabel << ", "
               << resolvedLabel << "\n";
        m_body << fatalLabel << "\n\tret " << kRtFatal << "\n";
        m_body << resolvedLabel << "\n";
        // OpResult::Resumed(1) here means the throw resolved at exactly
        // this function's own frame (m_stopTemp is this frame's own depth
        // minus one — see its own comment); OpResult::Stop(2) means it
        // resolved somewhere else. Only Local ever attempts the jump, and
        // only when handler_depth found a statically active handler here —
        // Resumed with no active handler, or under Propagate, is treated
        // exactly like Stop: propagate kRtThrow.
        std::optional<std::string> localCatchLabel =
            catchability == Catchability::Local ? activeCatchLabelAt(offset)
                                                : std::nullopt;
        if (!localCatchLabel.has_value()) {
            m_body << "\tret " << kRtThrow << "\n";
            m_body << okLabel << "\n";
            return;
        }
        std::string isResumed = newTemp();
        m_body << "\t" << isResumed << " =w ceqw " << status << ", "
               << kOpResultResumed << "\n";
        std::string localLabel =
            "@" + m_symbol + "_local" + std::to_string(m_tempCounter);
        std::string propagateLabel =
            "@" + m_symbol + "_propagate" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << isResumed << ", " << localLabel << ", "
               << propagateLabel << "\n";
        m_body << propagateLabel << "\n\tret " << kRtThrow << "\n";
        m_body << localLabel << "\n\tjmp @" << *localCatchLabel << "\n";
        m_body << okLabel << "\n";
    }

    // The compiled catch label a fallible op at `offset` must jump to when
    // its own runtime call reports the fault resolved at THIS function's
    // own frame — the statically active PUSH_HANDLER at `offset`
    // (m_activeHandlerAt, handler_depth.h), resolved to its own catch
    // block's label (m_pushOffsetToCatchLabel, built in run()). nullopt
    // when no handler is statically active at `offset`.
    std::optional<std::string> activeCatchLabelAt(int offset) const {
        auto it = m_activeHandlerAt.find(offset);
        if (it == m_activeHandlerAt.end() || it->second < 0) {
            return std::nullopt;
        }
        int pushOffset =
            m_fn.instructions[static_cast<std::size_t>(it->second)].offset;
        auto labelIt = m_pushOffsetToCatchLabel.find(pushOffset);
        if (labelIt == m_pushOffsetToCatchLabel.end()) {
            return std::nullopt;
        }
        return labelIt->second;
    }

    // `topHeight` is set via rt_set_top before the call (Q1: every value
    // must already live in its own stack slot before a call that can
    // allocate or unwind). `stopAtFrameCount` is nullopt for the wrappers
    // whose C signature has no such trailing parameter (rt_capi.h): QBE's
    // `call` performs no prototype check, so passing an extra argument a
    // callee's own body never reads would go unnoticed here but is still
    // undefined behavior against the real C signature — this must match
    // rt_capi.h exactly, not "whatever happens to work" on one ABI.
    void callSlowPath(const std::string& fnName, int topHeight,
                      const std::vector<std::string>& extraArgs,
                      std::optional<std::string> stopAtFrameCount, int offset,
                      Catchability catchability) {
        std::vector<std::string> typedArgs;
        typedArgs.reserve(extraArgs.size() + 1);
        for (const std::string& a : extraArgs) {
            typedArgs.push_back("l " + a);
        }
        if (stopAtFrameCount.has_value()) {
            typedArgs.push_back("w " + *stopAtFrameCount);
        }
        callSlowPathTyped(fnName, topHeight, typedArgs, offset, catchability);
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
        // Stack overflow at function entry: no ambient handler context of
        // this function's own to check (its own PUSH_HANDLERs have not run
        // yet) — always fatal here, matching rt_check_stack's own "w 0"
        // stopAtFrameCount above (Q3's own hard-ceiling/reserve split
        // already decides catchable vs. fatal inside Runtime; a nonzero
        // status this early can only be the fatal one).
        m_body << failLabel << "\n\tret " << kRtFatal << "\n";
        m_body << okLabel << "\n";
        // S6 (#459): this function's own frame depth minus one — every
        // fallible call this function makes below passes this same value
        // as its own stopAtFrameCount, so OpResult::Resumed from that call
        // means exactly "resolved at my own frame" (see m_stopTemp's own
        // comment and callSlowPathTyped's Catchability doc).
        std::string myDepth = newTemp();
        m_body << "\t" << myDepth << " =w call $rt_frame_count(l %rt)\n";
        m_stopTemp = newTemp();
        m_body << "\t" << m_stopTemp << " =w sub " << myDepth << ", 1\n";
        // S8 (#461): promoted parameters are loaded here, in the block that
        // jumps to block 0, so a phi at block 0 (a loop that jumps back to
        // it) can name them as the prologue predecessor's values.
        if (m_plan.has_value()) {
            for (int s = 1; s <= m_fn.function->arity; s++) {
                if (isPromoted(s)) {
                    std::string a = addr(s);
                    loadlInto(paramValueName(s), a);
                }
            }
        }
        m_body << "\tjmp @" << firstBlockLabel << "\n";
    }

    // The SSA value name the promotion plan gives parameter `slot` (matching
    // qbe_promotion.cpp's paramName).
    static std::string paramValueName(int slot) {
        return "%qp" + std::to_string(slot);
    }

    // Binary numeric op with an inline plain-double fast path (Q5): `a OP
    // b`, operands at heights (in-2, in-1), result at height in-2 (out =
    // in-1). `fastOp` is the QBE double instruction (`add`/`sub`/`mul`/
    // `div`); `slowFn` is the rt_capi wrapper for the whole opcode
    // (including its non-number cases: string concatenation for ADD,
    // operator-overload dispatch for the rest).
    void emitBinaryArith(const std::string& fastOp, const std::string& slowFn,
                         int heightIn, int offset) {
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
        callSlowPath(slowFn, heightIn, {}, m_stopTemp, offset,
                     Catchability::Local);
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
                           int heightIn, int offset) {
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
        callSlowPath(slowFn, heightIn, {}, m_stopTemp, offset,
                     Catchability::Local);
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
            // S8 (#461): a promoted slot already holds its value in m_cur —
            // skip the load. The result still goes to its temporary cell, so
            // every consumer that reads memory keeps working.
            std::string v;
            auto cur = m_cur.find(ins.byteOperand);
            if (isPromoted(ins.byteOperand) && cur != m_cur.end()) {
                v = cur->second;
            } else {
                v = loadl(addr(ins.byteOperand));
            }
            storel(v, addr(before.height));
            break;
        }
        case Op::SET_LOCAL: {
            // S8 (#461): the target slot's register value becomes the operand
            // at height - 1 (always in memory, since only local slots are
            // promoted). No store to the slot here; a later safe point spills
            // it, and the plan's own dataflow carries it across blocks.
            if (m_plan.has_value()) {
                auto it = m_plan->setLocalAt.find(ins.offset);
                if (it != m_plan->setLocalAt.end()) {
                    std::string a = addr(before.height - 1);
                    loadlInto(it->second.second, a);
                    m_cur[it->second.first] = it->second.second;
                    break;
                }
            }
            std::string v = loadl(addr(before.height - 1));
            storel(v, addr(ins.byteOperand));
            break;
        }
        case Op::DEFINE_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_define_global", before.height, {name},
                         std::nullopt, ins.offset, Catchability::Fatal);
            break;
        }
        case Op::GET_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_get_global", before.height, {name}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        }
        case Op::SET_GLOBAL: {
            std::string name = internedNamePtr(nameConstant(ins.constantIndex));
            callSlowPath("rt_op_set_global", before.height, {name}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        }
        case Op::PRINT:
            // print routes through opStr, which dispatches __str__ and can
            // raise a catchable error, so this needs the enclosing frame's
            // own stop depth and a local catch (like LEN below).
            callSlowPath("rt_op_print", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        case Op::ADD:
            // The slow path (rt_op_add) also covers string concatenation —
            // the fast path here only ever fires once both operands already
            // passed isNumberCheck, so it can never misfire on two strings.
            emitBinaryArith("add", "rt_op_add", before.height, ins.offset);
            break;
        case Op::SUBTRACT:
            emitBinaryArith("sub", "rt_op_subtract", before.height, ins.offset);
            break;
        case Op::MULTIPLY:
            emitBinaryArith("mul", "rt_op_multiply", before.height, ins.offset);
            break;
        case Op::DIVIDE:
            emitBinaryArith("div", "rt_op_divide", before.height, ins.offset);
            break;
        case Op::MODULO:
            // No inline fast path: MODULO's floor-division number case is
            // not a plain double-double op (Q5) — always the slow path.
            callSlowPath("rt_op_modulo", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
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
            callSlowPath("rt_op_negate", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
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
            emitBinaryCompare("cltd", "rt_op_less", before.height, ins.offset);
            break;
        case Op::GREATER:
            emitBinaryCompare("cgtd", "rt_op_greater", before.height,
                              ins.offset);
            break;
        case Op::EQUAL:
            emitBinaryCompare("ceqd", "rt_op_equal", before.height, ins.offset);
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
            callSlowPathTyped(
                "rt_call", before.height,
                {"w " + std::to_string(ins.byteOperand), "w " + m_stopTemp},
                ins.offset, Catchability::Local);
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
            // S8 (#461): rt_new_closure and rt_capture_local_upvalue both
            // allocate, so a promoted local live across this instruction
            // must be in its stack slot for the GC.
            spillPromotedLocals();
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
            m_body << "\tret " << kRtOk << "\n";
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
            callSlowPath("rt_op_class", before.height, {name}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        }
        case Op::GET_PROPERTY: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_get_property", before.height, {name},
                         m_stopTemp, ins.offset, Catchability::Local);
            break;
        }
        case Op::SET_PROPERTY: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_set_property", before.height, {name},
                         std::nullopt, ins.offset, Catchability::Fatal);
            break;
        }
        case Op::DEFINE_METHOD: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_define_method", before.height, {name},
                         std::nullopt, ins.offset, Catchability::Fatal);
            break;
        }
        case Op::INVOKE: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPathTyped("rt_op_invoke", before.height,
                              {"l " + name,
                               "w " + std::to_string(ins.byteOperand),
                               "w " + m_stopTemp},
                              ins.offset, Catchability::Local);
            break;
        }
        case Op::INHERIT:
            callSlowPath("rt_op_inherit", before.height, {}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::GET_SUPER: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_get_super", before.height, {name}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        }
        case Op::SUPER_INVOKE: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPathTyped("rt_op_super_invoke", before.height,
                              {"l " + name,
                               "w " + std::to_string(ins.byteOperand),
                               "w " + m_stopTemp},
                              ins.offset, Catchability::Local);
            break;
        }
        case Op::BUILD_LIST:
            callSlowPathTyped("rt_op_build_list", before.height,
                              {"w " + std::to_string(ins.byteOperand)},
                              ins.offset, Catchability::Fatal);
            break;
        case Op::BUILD_MAP:
            callSlowPathTyped(
                "rt_op_build_map", before.height,
                {"w " + std::to_string(ins.byteOperand), "w " + m_stopTemp},
                ins.offset, Catchability::Local);
            break;
        case Op::GET_INDEX:
            callSlowPath("rt_op_get_index", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        case Op::SET_INDEX:
            callSlowPath("rt_op_set_index", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        case Op::SLICE:
            // Like GET_INDEX/SET_INDEX: opSlice dispatches __slice__, so the
            // wrapper can resolve a catchable throw at this frame and must
            // receive this function's own stop depth.
            callSlowPath("rt_op_slice", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        case Op::IN:
            callSlowPath("rt_op_in", before.height, {}, m_stopTemp, ins.offset,
                         Catchability::Local);
            break;
        case Op::LEN:
            callSlowPath("rt_op_len", before.height, {}, m_stopTemp, ins.offset,
                         Catchability::Local);
            break;
        case Op::STR:
            callSlowPath("rt_op_str", before.height, {}, m_stopTemp, ins.offset,
                         Catchability::Local);
            break;
        case Op::GET_ITER:
            callSlowPath("rt_op_get_iter", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Local);
            break;
        case Op::ITER_HAS_NEXT:
            callSlowPath("rt_op_iter_has_next", before.height, {}, m_stopTemp,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::ITER_NEXT:
            callSlowPath("rt_op_iter_next", before.height, {}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::GET_TAG:
            callSlowPath("rt_op_get_tag", before.height, {}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::NOT:
            callSlowPath("rt_op_not", before.height, {}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::IS_SEQ:
            callSlowPath("rt_op_is_seq", before.height, {}, std::nullopt,
                         ins.offset, Catchability::Fatal);
            break;
        case Op::INSTANCEOF: {
            std::string name = constantStringPtr(ins.constantIndex);
            callSlowPath("rt_op_instanceof", before.height, {name},
                         std::nullopt, ins.offset, Catchability::Fatal);
            break;
        }
        case Op::MATCH_ERROR:
            // Always throws (there is no non-error stack effect for this
            // opcode — see vm.cpp) — genuinely catchable (spec's MatchError
            // kind; check_fault_table.py's own match_error row). The
            // unreachable "ok" fallthrough callSlowPathTyped still emits
            // (mirrored so this call site matches every other wrapper's
            // shape) still needs its own terminator, since MATCH_ERROR ends
            // its own cfg block with no successor.
            callSlowPathTyped("rt_op_match_error", before.height,
                              {"w " + m_stopTemp}, ins.offset,
                              Catchability::Local);
            m_body << "\tret " << kRtFatal << "\n";
            break;
        // --- S6 (#459): status protocol, try/catch, defer.
        case Op::PUSH_HANDLER: {
            // No stack effect (chunk.h) — before.height == after.height.
            // rt_push_handler cannot meaningfully fail (a bare vector
            // push_back), so this skips the generic callSlowPath* status
            // machinery entirely.
            std::string checkpointAddr = addr(before.height);
            m_body << "\tcall $rt_push_handler(l %rt, l " << checkpointAddr
                   << ", w -1)\n";
            break;
        }
        case Op::POP_HANDLER:
            callSlowPathTyped("rt_pop_handler", before.height, {}, ins.offset,
                              Catchability::Fatal);
            break;
        case Op::THROW: {
            // Pops the value to raise (chunk.h) at before.height - 1.
            std::string thrownAddr = addr(before.height - 1);
            std::string thrownBits = loadl(thrownAddr);
            callSlowPathTyped("rt_throw", before.height - 1,
                              {"l " + thrownBits, "w " + m_stopTemp},
                              ins.offset, Catchability::Local);
            // rt_throw never returns success (Op::THROW is terminal —
            // chunk.h: "control never falls through past THROW"); this is
            // the same unreachable safety net MATCH_ERROR's own case uses.
            m_body << "\tret " << kRtFatal << "\n";
            break;
        }
        case Op::DEFER_RECORD:
            callSlowPathTyped("rt_op_defer_record", before.height,
                              {"w " + std::to_string(ins.byteOperand)},
                              ins.offset, Catchability::Fatal);
            break;
        case Op::RUN_DEFERS:
            // Never a local-catch opportunity: the compiler emits this only
            // immediately before RETURN, after this frame's own try/catch
            // regions have already lexically closed — any throw a deferred
            // call raises here can only be caught by an ancestor (see
            // rt_capi.h's own comment on rt_run_defers).
            callSlowPathTyped("rt_run_defers", before.height,
                              {"w " + m_stopTemp}, ins.offset,
                              Catchability::Propagate);
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
            m_body << "\tret " << kRtOk << "\n";
            return;
        }
        // S8 (#461): phis first (a QBE requirement), then the block's
        // promotion entry. Must run before any instruction or spill.
        emitPromotionEntry(blockIndex, cfg);
        // S8 (#461), P8: a dense enum match compiles to GET_TAG immediately
        // followed by JUMP_TABLE. When they are adjacent and end the block
        // (JUMP_TABLE is a terminator, so it is this block's last
        // instruction), fuse them: read the tag as a word directly instead
        // of materialising a boxed Number and converting it straight back.
        bool fusedTag = m_options.fuseTagJumpTable &&
                        block.instructions.size() >= 2 &&
                        block.instructions[block.instructions.size() - 2].op ==
                            Op::GET_TAG &&
                        block.instructions.back().op == Op::JUMP_TABLE;
        std::size_t fusedIndex =
            block.instructions.size() >= 2 ? block.instructions.size() - 2 : 0;
        for (std::size_t i = 0; i < block.instructions.size(); i++) {
            if (fusedTag && i == fusedIndex) {
                emitFusedGetTagWord(block.instructions[i],
                                    block.instructions.back().offset);
                applyPromotionEvents(block.instructions[i].offset);
                continue;
            }
            emitInstruction(block.instructions[i]);
            applyPromotionEvents(block.instructions[i].offset);
        }
        emitTerminator(block, blockIndex, cfg);
    }

    // The fused GET_TAG half of a GET_TAG;JUMP_TABLE pair (S8, #461).
    // `ins` is the GET_TAG; `jumpTableOffset` is the JUMP_TABLE that
    // consumes its tag, so the terminator can find this word temp. The enum
    // Value sits at the GET_TAG's own `before.height - 1` (GET_TAG has net
    // zero stack effect) and is left untouched — this reads it in place, no
    // rt_set_top and no safe point (the helper never allocates).
    void emitFusedGetTagWord(const DecodedInstruction& ins,
                             int jumpTableOffset) {
        const auto& [before, after] = stateOf(ins.offset);
        (void)after;
        std::string topAddr = addr(before.height);
        std::string slotAddr = addr(before.height - 1);
        // Keep the frame offset current (Q4) so a non-enum subject's fatal
        // stack trace names the match's own line, exactly as the unfused
        // rt_op_get_tag call site does.
        m_body << "\tcall $rt_set_top(l %rt, l " << topAddr << ")\n";
        m_body << "\tcall $rt_set_frame_offset(l %rt, w " << ins.offset
               << ")\n";
        std::string tag = newTemp();
        m_body << "\t" << tag << " =w call $rt_get_tag_word(l %rt, l "
               << slotAddr << ")\n";
        // -1 is the helper's non-enum sentinel (rt_capi.h); every real tag
        // is a non-negative ObjEnumCtor::tag, so a signed less-than test
        // catches it and mirrors opGetTag's own fatal error exactly.
        std::string bad = newTemp();
        m_body << "\t" << bad << " =w csltw " << tag << ", 0\n";
        std::string fatalLabel =
            "@" + m_symbol + "_tagfatal" + std::to_string(m_tempCounter);
        std::string okLabel =
            "@" + m_symbol + "_tagok" + std::to_string(m_tempCounter);
        m_body << "\tjnz " << bad << ", " << fatalLabel << ", " << okLabel
               << "\n";
        m_body << fatalLabel << "\n\tret " << kRtFatal << "\n";
        m_body << okLabel << "\n";
        m_fusedTagWordByJumpOffset[jumpTableOffset] = tag;
    }

    void emitTerminator(const BasicBlock& block, std::size_t blockIndex,
                        const Cfg& cfg) {
        if (block.successors.empty()) {
            // RETURN's own emitInstruction() already emitted `ret kRtOk`;
            // MATCH_ERROR's and THROW's each already emitted `ret kRtFatal`
            // as an unreachable safety net (both always transfer control
            // away via their own callSlowPathTyped's jmp/ret — S6, #459).
            // Anything else reaching here is a decoder/cfg-analysis
            // mismatch this node cannot recover from.
            Op lastOp = block.instructions.empty()
                            ? Op{}
                            : block.instructions.back().op;
            if (lastOp != Op::RETURN && lastOp != Op::MATCH_ERROR &&
                lastOp != Op::THROW) {
                throw std::runtime_error(
                    "qbe_emitter: terminal block with no cfg successor and "
                    "no RETURN/MATCH_ERROR/THROW — decoder/cfg drift");
            }
            return;
        }
        // S8 (#461): a cfg block can expand to several QBE blocks (its
        // fast/slow paths, a call's status handling). A phi at a successor
        // must name the QBE block that actually emits the branch, so every
        // non-terminal cfg block gets a stable `_tail` block that holds its
        // terminator — the last emitted path falls through into it.
        m_body << "@" << block.label << "_tail\n";
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
        // S8 (#461): a fused GET_TAG already produced the tag as a word
        // (emitFusedGetTagWord); the unfused path lowers the Number GET_TAG
        // pushed back to a word here.
        std::string tagW;
        auto fused = m_fusedTagWordByJumpOffset.find(jt.offset);
        if (fused != m_fusedTagWordByJumpOffset.end()) {
            tagW = fused->second;
        } else {
            std::string tagBits = loadl(addr(before.height - 1));
            std::string tagD = newTemp();
            m_body << "\t" << tagD << " =d cast " << tagBits << "\n";
            tagW = newTemp();
            m_body << "\t" << tagW << " =w dtosi " << tagD << "\n";
        }

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
            m_body << "\tret " << kRtFatal << "\n";
        }
    }
};

} // namespace

std::string emitScript(const DecodedFunction& fn,
                       const FunctionStackAnalysis& analysis,
                       const FunctionCaptureInfo& captures,
                       const std::string& qbeSymbol,
                       const EmitOptions& options) {
    Emitter emitter(fn, analysis, captures, options, qbeSymbol);
    return emitter.run();
}

} // namespace qbe
