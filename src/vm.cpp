#include "vm.h"
#include "debug.h"
#include "objects.h"
#include "object.h"
#include "utility.h"

#include "stdlib/stdlib_context.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

InterpretResult VM::interpret(const std::string& source) {
    ObjClosure* closure = m_rt.loadSource(source);
    if (closure == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }
    // No handler can be active yet (nothing has executed), so the only
    // reachable outcome here is Pushed or Uncaught.
    if (m_rt.call(closure, 0) == Runtime::ThrowOutcome::Uncaught) {
        return InterpretResult::RUNTIME_ERROR;
    }
#ifdef LOXPP_PROFILE
    // Count the implicit script call so Op::CALL and Op::RETURN stay balanced.
    m_rt.m_profilerData.opcodeTable[static_cast<uint8_t>(Op::CALL)].count++;
    ProfileProgramScope programScope(m_rt.m_profilerData);
#endif
    InterpretResult result = run();
#ifdef LOXPP_PROFILE
    // Fold every coroutine's tables into the root before the report prints.
    m_rt.memoryManager().mergeCoroutineProfilers();
#endif
    return result;
}

InterpretResult VM::run(int stopAtFrameCount) {
#define RAISE_ERROR(...)                                                       \
    do {                                                                       \
        frame->ip = ip;                                                        \
        m_rt.runtimeError(__VA_ARGS__);                                        \
    } while (false)

    // tryCatchableError(kind, msg) returns Runtime::ThrowOutcome (see
    // runtime.h). This macro reacts to it exactly the way every catchable-
    // fault call site must: return RUNTIME_ERROR if uncaught, return OK
    // immediately (without touching frame/ip/chunk — they may not even be
    // safe to read, see ThrowOutcome's doc comment) if the catch resolved
    // outside this run() invocation's own frame range, and otherwise do
    // nothing, so the call site's own trailing `break;` resumes dispatch
    // normally. Only `return` appears in this macro's body — never
    // `break`/`continue` — so it is safe to expand inside another do/while
    // (BINARY_OP) or directly inside a switch case without an enclosing
    // loop/switch swallowing a break that was meant for the opcode dispatch
    // switch.
#define CATCHABLE_OR_RETURN(outcome_expr)                                      \
    do {                                                                       \
        Runtime::ThrowOutcome _catchableOutcome = (outcome_expr);              \
        if (_catchableOutcome == Runtime::ThrowOutcome::Uncaught) {            \
            return InterpretResult::RUNTIME_ERROR;                             \
        }                                                                      \
        if (_catchableOutcome == Runtime::ThrowOutcome::HandledStop) {         \
            return InterpretResult::OK;                                        \
        }                                                                      \
    } while (false)

    // Local to VM::run(). The single place that knows how to derive the
    // register-cached ip/chunk from the live CallFrame array — used for the
    // initial load below, for Op::RETURN's reload, and (via
    // dispatchOp/tryCatchableError below) after any Runtime call that can
    // allocate, call, or throw: flush the register-cached ip into frame->ip
    // before that call (so a runtimeError() raised inside sees an up-to-date
    // stack trace), then reload frame/ip/chunk from the current top of
    // m_rt's frames afterward (a new frame if one was pushed, the same frame
    // — a no-op reload — otherwise).
    class FrameSync {
      public:
        // Requires frameCount > 0 — true for every caller (the initial load
        // below, Op::RETURN, and dispatchOp/tryCatchableError after a
        // successful or caught-and-continuing call, all of which already
        // checked or guarantee it).
        static void loadTop(CallFrame* frames, int frameCount,
                            CallFrame*& frame, Chunk::const_iterator& ip,
                            const Chunk*& chunk) {
            frame = &frames[frameCount - 1];
            ip = frame->ip;
            chunk = &frame->closure->function->chunk;
        }
    };

    CallFrame* frame;
    Chunk::const_iterator ip;
    const Chunk* chunk; // constant-pool base
    FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip, chunk);

    auto readByte = [&ip]() -> Byte { return *ip++; };
    auto readShort = [&readByte]() -> uint16_t {
        uint16_t hi = readByte();
        uint16_t lo = readByte();
        return static_cast<uint16_t>((hi << 8) | lo);
    };

    // Helper lambda to construct and potentially catch a runtime error.
    // Syncs frame->ip before any operation, then forwards to
    // Runtime::raiseThrowableError with THIS run() invocation's own
    // stopAtFrameCount — the boundary a reentrant fault must be judged
    // against, not always 0 (see ThrowOutcome's doc comment in runtime.h;
    // using the wrong boundary here is exactly what let a caught fault
    // corrupt an unrelated, more-nested run() invocation's frame
    // bookkeeping). On HandledContinue, reloads frame/ip/chunk from the new
    // top — safe, since m_frameCount is still above stopAtFrameCount. On
    // HandledStop or Uncaught, frame/ip/chunk are deliberately left
    // untouched: CATCHABLE_OR_RETURN returns from run() before either is
    // read again, and reading them here could be out of bounds (m_frameCount
    // may be 0).
    auto tryCatchableError = [this, &frame, &ip, &chunk, stopAtFrameCount](
                                 const char* kind_str,
                                 const char* msg) -> Runtime::ThrowOutcome {
        frame->ip = ip; // Sync frame->ip before allocations (fixes line number)

        Runtime::ThrowOutcome outcome =
            m_rt.raiseThrowableError(kind_str, msg, stopAtFrameCount);
        if (outcome == Runtime::ThrowOutcome::HandledContinue) {
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
        }
        return outcome;
    };

    // Calls a Runtime op*() opcode helper and dispatches its result (see
    // runtime.h's Runtime::OpResult). Takes the call itself, not an
    // already-evaluated result, so it can flush frame->ip immediately
    // before invoking it: a runtimeError() raised inside must see this
    // opcode's own line, and nothing but convention enforced that flush at
    // each of the ~11 call sites when it lived there instead (the old
    // single-file VM's FrameSync did this via its constructor, structurally,
    // not by convention). Returns a value only when run() must return
    // immediately; std::nullopt means "the opcode is done, resume dispatch"
    // — frame/ip/chunk are reloaded first only for Resumed (the helper
    // pushed a frame or a caught error unwound one); OK means they are
    // already exactly what the caller has, so skipping the reload is more
    // than a correct no-op — it is one dispatch's whole reason to still be
    // on the fast path.
    auto dispatchOp = [this, &frame, &ip,
                       &chunk](auto&& call) -> std::optional<InterpretResult> {
        frame->ip = ip;
        switch (Runtime::OpResult result = call(); result) {
        case Runtime::OpResult::OK:
            return std::nullopt;
        case Runtime::OpResult::Resumed:
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
            return std::nullopt;
        case Runtime::OpResult::Stop:
            return InterpretResult::OK;
        case Runtime::OpResult::Fatal:
            return InterpretResult::RUNTIME_ERROR;
        }
        return InterpretResult::RUNTIME_ERROR; // unreachable
    };

    auto readConstant = [&chunk, &readShort]() -> Value {
        return chunk->getConstant(readShort());
    };

    for (;;) {
        if (m_rt.m_stackOverflow) {
            // Consume the flag now: whichever branch below runs, this exact
            // overflow event is fully handled by it (caught, reported, or
            // re-armed by a fresh push() past the threshold later).
            m_rt.m_stackOverflow = false;
            if (!m_rt.m_handlerStack.empty() &&
                !m_rt.m_unwindingStackOverflow) {
                // See m_unwindingStackOverflow's own comment (runtime.h) and
                // Runtime::call()'s matching guard: hold the flag for
                // exactly this catch attempt, so a nested hit (from a
                // deferred call this unwind drains) falls through to the
                // fatal branch below instead of recursing.
                m_rt.m_unwindingStackOverflow = true;
                Runtime::ThrowOutcome outcome =
                    tryCatchableError("StackOverflowError", "Stack overflow.");
                m_rt.m_unwindingStackOverflow = false;
                CATCHABLE_OR_RETURN(outcome);
            } else {
                RAISE_ERROR("Stack overflow.");
                return InterpretResult::RUNTIME_ERROR;
            }
        }

#ifdef LOXPP_DEBUG_TRACE_EXECUTION
        {
            // The stack dump is an internal stringify, so it must not dispatch
            // __str__ (object.cpp's INSTANCE case). While an outer print/str
            // runs, the canonical depth is > 0; without this the dump would
            // re-enter the user's own __str__ once per traced instruction and
            // recurse, because the method's frame is still on this stack.
            const int savedCanonicalDepth = m_rt.m_stringifyCanonicalDepth;
            m_rt.m_stringifyCanonicalDepth = 0;
            int currentOffset = static_cast<int>(ip - chunk->cbegin());
            bool color = isatty(STDOUT_FILENO) != 0;
            std::printf("[line %d] ", chunk->getLine(currentOffset));
            std::printf("          ");
            for (Value* slot = m_rt.stack; slot < m_rt.stackTop; slot++) {
                std::printf("[ ");
                printValue(*slot);
                std::printf(" ]");
            }
            std::printf("\n");
            disassembleInstruction(*chunk, m_rt.m_mm, currentOffset, std::cout,
                                   color);
            m_rt.m_stringifyCanonicalDepth = savedCanonicalDepth;
        }
#endif

        if (m_rt.m_handlerDepthTrace != nullptr) {
            int currentOffset = static_cast<int>(ip - chunk->cbegin());
            m_rt.m_handlerDepthTrace->emplace_back(
                currentOffset, static_cast<int>(m_rt.m_handlerStack.size()));
        }

        Byte instruction = readByte();
#ifdef LOXPP_PROFILE
        m_rt.m_activeProfiler->opcodeTable[instruction].count++;
#endif
        switch (toOpcode(instruction)) {
        case Op::CONSTANT: {
            m_rt.push(readConstant());
            break;
        }
        case Op::NIL: {
            m_rt.push(from<Nil>(Nil{}));
            break;
        }
        case Op::TRUE: {
            m_rt.push(from<bool>(true));
            break;
        }
        case Op::FALSE: {
            m_rt.push(from<bool>(false));
            break;
        }
        case Op::EQUAL: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<bool>(a == b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opEqual(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::GREATER: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<bool>(a > b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opGreater(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::LESS: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<bool>(a < b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opLess(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::NEGATE: {
            if (is<Number>(m_rt.peek(0))) {
                m_rt.push(from<Number>(-as<Number>(m_rt.pop())));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opNegate(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::ADD: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<Number>(a + b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opAdd(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::SUBTRACT: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<Number>(a - b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opSubtract(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::MULTIPLY: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<Number>(a * b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opMultiply(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::DIVIDE: {
            if (is<Number>(m_rt.peek(0)) && is<Number>(m_rt.peek(1))) {
                Number b = as<Number>(m_rt.pop());
                Number a = as<Number>(m_rt.pop());
                m_rt.push(from<Number>(a / b));
            } else {
                if (auto ret = dispatchOp(
                        [&] { return m_rt.opDivide(stopAtFrameCount); })) {
                    return *ret;
                }
            }
            break;
        }
        case Op::MODULO: {
            // No inline fast path: MODULO's floor-division number case lives in
            // Runtime::opModulo (see its own comment — the QBE backend reuses
            // it, and the sign correction is not a plain double-double case).
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opModulo(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::NOT: {
            m_rt.opNot();
            break;
        }
        case Op::LEN: {
            if (auto ret =
                    dispatchOp([&] { return m_rt.opLen(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::STR: {
            if (auto ret =
                    dispatchOp([&] { return m_rt.opStr(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::PRINT: {
            frame->ip = ip;
            Runtime::OpResult result = m_rt.opStr(stopAtFrameCount);
            if (result == Runtime::OpResult::OK) {
                ObjString* s = asObjString(m_rt.pop());
                std::fwrite(s->chars.data(), 1, s->chars.size(), stdout);
                std::printf("\n");
                break;
            }
            if (result == Runtime::OpResult::Resumed) {
                FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                                   chunk);
                break;
            }
            return result == Runtime::OpResult::Stop
                       ? InterpretResult::OK
                       : InterpretResult::RUNTIME_ERROR;
        }
        case Op::POP: {
            m_rt.m_lastResult = m_rt.pop();
            break;
        }
        case Op::GET_LOCAL: {
            uint8_t slot = readByte();
            m_rt.push(frame->slots[slot]);
            break;
        }
        case Op::SET_LOCAL: {
            uint8_t slot = readByte();
            // assignment is an expression; leave value on stack
            frame->slots[slot] = m_rt.peek(0);
            break;
        }
        case Op::DEFINE_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            m_rt.opDefineGlobal(name);
            break;
        }
        case Op::GET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opGetGlobal(name, stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::SET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opSetGlobal(name, stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::JUMP: {
            ip += readShort();
            break;
        }
        case Op::JUMP_IF_FALSE: {
            uint16_t offset = readShort();
            if (isFalsy(m_rt.peek(0))) {
                ip += offset;
            }
            break;
        }
        case Op::LOOP: {
            ip -= readShort();
            break;
        }
        case Op::MATCH_ERROR: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opMatchError(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::JUMP_TABLE: {
            uint8_t minTag = readByte();
            uint8_t count = readByte();
            auto tableBase = ip; // iterator to entry[0]
            ip += static_cast<int>(count) * 2;
            Value tagVal = m_rt.pop();
            int tag = static_cast<int>(as<Number>(tagVal));
            int idx = tag - static_cast<int>(minTag);
            if (idx >= 0 && idx < static_cast<int>(count)) {
                uint16_t fwd = static_cast<uint16_t>((tableBase[idx * 2] << 8) |
                                                     tableBase[(idx * 2) + 1]);
                ip += fwd;
            }
            break;
        }
        case Op::GET_TAG: {
            if (auto ret = dispatchOp([&] { return m_rt.opGetTag(); })) {
                return *ret;
            }
            break;
        }
        case Op::IS_SEQ: {
            m_rt.opIsSeq();
            break;
        }
        case Op::INSTANCEOF: {
            ObjString* className = asObjString(readConstant());
            m_rt.opInstanceof(className);
            break;
        }
        case Op::CALL: {
            int argCount = readByte();
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opCall(argCount, stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::CLASS: {
            ObjString* name = asObjString(readConstant());
            m_rt.opClass(name);
            break;
        }
        case Op::GET_PROPERTY: {
            ObjString* name = asObjString(readConstant());
            if (auto ret = dispatchOp([&] {
                    return m_rt.opGetProperty(name, stopAtFrameCount);
                })) {
                return *ret;
            }
            break;
        }
        case Op::SET_PROPERTY: {
            ObjString* name = asObjString(readConstant());
            if (auto ret =
                    dispatchOp([&] { return m_rt.opSetProperty(name); })) {
                return *ret;
            }
            break;
        }
        case Op::DEFINE_METHOD: {
            ObjString* name = asObjString(readConstant());
            m_rt.opDefineMethod(name);
            break;
        }
        case Op::INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            if (auto ret = dispatchOp([&] {
                    return m_rt.opInvoke(name, argCount, stopAtFrameCount);
                })) {
                return *ret;
            }
            break;
        }
        case Op::INHERIT: {
            if (auto ret = dispatchOp([&] { return m_rt.opInherit(); })) {
                return *ret;
            }
            break;
        }
        case Op::GET_SUPER: {
            ObjString* name = asObjString(readConstant());
            if (auto ret = dispatchOp([&] { return m_rt.opGetSuper(name); })) {
                return *ret;
            }
            break;
        }
        case Op::SUPER_INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            if (auto ret = dispatchOp([&] {
                    return m_rt.opSuperInvoke(name, argCount, stopAtFrameCount);
                })) {
                return *ret;
            }
            break;
        }
        case Op::CLOSURE: {
            ObjFunction* fn = asObjFunction(readConstant());
            ObjClosure* cl = m_rt.m_mm.create<ObjClosure>(fn);
            m_rt.push(Value{static_cast<Obj*>(cl)});
            for (int i = 0; i < fn->upvalueCount; i++) {
                uint8_t isLocal = readByte();
                uint8_t index = readByte();
                if (isLocal) {
                    cl->upvalues[i] = m_rt.captureUpvalue(frame->slots + index);
                } else {
                    cl->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }
        case Op::GET_UPVALUE: {
            uint8_t slot = readByte();
            m_rt.push(*frame->closure->upvalues[slot]->location);
            break;
        }
        case Op::SET_UPVALUE: {
            uint8_t slot = readByte();
            *frame->closure->upvalues[slot]->location = m_rt.peek(0);
            break;
        }
        case Op::CLOSE_UPVALUE: {
            m_rt.closeUpvalues(m_rt.stackTop - 1);
            m_rt.pop();
            break;
        }
        case Op::RETURN: {
            Value result = m_rt.pop();
            m_rt.closeUpvalues(frame->slots);
            // A defer-free function has no RUN_DEFERS, so this is the only
            // place its own stale records get discarded. A function with
            // defers already had this done by RUN_DEFERS below, before its
            // defers ran; this is then a no-op.
            m_rt.popHandlersOwnedByCurrentFrame();
            Runtime::ResultCheck check =
                m_rt.m_frameResultCheck[m_rt.m_frameCount - 1];
            m_rt.m_frameResultCheck[m_rt.m_frameCount - 1] =
                Runtime::ResultCheck::None;
            bool overrideSet =
                m_rt.m_frameResultOverrideSet[m_rt.m_frameCount - 1];
            Value overrideVal =
                m_rt.m_frameResultOverride[m_rt.m_frameCount - 1];
            m_rt.m_frameResultOverrideSet[m_rt.m_frameCount - 1] = false;
#ifdef LOXPP_PROFILE
            // Destroy the function scope before decrementing frameCount so the
            // depth index still points to this frame's slot.
            m_rt.m_profilerScopes[m_rt.m_frameCount - 1].reset();
#endif
            m_rt.m_frameCount--;
            if (m_rt.m_frameCount == 0) {
                // Finished executing the top-level script.
                m_rt.pop(); // remove the script ObjClosure from the stack
                return InterpretResult::OK;
            }
            // Discard the callee's stack window and push return value.
            m_rt.stackTop = frame->slots;
            m_rt.push(overrideSet ? overrideVal : result);
            // __iter__: the method returns the sequence to iterate; replace it
            // with the iterator. `result` is rooted on the stack during the
            // create<>().
            if (check == Runtime::ResultCheck::Sequence &&
                (isList(result) || isString(result) || isMap(result) ||
                 isCoroutine(result))) {
                Obj* obj = as<Obj*>(result);
                ObjIterator* it = m_rt.m_mm.create<ObjIterator>(
                    result, 0, isObjMap(obj) ? asObjMap(obj)->version : -1);
                m_rt.stackTop[-1] = Value{static_cast<Obj*>(it)};
            }
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
            if (check == Runtime::ResultCheck::Boolean && !is<bool>(result)) {
                // An operator-overloading method returned a non-Boolean. The
                // method frame is already gone, so this raises at the caller's
                // operator site and any handler there (or above) can catch it.
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "OperatorResultTypeError",
                    "Operator method must return a Boolean."));
                break;
            }
            if (check == Runtime::ResultCheck::Number && !is<Number>(result)) {
                CATCHABLE_OR_RETURN(
                    tryCatchableError("OperatorResultTypeError",
                                      "Operator method must return a Number."));
                break;
            }
            if (check == Runtime::ResultCheck::Sequence &&
                !(isList(result) || isString(result) || isMap(result) ||
                  isCoroutine(result))) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "OperatorResultTypeError",
                    "Operator method must return a sequence."));
                break;
            }
            if (check == Runtime::ResultCheck::String && !isString(result)) {
                CATCHABLE_OR_RETURN(
                    tryCatchableError("OperatorResultTypeError",
                                      "Operator method must return a String."));
                break;
            }
            if (m_rt.m_frameCount <= stopAtFrameCount) {
                // A nested run() (draining a deferred call — see
                // Runtime::runPendingDefers) reached the depth it was asked
                // to stop at; hand control back to whichever C++ frame
                // started it.
                return InterpretResult::OK;
            }
            break;
        }
        case Op::BUILD_LIST: {
            uint8_t count = readByte();
            if (auto ret =
                    dispatchOp([&] { return m_rt.opBuildList(count); })) {
                return *ret;
            }
            break;
        }
        case Op::BUILD_MAP: {
            uint8_t count = readByte();
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opBuildMap(count, stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::GET_INDEX: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opGetIndex(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::SET_INDEX: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opSetIndex(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::SLICE: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opSlice(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::IN: {
            if (auto ret =
                    dispatchOp([&] { return m_rt.opIn(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::GET_ITER: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opGetIter(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::ITER_HAS_NEXT: {
            if (auto ret = dispatchOp(
                    [&] { return m_rt.opIterHasNext(stopAtFrameCount); })) {
                return *ret;
            }
            break;
        }
        case Op::ITER_NEXT: {
            if (auto ret = dispatchOp([&] { return m_rt.opIterNext(); })) {
                return *ret;
            }
            break;
        }
        case Op::PUSH_HANDLER: {
            uint16_t catchOffset = readShort();
            // catchOffset is relative to the current IP, just like JUMP.
            // ip points to the first byte after the PUSH_HANDLER instruction.
            Chunk::const_iterator catchIp = ip + catchOffset;
            m_rt.pushHandler(m_rt.stackTop, catchIp);
            break;
        }
        case Op::POP_HANDLER: {
            if (m_rt.handlerStackDepth() == 0) {
                RAISE_ERROR("BUG: POP_HANDLER with empty handler stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            m_rt.popTopHandler();
            break;
        }
        case Op::DEFER_RECORD: {
            uint8_t argc = readByte();
            m_rt.opDeferRecord(argc);
            break;
        }
        case Op::RUN_DEFERS: {
            int frameIndex = m_rt.m_frameCount - 1;
            // The compiler emits RUN_DEFERS only immediately before RETURN,
            // in the same frame, so this is a return leaving this frame —
            // see INVARIANT(handler-stack-frame-scoped) on m_handlerStack's
            // declaration (runtime.h). This frame's own stale records must
            // be gone BEFORE its defers run, not only at the RETURN below:
            // otherwise a defer that throws here would still match this
            // frame's own (already-exited) protected region instead of
            // unwinding to the real caller.
            m_rt.popHandlersOwnedByCurrentFrame();
            // Flush ip into frame->ip first: runPendingDefers may run
            // arbitrary Lox++ code (each deferred call, to completion), and
            // a runtimeError() raised inside it must see this frame's
            // current position, not a stale one (see FrameSync's own
            // comment above for why this matters).
            frame->ip = ip;
            InterpretResult result =
                m_rt.runPendingDefers(frameIndex, stopAtFrameCount);
            if (result != InterpretResult::OK) {
                return result;
            }
            // Checking against frameIndex here (instead of stopAtFrameCount)
            // was a second instance of R15's bug class: a deferred call's own
            // fault can be caught by a handler ABOVE stopAtFrameCount but AT
            // OR BELOW frameIndex (e.g. an outer try wrapping this frame's
            // own call site) — that catch's frame is a real, still-live
            // frame belonging to THIS SAME run() invocation, and its ip now
            // correctly points at the catch block. Checking frameIndex
            // treated that live catch context as "gone" and returned OK
            // without ever dispatching it (reproduced: defer_throw_outer_
            // catch.lox printed only "middle body" and silently exited 0,
            // never reaching its catch block or "program end"). Only
            // stopAtFrameCount — this run() invocation's own boundary — is
            // the right test: below it, control belongs to a different,
            // less-nested run() invocation (m_frameCount may even be 0);
            // at or above it, any surviving frame is this invocation's own
            // and must be dispatched, whether or not it is still frameIndex.
            if (m_rt.m_frameCount <= stopAtFrameCount) {
                return InterpretResult::OK;
            }
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
            break;
        }
        case Op::YIELD: {
            if (m_rt.m_currentCoroutine == nullptr) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "YieldOutsideCoroutineError",
                    "Cannot yield from outside a coroutine."));
                break;
            }
            if (m_rt.m_reentrantRunDepth >
                m_rt.m_currentCoroutine->resumeReentrantDepth) {
                // A native (or a __str__/__hash__/__eq__ method invoked by
                // one, or a defer drain) was entered inside this coroutine
                // since it was resumed. That C++ frame's continuation cannot
                // be captured, so suspending here would silently drop its
                // work. A C++ frame that was already below the resume point
                // is fine: it keeps running while the coroutine is suspended.
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "YieldAcrossNativeError",
                    "Cannot yield across a native callback."));
                break;
            }
            // Flush the register-cached ip before the coroutine snapshot
            // takes frame->ip, and exit this nested run so whoever resumed
            // the coroutine gets the yielded value back.
            frame->ip = ip;
            Value yielded = m_rt.pop();
            m_rt.suspendCurrentCoroutine(yielded);
            return InterpretResult::OK;
        }
        case Op::THROW: {
            Value thrownValue = m_rt.pop();
            // Use the shared unwind implementation (same as runtime faults).
            // dispatchOp flushes frame->ip before calling handleThrowOp, so
            // a runtimeError() raised inside sees the THROW's own line; its
            // OpResult switch collapses Uncaught/HandledStop/HandledContinue
            // exactly the way fromThrow() maps them everywhere else.
            if (auto ret = dispatchOp([&] {
                    return m_rt.handleThrowOp(thrownValue, stopAtFrameCount);
                })) {
                return *ret;
            }
            break;
        }
        }
    }

#undef CATCHABLE_OR_RETURN
#undef RAISE_ERROR
}
