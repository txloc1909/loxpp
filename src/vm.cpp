#include "vm.h"
#include "debug.h"
#include "objects.h"
#include "object.h"
#include "compiler.h"
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
    // Guard against dangling class pointers from a prior VM instance. GC can
    // fire inside compile(), and markRoots() must not dereference a pointer
    // that was freed when the previous VM's MemoryManager was destroyed.
    m_rt.m_fileClass = nullptr;
    m_rt.m_mapClass = nullptr;
    ObjFunction* fn = compile(source, &m_rt.m_mm);
    if (fn == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    // Root fn on the stack before any allocation (defineNatives,
    // create<ObjClosure>) can trigger GC. Without this, fn is unreachable
    // between compile() returning and push(closure) — the Compiler has already
    // been destroyed and m_currentCompiler is nullptr.
    m_rt.push(Value{static_cast<Obj*>(fn)});
    m_rt.m_stdlibCtx.mm = &m_rt.m_mm;
    setActiveContext(&m_rt.m_stdlibCtx);
    m_rt.defineNatives();
    ObjClosure* closure = m_rt.m_mm.create<ObjClosure>(fn);
    m_rt.stackTop[-1] =
        Value{static_cast<Obj*>(closure)}; // replace fn with its closure
    // No handler can be active yet (nothing has executed), so the only
    // reachable outcome here is Pushed or Uncaught.
    if (m_rt.call(closure, 0) == Runtime::CallOutcome::Uncaught) {
        return InterpretResult::RUNTIME_ERROR;
    }
#ifdef LOXPP_PROFILE
    // Count the implicit script call so Op::CALL and Op::RETURN stay balanced.
    m_rt.m_profilerData.opcodeTable[static_cast<uint8_t>(Op::CALL)].count++;
    ProfileProgramScope programScope(m_rt.m_profilerData);
#endif
    return run();
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

#define BINARY_OP(valueType, op, kind_str, msg)                                \
    do {                                                                       \
        if (!is<Number>(m_rt.peek(0)) || !is<Number>(m_rt.peek(1))) {          \
            CATCHABLE_OR_RETURN(tryCatchableError(kind_str, msg));             \
            break;                                                             \
        }                                                                      \
        Number b = as<Number>(m_rt.pop());                                     \
        Number a = as<Number>(m_rt.pop());                                     \
        m_rt.push(as<valueType>(a op b));                                      \
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

    // Dispatches the result of a Runtime op*() opcode helper (see
    // runtime.h's Runtime::OpResult). Returns a value only when run() must
    // return immediately; std::nullopt means "the opcode is done, resume
    // dispatch" (frame/ip/chunk are reloaded first — a correct no-op unless
    // the helper pushed a frame or a caught error unwound one).
    auto dispatchOp =
        [this, &frame, &ip,
         &chunk](Runtime::OpResult result) -> std::optional<InterpretResult> {
        switch (result) {
        case Runtime::OpResult::OK:
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
        }
#endif

        if (m_rt.m_handlerDepthTrace != nullptr) {
            int currentOffset = static_cast<int>(ip - chunk->cbegin());
            m_rt.m_handlerDepthTrace->emplace_back(
                currentOffset, static_cast<int>(m_rt.m_handlerStack.size()));
        }

        Byte instruction = readByte();
#ifdef LOXPP_PROFILE
        m_rt.m_profilerData.opcodeTable[instruction].count++;
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
            Value b = m_rt.pop();
            Value a = m_rt.pop();
            m_rt.push(from<bool>(a == b));
            break;
        }
        case Op::GREATER: {
            BINARY_OP(bool, >, "ComparisonTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::LESS: {
            BINARY_OP(bool, <, "ComparisonTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::NEGATE: {
            if (!is<Number>(m_rt.peek(0))) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "ArithmeticTypeError", "Operand must be a number."));
                break;
            }
            m_rt.push(from<Number>(-as<Number>(m_rt.pop())));
            break;
        }
        case Op::ADD: {
            if (isString(m_rt.peek(0)) && isString(m_rt.peek(1))) {
                auto* b_str = asObjString(m_rt.pop());
                auto* a_str = asObjString(m_rt.pop());
                std::string result;
                result.reserve(a_str->chars.size() + b_str->chars.size());
                result.append(a_str->chars.data(), a_str->chars.size());
                result.append(b_str->chars.data(), b_str->chars.size());
                m_rt.push(Value{static_cast<Obj*>(
                    m_rt.m_mm.makeString(std::move(result)))});
            } else {
                BINARY_OP(Number, +, "ConcatenationTypeError",
                          "Operands must be two numbers, two strings, or a "
                          "string and a number.");
            }
            break;
        }
        case Op::SUBTRACT: {
            BINARY_OP(Number, -, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::MULTIPLY: {
            BINARY_OP(Number, *, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::DIVIDE: {
            BINARY_OP(Number, /, "ArithmeticTypeError",
                      "Operands must be numbers.");
            break;
        }
        case Op::MODULO: {
            if (!is<Number>(m_rt.peek(0)) || !is<Number>(m_rt.peek(1))) {
                CATCHABLE_OR_RETURN(tryCatchableError(
                    "ArithmeticTypeError", "Operands must be numbers."));
                break;
            }
            Number b = as<Number>(m_rt.pop());
            Number a = as<Number>(m_rt.pop());
            Number result = std::fmod(a, b);
            // Floor-division semantics: result has same sign as b (Python/Lua
            // behavior)
            if (result != 0 && (result < 0) != (b < 0)) {
                result += b;
            }
            m_rt.push(from<Number>(result));
            break;
        }
        case Op::NOT: {
            m_rt.push(from<bool>(!m_rt.pop()));
            break;
        }
        case Op::PRINT: {
            m_rt.m_stdlibCtx.clearError();
            std::string s = stringify(m_rt.pop());
            if (m_rt.m_stdlibCtx.nativeError) {
                RAISE_ERROR("%s", m_rt.m_stdlibCtx.nativeErrorMsg.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            std::fwrite(s.data(), 1, s.size(), stdout);
            std::printf("\n");
            break;
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
            m_rt.m_globals.set(name, m_rt.peek(0));
            m_rt.pop();
            break;
        }
        case Op::GET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            Value value;
            if (!m_rt.m_globals.get(name, value)) {
                CATCHABLE_OR_RETURN(tryCatchableError("UndefinedVariableError",
                                                      "Undefined variable."));
                break;
            }
            m_rt.push(value);
            break;
        }
        case Op::SET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            // set() returns true if the key is *new*; an existing key is
            // valid. An entirely new key means the variable was never
            // declared.
            if (m_rt.m_globals.set(name, m_rt.peek(0))) {
                m_rt.m_globals.del(name); // undo the spurious insertion
                CATCHABLE_OR_RETURN(tryCatchableError("UndefinedVariableError",
                                                      "Undefined variable."));
                break;
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
            CATCHABLE_OR_RETURN(tryCatchableError(
                "MatchError", "No matching arm in match expression."));
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
            Value val = m_rt.pop();
            if (!isEnumValue(val)) {
                RAISE_ERROR("GET_TAG: expected an enum value.");
                return InterpretResult::RUNTIME_ERROR;
            }
            auto tag = static_cast<double>(asObjEnum(as<Obj*>(val))->ctor->tag);
            m_rt.push(Value{tag});
            break;
        }
        case Op::IS_SEQ: {
            Value val = m_rt.pop();
            m_rt.push(Value{isList(val) || isString(val)});
            break;
        }
        case Op::INSTANCEOF: {
            ObjString* className = asObjString(readConstant());
            Value val = m_rt.pop();
            Value classVal;
            bool result = false;
            if (m_rt.m_globals.get(className, classVal) && isClass(classVal)) {
                ObjClass* target = asObjClass(as<Obj*>(classVal));
                if (isInstance(val)) {
                    ObjClass* klass = asObjInstance(as<Obj*>(val))->klass;
                    const ObjClass* found = walkChain<ObjClass>(
                        klass,
                        [target](const ObjClass* k) { return k == target; },
                        [](const ObjClass* k) { return k->superclass; });
                    result = found != nullptr;
                }
            }
            m_rt.push(Value{result});
            break;
        }
        case Op::CALL: {
            int argCount = readByte();
            frame->ip = ip;
            if (auto ret =
                    dispatchOp(m_rt.opCall(argCount, stopAtFrameCount))) {
                return *ret;
            }
            break;
        }
        case Op::CLASS: {
            ObjString* name = asObjString(readConstant());
            ObjClass* klass = m_rt.m_mm.create<ObjClass>(
                name, VmAllocator<Entry>{&m_rt.m_mm});
            m_rt.push(Value{static_cast<Obj*>(klass)});
            break;
        }
        case Op::GET_PROPERTY: {
            ObjString* name = asObjString(readConstant());
            frame->ip = ip;
            if (auto ret =
                    dispatchOp(m_rt.opGetProperty(name, stopAtFrameCount))) {
                return *ret;
            }
            break;
        }
        case Op::SET_PROPERTY: {
            if (!isInstance(m_rt.peek(1))) {
                RAISE_ERROR("Only instances have fields.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjInstance* instance = asObjInstance(as<Obj*>(m_rt.peek(1)));
            ObjString* name = asObjString(readConstant());
            instance->fields.set(name, m_rt.peek(0));
            Value val = m_rt.pop(); // value
            m_rt.pop();             // instance
            m_rt.push(val);         // assignment is an expression
            break;
        }
        case Op::DEFINE_METHOD: {
            ObjString* name = asObjString(readConstant());
            Value method = m_rt.peek(0); // ObjClosure* on top
            ObjClass* klass = asObjClass(as<Obj*>(m_rt.peek(1))); // class below
            klass->methods.set(name, method);
            m_rt.pop(); // pop closure; leave class on stack for next method
            break;
        }
        case Op::INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            frame->ip = ip;
            if (auto ret = dispatchOp(
                    m_rt.opInvoke(name, argCount, stopAtFrameCount))) {
                return *ret;
            }
            break;
        }
        case Op::INHERIT: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opInherit())) {
                return *ret;
            }
            break;
        }
        case Op::GET_SUPER: {
            ObjString* name = asObjString(readConstant());
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opGetSuper(name))) {
                return *ret;
            }
            break;
        }
        case Op::SUPER_INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            frame->ip = ip;
            if (auto ret = dispatchOp(
                    m_rt.opSuperInvoke(name, argCount, stopAtFrameCount))) {
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
            m_rt.push(result);
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
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
            ObjList* list =
                m_rt.m_mm.create<ObjList>(VmAllocator<Value>{&m_rt.m_mm});
            m_rt.m_mm.pushTempRoot(
                list); // protect across resize's potential GC
            list->elements.resize(count);
            for (int i = count - 1; i >= 0; i--) {
                list->elements[i] = m_rt.pop();
            }
            m_rt.m_mm.popTempRoot();
            m_rt.push(Value{static_cast<Obj*>(list)});
            break;
        }
        case Op::BUILD_MAP: {
            uint8_t count = readByte();
            // Validate all keys before any allocation. Stack (top to bottom):
            //   val_{n-1}, key_{n-1}, ..., val_0, key_0
            bool errorCaught = false;
            for (int i = 0; i < count; i++) {
                Value key = m_rt.peek(2 * (count - 1 - i) + 1);
                // Check for NaN first
                if (is<Number>(key) && std::isnan(as<Number>(key))) {
                    // The macro's own early returns cover Uncaught/HandledStop;
                    // reaching here means HandledContinue, so break out of this
                    // validation loop (not the outer switch — errorCaught does
                    // that below) same as before.
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    errorCaught = true;
                    break;
                }
                // Then check for invalid object types (non-String)
                if (is<Obj*>(key) && as<Obj*>(key)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    errorCaught = true;
                    break;
                }
            }
            if (errorCaught) {
                break;
            }
            ObjMap* map = m_rt.m_mm.create<ObjMap>(
                m_rt.m_mapClass, VmAllocator<MapEntry>{&m_rt.m_mm});
            // Values are still on the stack → GC-rooted; map is temp-rooted
            // so it survives any GC triggered by mapSet's grow.
            m_rt.m_mm.pushTempRoot(map);
            for (int i = 0; i < count; i++) {
                Value key = m_rt.peek(2 * (count - 1 - i) + 1);
                Value val = m_rt.peek(2 * (count - 1 - i));
                map->mapSet(key, val);
            }
            m_rt.m_mm.popTempRoot();
            for (int i = 0; i < 2 * count; i++) {
                m_rt.pop();
            }
            m_rt.push(Value{static_cast<Obj*>(map)});
            break;
        }
        case Op::GET_INDEX: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opGetIndex(stopAtFrameCount))) {
                return *ret;
            }
            break;
        }
        case Op::SET_INDEX: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opSetIndex(stopAtFrameCount))) {
                return *ret;
            }
            break;
        }
        case Op::SLICE: {
            // Stack (bottom→top): seq, start, end
            Value endVal = m_rt.peek(0);
            Value startVal = m_rt.peek(1);
            Value seqVal = m_rt.peek(2);

            if (!isList(seqVal) && !isString(seqVal)) {
                RAISE_ERROR("Slice requires a List or String.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(startVal)) {
                RAISE_ERROR("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double startD = as<Number>(startVal);
            if (startD != std::floor(startD)) {
                RAISE_ERROR("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (startD < 0.0) {
                RAISE_ERROR("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(endVal)) {
                RAISE_ERROR("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double endD = as<Number>(endVal);
            if (endD != std::floor(endD)) {
                RAISE_ERROR("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (endD < 0.0) {
                RAISE_ERROR("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }

            if (isList(seqVal)) {
                auto* src = asObjList(as<Obj*>(seqVal));
                int n = static_cast<int>(src->elements.size());
                int s = static_cast<int>(std::min(startD, (double)n));
                int e = static_cast<int>(std::min(endD, (double)n));
                int count = (s < e) ? e - s : 0;

                ObjList* result =
                    m_rt.m_mm.create<ObjList>(VmAllocator<Value>{&m_rt.m_mm});
                // seqVal is still at peek(2) → src is GC-rooted on the stack
                m_rt.m_mm.pushTempRoot(result);
                result->elements.resize(count); // may trigger GC
                src = asObjList(
                    as<Obj*>(m_rt.peek(2))); // re-read after potential GC
                for (int i = 0; i < count; i++) {
                    result->elements[i] = src->elements[s + i];
                }
                m_rt.m_mm.popTempRoot();
                m_rt.pop();
                m_rt.pop();
                m_rt.pop();
                m_rt.push(Value{static_cast<Obj*>(result)});
            } else {
                // String — copy chars to local buffer while src is still on
                // stack
                auto* src = asObjString(as<Obj*>(seqVal));
                int n = static_cast<int>(src->chars.size());
                int s = static_cast<int>(std::min(startD, (double)n));
                int e = static_cast<int>(std::min(endD, (double)n));
                std::string substr =
                    (s < e) ? std::string(src->chars.data() + s,
                                          static_cast<size_t>(e - s))
                            : std::string{};
                m_rt.pop();
                m_rt.pop();
                m_rt.pop();
                m_rt.push(Value{static_cast<Obj*>(
                    m_rt.m_mm.makeString(std::move(substr)))});
            }
            break;
        }
        case Op::IN: {
            Value seq = m_rt.pop();
            Value elem = m_rt.pop();
            if (isList(seq)) {
                auto* list = asObjList(as<Obj*>(seq));
                bool found = false;
                for (const auto& v : list->elements) {
                    if (v == elem) {
                        found = true;
                        break;
                    }
                }
                m_rt.push(from<bool>(found));
            } else if (isString(seq)) {
                if (!isString(elem)) {
                    RAISE_ERROR(
                        "Left operand of 'in' on a string must be a string.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* haystack = asObjString(as<Obj*>(seq));
                auto* needle = asObjString(as<Obj*>(elem));
                bool found = haystack->chars.find(needle->chars.data(), 0,
                                                  needle->chars.size()) !=
                             LoxString::npos;
                m_rt.push(from<bool>(found));
            } else if (isMap(seq)) {
                if (is<Number>(elem) && std::isnan(as<Number>(elem))) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "NaNKeyError", "NaN cannot be used as a map key."));
                    break;
                }
                if (is<Obj*>(elem) && as<Obj*>(elem)->type != ObjType::STRING) {
                    CATCHABLE_OR_RETURN(tryCatchableError(
                        "InvalidMapKeyError",
                        "Map keys must be Bool, Number, Nil, or String."));
                    break;
                }
                auto* map = asObjMap(as<Obj*>(seq));
                Value dummy;
                m_rt.push(from<bool>(map->mapGet(elem, dummy)));
            } else {
                RAISE_ERROR(
                    "Right operand of 'in' must be a list, string, or map.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::GET_ITER: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opGetIter())) {
                return *ret;
            }
            break;
        }
        case Op::ITER_HAS_NEXT: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opIterHasNext())) {
                return *ret;
            }
            break;
        }
        case Op::ITER_NEXT: {
            frame->ip = ip;
            if (auto ret = dispatchOp(m_rt.opIterNext())) {
                return *ret;
            }
            break;
        }
        case Op::PUSH_HANDLER: {
            uint16_t catchOffset = readShort();
            // catchOffset is relative to the current IP, just like JUMP.
            // ip points to the first byte after the PUSH_HANDLER instruction.
            Chunk::const_iterator catchIp = ip + catchOffset;
            m_rt.m_handlerStack.push_back(
                HandlerRecord{m_rt.m_frameCount, m_rt.stackTop, catchIp});
            break;
        }
        case Op::POP_HANDLER: {
            if (m_rt.m_handlerStack.empty()) {
                RAISE_ERROR("BUG: POP_HANDLER with empty handler stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            m_rt.m_handlerStack.pop_back();
            break;
        }
        case Op::DEFER_RECORD: {
            uint8_t argc = readByte();
            // Pop callee and arguments from stack. Create an ObjDeferredCall
            // object that captures them, and store it on the defer list.
            Value callee = m_rt.stackTop[-(argc + 1)];
            ObjDeferredCall* deferred = m_rt.m_mm.create<ObjDeferredCall>(
                callee, VmAllocator<Value>{&m_rt.m_mm});
            m_rt.m_mm.pushTempRoot(deferred);
            for (int i = argc - 1; i >= 0; i--) {
                deferred->args.push_back(m_rt.stackTop[-(i + 1)]);
            }
            m_rt.m_mm.popTempRoot();
            // Pop arguments and callee from stack.
            m_rt.stackTop -= argc + 1;
            // Add to defer list for the current frame.
            m_rt.m_deferLists[m_rt.m_frameCount - 1].push_back(
                Value{static_cast<Obj*>(deferred)});
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
        case Op::THROW: {
            Value thrownValue = m_rt.pop();
            // Use the shared unwind implementation (same as runtime faults).
            // Sync frame->ip before calling handleThrow for error reporting.
            frame->ip = ip;
            Runtime::ThrowOutcome outcome =
                m_rt.handleThrow(thrownValue, stopAtFrameCount);
            if (outcome == Runtime::ThrowOutcome::Uncaught) {
                // No handler found, runtimeError was called, and the stack
                // was reset.
                return InterpretResult::RUNTIME_ERROR;
            }
            if (outcome == Runtime::ThrowOutcome::HandledStop) {
                // Handled, but by a handler outside THIS run() invocation's
                // own frame range (possibly several reentrant handleThrow
                // calls down — see ThrowOutcome's doc comment in runtime.h).
                // m_frameCount may even be 0 here (the resolution ran the
                // rest of the program to completion) — do NOT touch
                // frame/ip/chunk; FrameSync::loadTop would read out of
                // bounds. Hand control back to whichever context started
                // this run() invocation.
                return InterpretResult::OK;
            }
            // HandledContinue: a handler was found and set up, and this
            // run() invocation's own frame context is still live. Reload
            // frame/ip/chunk from the new top and continue dispatch.
            FrameSync::loadTop(m_rt.m_frames, m_rt.m_frameCount, frame, ip,
                               chunk);
            break;
        }
        }
    }

#undef BINARY_OP
#undef CATCHABLE_OR_RETURN
#undef RAISE_ERROR
}
