#include "runtime.h"
#include "objects.h"
#include "object.h"
#include "utility.h"

#include "stdlib/stdlib_registrar.h"
#include "stdlib/globals.h"
#include "stdlib/file_api.h"
#include "stdlib/map_api.h"
#include "stdlib/error_api.h"
#include "stdlib/math_module.h"
#include "stdlib/os_api.h"
#include "stdlib/reflect_api.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>

std::optional<Value> Runtime::getGlobal(const std::string& name) const {
    ObjString* key = m_mm.findString(name);
    if (!key) {
        return std::nullopt;
    }
    Value out;
    if (!m_globals.get(key, out)) {
        return std::nullopt;
    }
    return out;
}

Runtime::CallOutcome Runtime::call(ObjClosure* closure, int argCount,
                                   int stopAtFrameCount) {
    ObjFunction* fn = closure->function;
    if (argCount != fn->arity) {
        // Arity mismatch is now catchable as ArityError, but only if a handler
        // is active. If no handler is active, fall back to uncaught error.
        // The arity check happens before any new frame is pushed, so it never
        // needs the frame reserve StackOverflowError's own guard below relies
        // on.
        char msg[256];
        snprintf(msg, sizeof(msg), "Expected %d arguments but got %d.",
                 fn->arity, argCount);
        if (!m_handlerStack.empty()) {
            // stopAtFrameCount must be the boundary of whichever run()
            // invocation is actually calling us (threaded down from there,
            // not always 0) — see ThrowOutcome's doc comment in runtime.h.
            // Using the wrong boundary here is exactly the class of bug that
            // let a caught fault corrupt an unrelated, more-nested run()
            // invocation's own frame bookkeeping.
            ThrowOutcome outcome =
                raiseThrowableError("ArityError", msg, stopAtFrameCount);
            if (outcome == ThrowOutcome::HandledContinue) {
                return CallOutcome::CaughtContinue;
            }
            if (outcome == ThrowOutcome::HandledStop) {
                return CallOutcome::CaughtStop;
            }
        }
        // No handler found, or error class not ready; uncaught error.
        runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
        return CallOutcome::Uncaught;
    }
    // Fires at FRAMES_MAX itself — the same threshold whether or not a
    // handler is active — so an open try/catch never changes how deep a
    // program that does not overflow can go (see
    // STACK_OVERFLOW_FRAME_RESERVE's own comment in runtime.h). Only when a
    // handler is active does hitting it raise a catchable StackOverflowError
    // instead of going straight to the hard ceiling below: that unwind
    // (closing upvalues, draining each discarded frame's own defers,
    // building the Error) spends the reserve capacity held above FRAMES_MAX.
    // This must be `>=`, not `==`: m_frameCount only increases, so a try
    // opened after it has already passed FRAMES_MAX would never see an exact
    // match again.
    if (m_frameCount >= FRAMES_MAX && !m_handlerStack.empty() &&
        !m_unwindingStackOverflow) {
        // See m_unwindingStackOverflow's own comment (runtime.h) and this
        // guard's own matching one in push(): a deferred call drained by the
        // handleThrow() below can itself reach this same guard again. Hold
        // the flag for exactly this call, so a nested hit falls through to
        // the hard ceiling below instead of recursing.
        m_unwindingStackOverflow = true;
        ThrowOutcome outcome = raiseThrowableError(
            "StackOverflowError", "Stack overflow.", stopAtFrameCount);
        m_unwindingStackOverflow = false;
        if (outcome == ThrowOutcome::HandledContinue) {
            return CallOutcome::CaughtContinue;
        }
        if (outcome == ThrowOutcome::HandledStop) {
            return CallOutcome::CaughtStop;
        }
        // Uncaught: raiseThrowableError() already reported it. m_frameCount
        // is reset to 0 by then (see runtimeError()/resetStack()), so
        // continuing to push a frame below would be reading a torn-down VM.
        return CallOutcome::Uncaught;
    }
    // No handler active: the true hard ceiling, unaffected by the reserve,
    // same depth as before the reserve existed. While unwinding a
    // StackOverflowError (m_unwindingStackOverflow), the ceiling moves out to
    // FRAMES_MAX + STACK_OVERFLOW_FRAME_RESERVE — the physical capacity
    // m_frames[] actually has — so a deferred call drained during that
    // unwind can use the reserve; past that, it stays fatal rather than
    // recursing into the same handler again.
    if (m_frameCount >=
        FRAMES_MAX +
            (m_unwindingStackOverflow ? STACK_OVERFLOW_FRAME_RESERVE : 0)) {
        runtimeError("Stack overflow.");
        return CallOutcome::Uncaught;
    }
    CallFrame* frame = &m_frames[m_frameCount++];
    frame->closure = closure;
    frame->ip = fn->chunk.cbegin();
    frame->slots = stackTop - argCount - 1;
#ifdef LOXPP_PROFILE
    {
        int depth = m_frameCount - 1;
        ObjClosure* parent =
            (depth > 0) ? m_frames[depth - 1].closure : nullptr;
        m_profilerScopes[depth].emplace(m_profilerData, closure, depth, parent);
    }
#endif
    return CallOutcome::Pushed;
}

ObjUpvalue* Runtime::captureUpvalue(Value* local) {
    ObjUpvalue* prev = nullptr;
    ObjUpvalue* cur = m_openUpvalues;
    while (cur != nullptr && cur->location > local) {
        prev = cur;
        cur = cur->next;
    }
    if (cur != nullptr && cur->location == local) {
        return cur;
    }
    ObjUpvalue* uv = m_mm.create<ObjUpvalue>(local);
    uv->next = cur;
    if (prev == nullptr) {
        m_openUpvalues = uv;
    } else {
        prev->next = uv;
    }
    return uv;
}

void Runtime::closeUpvalues(Value* last) {
    while (m_openUpvalues != nullptr && m_openUpvalues->location >= last) {
        ObjUpvalue* uv = m_openUpvalues;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        m_openUpvalues = uv->next;
    }
}

void Runtime::popHandlersOwnedByCurrentFrame() {
    // m_frameCount is still the depth of the frame being left here — see
    // INVARIANT(handler-stack-frame-scoped) on m_handlerStack's declaration
    // (runtime.h). Nested protected regions opened by this same frame all
    // share that depth, so the loop clears every one of them and stops at
    // the first record belonging to an ancestor frame.
    while (!m_handlerStack.empty() &&
           m_handlerStack.back().frameCount == m_frameCount) {
        m_handlerStack.pop_back();
    }
}

InterpretResult Runtime::runPendingDefers(int frameIndex,
                                          int stopAtFrameCount) {
    // Run every deferred call for m_frames[frameIndex], LIFO (most recently
    // recorded first). Each one runs to completion — via a nested run() that
    // stops once m_frameCount returns to frameIndex + 1 — before the next
    // one starts: popping every frame up front and letting the ordinary
    // dispatch loop replay them would run them in declaration order instead
    // of LIFO, and would leave call()'s later pushes silently discarding the
    // stack effects of earlier ones.
    auto& deferList = m_deferLists[frameIndex];
    while (!deferList.empty()) {
        Value deferValue = deferList.back();
        deferList.pop_back();
        if (!isDeferredCall(deferValue)) {
            continue;
        }
        ObjDeferredCall* deferred = asObjDeferredCall(as<Obj*>(deferValue));
        // call()'s own convention (and DEFER_RECORD's documented "stack
        // before" shape, chunk.h) is [callee, arg0, ..., argN-1] — the
        // callee goes first, underneath its arguments, not last.
        Value calleeVal = deferred->callable;
        push(calleeVal);
        for (const Value& arg : deferred->args) {
            push(arg);
        }
        int argCount = static_cast<int>(deferred->args.size());

        // Handle various callable types (similar to Op::CALL dispatch).
        // For BoundMethod, replace the method on the stack with the receiver,
        // then call the underlying method closure.
        CallOutcome outcome;
        if (isBoundMethod(calleeVal)) {
            ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(calleeVal));
            stackTop[-argCount - 1] = bound->receiver;
            outcome = call(bound->method, argCount, stopAtFrameCount);
        } else if (isClosure(calleeVal)) {
            ObjClosure* closure = asObjClosure(as<Obj*>(calleeVal));
            outcome = call(closure, argCount, stopAtFrameCount);
        } else if (isNative(calleeVal)) {
            ObjNative* native = asObjNative(as<Obj*>(calleeVal));
            outcome = callNative(native, argCount) ? CallOutcome::Pushed
                                                   : CallOutcome::Uncaught;
        } else if (isBoundNative(calleeVal)) {
            ObjBoundNative* bound = asObjBoundNative(as<Obj*>(calleeVal));
            outcome = callBoundNative(bound, argCount) ? CallOutcome::Pushed
                                                       : CallOutcome::Uncaught;
        } else {
            // Unexpected callable type in deferred call
            runtimeError("Deferred callable has unexpected type.");
            return InterpretResult::RUNTIME_ERROR;
        }

        if (outcome == CallOutcome::Uncaught) {
            return InterpretResult::RUNTIME_ERROR;
        }
        if (outcome != CallOutcome::Pushed) {
            // The deferred call's own arity mismatch was caught instead of
            // pushing a new frame. Any handler reachable here was pushed
            // before frameIndex's own function was even called (a handler
            // scoped inside that function's body is already popped by the
            // time RUN_DEFERS runs), so frameIndex no longer exists — there
            // is no new frame to run to completion. Per defer step 5's
            // documented limitation, abandon any remaining sibling defers
            // rather than still running them.
            return InterpretResult::OK;
        }
        InterpretResult result = m_runLoop(frameIndex + 1);
        if (result != InterpretResult::OK) {
            return result;
        }
        if (m_frameCount != frameIndex + 1) {
            // The deferred call's own throw propagated past this frame (it
            // no longer exists — an outer handler or program exit already
            // took over dispatch). Abandon the rest of this list; the
            // caller must notice m_frameCount changed and stop too.
            return InterpretResult::OK;
        }
    }
    return InterpretResult::OK;
}

Runtime::ThrowOutcome Runtime::handleThrow(Value thrownValue,
                                           int stopAtFrameCount) {
    // Unwind frame-by-frame, running each frame's pending defers, regardless
    // of whether a handler will ultimately be found. This unifies the search
    // and the defer-draining (spec/04-semantics.md throw Statement step 5,
    // defer Statement step 4). This is the ONE unwind implementation for both
    // explicit throw (Op::THROW) and runtime faults (IndexOutOfBoundsError,
    // etc.). See ThrowOutcome's doc comment in runtime.h for what each of the
    // three results means and obligates the caller to do.

    // handleThrow() owns thrownValue for as long as it runs. markRoots()
    // marks only [stack, stackTop), and Step 2 below moves stackTop to each
    // discarded frame's own base *before* running that frame's defers, so a
    // defer that allocates (a nested, real call — runPendingDefers() ->
    // call() -> run()) can trigger a collection while thrownValue is off the
    // stack entirely. Root it here, once, for both callers (Op::THROW and
    // raiseThrowableError()), rather than at each call site.
    struct ThrownValueGuard {
        MemoryManager& mm;
        bool rooted;
        ThrownValueGuard(MemoryManager& mm, Value v)
            : mm(mm), rooted(isObj(v)) {
            if (rooted) {
                mm.pushTempRoot(asObj(v));
            }
        }
        ~ThrownValueGuard() {
            if (rooted) {
                mm.popTempRoot();
            }
        }
    } thrownValueGuard(m_mm, thrownValue);

    // Step 1: Find if any live handler exists (without popping it yet).
    bool foundHandler = false;
    HandlerRecord handlerToUse;
    int handlerIndex = -1;
    for (int i = (int)m_handlerStack.size() - 1; i >= 0; i--) {
        HandlerRecord handler = m_handlerStack[i];
        if (handler.frameCount <= m_frameCount) {
            // Found a live handler (innermost one, since we iterate LIFO).
            foundHandler = true;
            handlerToUse = handler;
            handlerIndex = i;
            break;
        }
    }

    // Step 2: Unwind frame-by-frame, draining defers, to either the
    // handler's frame (if found) or frame 0 (if not found).
    int targetFrameCount = foundHandler ? handlerToUse.frameCount : 0;
    while (m_frameCount > targetFrameCount) {
        int unwoundFrameIndex = m_frameCount - 1;
        closeUpvalues(m_frames[unwoundFrameIndex].slots);
        // Reclaim this frame's own window before running its defers, not
        // only at the end of the whole unwind (step 3 below). A deferred
        // call's own args are already captured on ObjDeferredCall, not read
        // off this frame's live slots, so nothing here needs them once
        // closeUpvalues has run. Without this, an unwind through many frames
        // (each with its own pending defer) leaves every one of those
        // frames' operands live on the value stack at once while the defers
        // run, so the STACK_OVERFLOW_STACK_RESERVE reserve — sized for one
        // frame's own defer call — is exhausted by the second or third
        // frame instead of lasting the whole unwind.
        stackTop = m_frames[unwoundFrameIndex].slots;
        // stopAtFrameCount is OUR OWN parameter, not unwoundFrameIndex: it
        // is the boundary of whichever run() invocation is unwinding right
        // now (see runtime.h), and a reentrant fault inside this defer must
        // be judged against that same boundary, not a fresh default.
        InterpretResult result =
            runPendingDefers(unwoundFrameIndex, stopAtFrameCount);
        if (result != InterpretResult::OK) {
            // Hard error during defer, already reported.
            return ThrowOutcome::Uncaught;
        }
        if (m_frameCount != unwoundFrameIndex + 1) {
            // Deferred call threw (or its own arity mismatch was caught);
            // that inner dispatch already fully resolved things — possibly by
            // running the rest of the program to completion, which can leave
            // m_frameCount at 0. Our own unwind has nothing left to finish:
            // whether OUR caller must also stop depends on where m_frameCount
            // landed relative to OUR OWN stopAtFrameCount.
            return (m_frameCount <= stopAtFrameCount)
                       ? ThrowOutcome::HandledStop
                       : ThrowOutcome::HandledContinue;
        }
#ifdef LOXPP_PROFILE
        m_profilerScopes[m_frameCount - 1].reset();
#endif
        m_frameCount--;
    }

    // Step 3: After unwinding is complete, decide what to do.
    if (foundHandler) {
        // The handler's own frame survives, so step 2 closed nothing in
        // it. Close upvalues for the dropped region first: the catch block
        // reuses those slots, and an open upvalue would alias the new
        // content. The checkpoint bounds the region from below.
        closeUpvalues(handlerToUse.stackTop);
        // Truncate stack to checkpoint and push thrown value.
        stackTop = handlerToUse.stackTop;
        push(thrownValue);
        // Set IP to catch block in the frame record directly.
        m_frames[m_frameCount - 1].ip = handlerToUse.catchIp;
        // Pop this handler since we're handling the throw.
        m_handlerStack.erase(m_handlerStack.begin() + handlerIndex);
        return (m_frameCount <= stopAtFrameCount)
                   ? ThrowOutcome::HandledStop
                   : ThrowOutcome::HandledContinue;
    }

    // No handler found — report uncaught error (after defers have run).
    if (isError(thrownValue)) {
        ObjError* err = asObjError(as<Obj*>(thrownValue));
        runtimeError("%s", err->message->chars.c_str());
    } else {
        std::string thrownStr = stringify(thrownValue);
        runtimeError("%s", thrownStr.c_str());
    }
    return ThrowOutcome::Uncaught;
}

Runtime::ThrowOutcome Runtime::raiseThrowableError(const char* kind_str,
                                                   const char* msg,
                                                   int stopAtFrameCount) {
    // Shared implementation for raising a catchable runtime error. Used by
    // both run()'s tryCatchableError and by call()'s arity check, and now by
    // the op*() helpers below. Constructs an Error instance with the given
    // kind and message, then calls handleThrow to search for a handler. See
    // ThrowOutcome's doc comment in runtime.h.

    if (m_errorClass == nullptr) {
        // Error class not ready; fallback to uncaught error
        runtimeError("%s", msg);
        return ThrowOutcome::Uncaught;
    }

    // GC safety: root msg_obj/kind_obj only for the window before err_obj
    // holds them. Once err_obj exists, its own traceObject marks both, so
    // their liveness after that rides on err_obj staying reachable —
    // guaranteed for the whole call below by handleThrow()'s own root on
    // thrownValue (see its comment), not by anything here.
    ObjString* msg_obj = m_mm.makeString(msg);
    m_mm.pushTempRoot(msg_obj);

    ObjString* kind_obj = m_mm.makeString(kind_str);
    m_mm.pushTempRoot(kind_obj);

    ObjError* err_obj = m_mm.create<ObjError>(m_errorClass, msg_obj, kind_obj);

    m_mm.popTempRoot(); // Unroot kind_obj
    m_mm.popTempRoot(); // Unroot msg_obj

    return handleThrow(Value{static_cast<Obj*>(err_obj)}, stopAtFrameCount);
}

bool Runtime::callNative(ObjNative* native, int argCount) {
    if (native->arity != -1 && argCount != native->arity) {
        runtimeError("Expected %d arguments but got %d.", native->arity,
                     argCount);
        return false;
    }
    m_stdlibCtx.clearError();
    Value result = native->function(argCount, stackTop - argCount);
    if (m_stdlibCtx.nativeError) {
        runtimeError("%s", m_stdlibCtx.nativeErrorMsg.c_str());
        return false;
    }
    stackTop -= argCount + 1; // pop args + callee
    push(result);
    return true;
}

bool Runtime::callBoundNative(ObjBoundNative* bn, int argCount) {
    ObjNative* fn = bn->native;             // read before the slot changes
    stackTop[-argCount - 1] = bn->receiver; // natives read args[-1]
    return callNative(fn, argCount);
}

bool Runtime::bindMethod(ObjClass* klass, ObjString* name) {
    Value method;
    if (!klass->methods.get(name, method)) {
        runtimeError("Undefined property '%s'.", name->chars.c_str());
        return false;
    }
    ObjBoundMethod* bound =
        m_mm.create<ObjBoundMethod>(peek(0), asObjClosure(as<Obj*>(method)));
    pop(); // instance
    push(Value{static_cast<Obj*>(bound)});
    return true;
}

void Runtime::defineNatives() {
    StdlibRegistrar reg(m_mm, m_globals);
    registerGlobals(reg);
    m_fileClass = registerFileAPI(reg);
    m_mapClass = registerMapAPI(reg);
    m_errorClass = registerErrorAPI(reg);
    registerMath(reg);
    registerOSAPI(reg, m_mapClass);
    registerReflectAPI(reg);
}

void Runtime::runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputs("\n", stderr);

    // Print a stack trace (innermost frame first).
    for (int i = m_frameCount - 1; i >= 0; i--) {
        const CallFrame& frame = m_frames[i];
        ObjFunction* fn = frame.closure->function;
        const Chunk& chunk = fn->chunk;
        auto offset = static_cast<int>(frame.ip - chunk.cbegin()) - 1;
        int line = chunk.getLine(offset);
        std::fprintf(stderr, "[line %d] in ", line);
        if (fn->name == nullptr) {
            std::fprintf(stderr, "script\n");
        } else {
            std::fprintf(stderr, "%s()\n", fn->name->chars.c_str());
        }
    }

    resetStack();
}

void Runtime::markRoots() {
    for (Value* slot = stack; slot < stackTop; ++slot) {
        m_mm.markValue(*slot);
    }
    for (int i = 0; i < m_frameCount; ++i) {
        m_mm.markObject(m_frames[i].closure);
        // Mark pending deferred calls in this frame.
        for (const Value& defer : m_deferLists[i]) {
            m_mm.markValue(defer);
        }
    }
    for (ObjUpvalue* uv = m_openUpvalues; uv != nullptr; uv = uv->next) {
        m_mm.markObject(uv);
    }
    m_globals.forEach([this](ObjString* key, Value val) {
        m_mm.markObject(key);
        m_mm.markValue(val);
    });
    if (m_fileClass) {
        m_mm.markObject(m_fileClass);
    }
    if (m_mapClass) {
        m_mm.markObject(m_mapClass);
    }
    if (m_errorClass) {
        m_mm.markObject(m_errorClass);
    }
}

void Runtime::resetStack() {
    stackTop = stack;
    m_frameCount = 0;
    m_stackOverflow = false;
    m_handlerStack.clear();
    for (auto& deferList : m_deferLists) {
        deferList.clear();
    }
}

// --- op*() opcode helpers (Layer 1) -------------------------------------

Runtime::OpResult Runtime::opCall(int argCount, int stopAtFrameCount) {
    Value callee = peek(argCount);
    if (isNative(callee)) {
        return callNative(asObjNative(callee), argCount) ? OpResult::OK
                                                         : OpResult::Fatal;
    }
    if (isClosure(callee)) {
        return fromCall(call(asObjClosure(callee), argCount, stopAtFrameCount));
    }
    if (isBoundMethod(callee)) {
        ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(callee));
        // Slot 0 of the new frame = receiver (= this).
        stackTop[-argCount - 1] = bound->receiver;
        return fromCall(call(bound->method, argCount, stopAtFrameCount));
    }
    if (isBoundNative(callee)) {
        ObjBoundNative* bn = asObjBoundNative(as<Obj*>(callee));
        return callBoundNative(bn, argCount) ? OpResult::OK : OpResult::Fatal;
    }
    if (isClass(callee)) {
        ObjClass* klass = asObjClass(as<Obj*>(callee));
        ObjInstance* instance =
            m_mm.create<ObjInstance>(klass, VmAllocator<Entry>{&m_mm});
        stackTop[-argCount - 1] = Value{static_cast<Obj*>(instance)};
        // Call init() if the class defines one.
        ObjString* initStr = m_mm.findString("init");
        Value initMethod;
        if (initStr && klass->methods.get(initStr, initMethod)) {
            return fromCall(call(asObjClosure(as<Obj*>(initMethod)), argCount,
                                 stopAtFrameCount));
        }
        if (argCount != 0) {
            return fromThrow(raiseThrowableError(
                "ConstructorArityError", "Expected 0 arguments but got some.",
                stopAtFrameCount));
        }
        return OpResult::OK;
    }
    if (isEnumCtor(callee)) {
        ObjEnumCtor* ctor = asObjEnumCtor(as<Obj*>(callee));
        if (argCount != static_cast<int>(ctor->arity)) {
            return fromThrow(raiseThrowableError(
                "ConstructorArityError", "Constructor called with wrong arity.",
                stopAtFrameCount));
        }
        ObjEnum* enumVal =
            m_mm.create<ObjEnum>(ctor, VmAllocator<Value>{&m_mm});
        m_mm.pushTempRoot(enumVal);
        enumVal->fields.resize(static_cast<size_t>(argCount));
        for (int i = argCount - 1; i >= 0; i--) {
            enumVal->fields[static_cast<size_t>(i)] = pop();
        }
        m_mm.popTempRoot();
        pop(); // pop the ObjEnumCtor from the callee slot
        push(Value{static_cast<Obj*>(enumVal)});
        return OpResult::OK;
    }
    return fromThrow(raiseThrowableError(
        "NotCallableError", "Can only call functions, classes and enums.",
        stopAtFrameCount));
}

Runtime::OpResult Runtime::opInvoke(ObjString* name, int argCount,
                                    int stopAtFrameCount) {
    Value receiver = peek(argCount);
    if (isInstance(receiver)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(receiver));
        // A field can shadow a method — check fields first.
        Value fieldVal;
        if (instance->fields.get(name, fieldVal)) {
            stackTop[-argCount - 1] = fieldVal;
            if (isClosure(fieldVal)) {
                return fromCall(call(asObjClosure(as<Obj*>(fieldVal)), argCount,
                                     stopAtFrameCount));
            }
            if (isNative(fieldVal)) {
                return callNative(asObjNative(as<Obj*>(fieldVal)), argCount)
                           ? OpResult::OK
                           : OpResult::Fatal;
            }
            if (isBoundNative(fieldVal)) {
                ObjBoundNative* bn = asObjBoundNative(as<Obj*>(fieldVal));
                return callBoundNative(bn, argCount) ? OpResult::OK
                                                     : OpResult::Fatal;
            }
            runtimeError("Can only call functions, classes and enums.");
            return OpResult::Fatal;
        }
        // Fast path: call the method directly — receiver already sits at
        // stackTop[-argCount-1], which becomes slot 0 (= this) of the new
        // frame.
        Value method;
        if (!instance->klass->methods.get(name, method)) {
            runtimeError("Undefined property '%s'.", name->chars.c_str());
            return OpResult::Fatal;
        }
        Obj* methodObj = as<Obj*>(method);
        if (isObjNative(methodObj)) {
            return callNative(asObjNative(methodObj), argCount)
                       ? OpResult::OK
                       : OpResult::Fatal;
        }
        return fromCall(
            call(asObjClosure(methodObj), argCount, stopAtFrameCount));
    }
    if (isList(receiver)) {
        ObjList* list = asObjList(as<Obj*>(receiver));
        if (name->chars == "append") {
            if (argCount != 1) {
                runtimeError("'append' expects 1 argument but got %d.",
                             argCount);
                return OpResult::Fatal;
            }
            Value val = peek(0); // still on stack — GC-safe during push_back
            list->elements.push_back(val);
            pop(); // arg
            pop(); // receiver
            push(from<Nil>(Nil{}));
            return OpResult::OK;
        }
        if (name->chars == "pop") {
            if (argCount != 0) {
                runtimeError("'pop' expects 0 arguments but got %d.", argCount);
                return OpResult::Fatal;
            }
            if (list->elements.empty()) {
                return fromThrow(raiseThrowableError(
                    "EmptyListError", "Cannot pop from an empty list.",
                    stopAtFrameCount));
            }
            Value val = list->elements.back();
            list->elements.pop_back();
            pop(); // receiver
            push(val);
            return OpResult::OK;
        }
        if (name->chars == "remove") {
            if (argCount != 1) {
                runtimeError("'remove' expects 1 argument but got %d.",
                             argCount);
                return OpResult::Fatal;
            }
            Value target = peek(0);
            auto& elems = list->elements;
            bool found = false;
            for (int i = 0; i < static_cast<int>(elems.size()); i++) {
                if (elems[i] == target) {
                    elems.erase(elems.begin() + i);
                    found = true;
                    break;
                }
            }
            if (!found) {
                runtimeError("Value not found in list.");
                return OpResult::Fatal;
            }
            pop(); // arg
            pop(); // receiver
            push(from<Nil>(Nil{}));
            return OpResult::OK;
        }
        runtimeError("Undefined method '%s' on list.", name->chars.c_str());
        return OpResult::Fatal;
    }
    if (isFile(receiver)) {
        Value method;
        if (!m_fileClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on file.", name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount)
                   ? OpResult::OK
                   : OpResult::Fatal;
    }
    if (isMap(receiver)) {
        Value method;
        if (!m_mapClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on map.", name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount)
                   ? OpResult::OK
                   : OpResult::Fatal;
    }
    return fromThrow(raiseThrowableError("InvalidReceiverError",
                                         "Method called on invalid receiver.",
                                         stopAtFrameCount));
}

Runtime::OpResult Runtime::opGetProperty(ObjString* name,
                                         int stopAtFrameCount) {
    if (isError(peek(0))) {
        ObjError* err = asObjError(as<Obj*>(peek(0)));
        pop(); // error
        if (name->chars == "message") {
            push(Value{static_cast<Obj*>(err->message)});
        } else if (name->chars == "kind") {
            push(Value{static_cast<Obj*>(err->kind)});
        } else {
            return fromThrow(raiseThrowableError("UndefinedPropertyError",
                                                 "Undefined property on error.",
                                                 stopAtFrameCount));
        }
        return OpResult::OK;
    }
    if (isFile(peek(0))) {
        Value method;
        if (!m_fileClass->methods.get(name, method)) {
            runtimeError("Undefined property '%s' on file.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        ObjBoundNative* bound =
            m_mm.create<ObjBoundNative>(peek(0), asObjNative(as<Obj*>(method)));
        pop(); // file
        push(Value{static_cast<Obj*>(bound)});
        return OpResult::OK;
    }
    if (isMap(peek(0))) {
        Value method;
        if (!m_mapClass->methods.get(name, method)) {
            runtimeError("Undefined property '%s' on map.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        ObjBoundNative* bound =
            m_mm.create<ObjBoundNative>(peek(0), asObjNative(as<Obj*>(method)));
        pop(); // map
        push(Value{static_cast<Obj*>(bound)});
        return OpResult::OK;
    }
    if (!isInstance(peek(0))) {
        runtimeError("Only instances have properties.");
        return OpResult::Fatal;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(peek(0)));
    Value value;
    if (instance->fields.get(name, value)) {
        pop(); // instance
        push(value);
        return OpResult::OK;
    }
    if (!bindMethod(instance->klass, name)) {
        return OpResult::Fatal;
    }
    return OpResult::OK;
}

Runtime::OpResult Runtime::opGetSuper(ObjString* name) {
    ObjClass* superclass = asObjClass(as<Obj*>(pop()));
    if (!bindMethod(superclass, name)) {
        return OpResult::Fatal;
    }
    return OpResult::OK;
}

Runtime::OpResult Runtime::opSuperInvoke(ObjString* name, int argCount,
                                         int stopAtFrameCount) {
    ObjClass* superclass = asObjClass(as<Obj*>(pop()));
    Value method;
    if (!superclass->methods.get(name, method)) {
        runtimeError("Undefined property '%s'.", name->chars.c_str());
        return OpResult::Fatal;
    }
    return fromCall(
        call(asObjClosure(as<Obj*>(method)), argCount, stopAtFrameCount));
}

Runtime::OpResult Runtime::opInherit() {
    Value superVal = peek(1);
    if (!isClass(superVal)) {
        runtimeError("Superclass must be a class.");
        return OpResult::Fatal;
    }
    ObjClass* superclass = asObjClass(as<Obj*>(superVal));
    ObjClass* subclass = asObjClass(as<Obj*>(peek(0)));
    subclass->methods.addAll(superclass->methods);
    subclass->superclass = superclass;
    pop(); // pop subclass; superclass stays as "super" local
    return OpResult::OK;
}

Runtime::OpResult Runtime::opGetIndex(int stopAtFrameCount) {
    Value indexVal = pop();
    Value collectionVal = pop();
    if (isList(collectionVal)) {
        if (!is<Number>(indexVal)) {
            return fromThrow(raiseThrowableError("IndexTypeError",
                                                 "List index must be a number.",
                                                 stopAtFrameCount));
        }
        double n = as<Number>(indexVal);
        if (n != std::floor(n)) {
            return fromThrow(raiseThrowableError(
                "IndexNotIntegerError", "List index must be an integer.",
                stopAtFrameCount));
        }
        auto* list = asObjList(as<Obj*>(collectionVal));
        int idx = static_cast<int>(n);
        if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
            return fromThrow(raiseThrowableError("IndexOutOfBoundsError",
                                                 "List index out of bounds.",
                                                 stopAtFrameCount));
        }
        push(list->elements[idx]);
        return OpResult::OK;
    }
    if (isString(collectionVal)) {
        if (!is<Number>(indexVal)) {
            return fromThrow(raiseThrowableError(
                "IndexTypeError", "String index must be a number.",
                stopAtFrameCount));
        }
        double n = as<Number>(indexVal);
        if (n != std::floor(n)) {
            return fromThrow(raiseThrowableError(
                "IndexNotIntegerError", "String index must be an integer.",
                stopAtFrameCount));
        }
        auto* str = asObjString(as<Obj*>(collectionVal));
        int idx = static_cast<int>(n);
        if (idx < 0 || idx >= static_cast<int>(str->chars.size())) {
            return fromThrow(raiseThrowableError("IndexOutOfBoundsError",
                                                 "String index out of bounds.",
                                                 stopAtFrameCount));
        }
        // Copy char before makeString (GC-safe: same pattern as ADD)
        char ch = str->chars[idx];
        push(Value{
            static_cast<Obj*>(m_mm.makeString(std::string_view{&ch, 1}))});
        return OpResult::OK;
    }
    if (isMap(collectionVal)) {
        if (is<Number>(indexVal) && std::isnan(as<Number>(indexVal))) {
            return fromThrow(raiseThrowableError(
                "NaNKeyError", "NaN cannot be used as a map key.",
                stopAtFrameCount));
        }
        if (is<Obj*>(indexVal) && as<Obj*>(indexVal)->type != ObjType::STRING) {
            return fromThrow(raiseThrowableError(
                "InvalidMapKeyError",
                "Map keys must be Bool, Number, Nil, or String.",
                stopAtFrameCount));
        }
        auto* map = asObjMap(as<Obj*>(collectionVal));
        Value result{Nil{}}; // default nil — returned when key absent
        map->mapGet(indexVal, result);
        push(result);
        return OpResult::OK;
    }
    if (isEnumValue(collectionVal)) {
        if (!is<Number>(indexVal)) {
            runtimeError("Enum field index must be a number.");
            return OpResult::Fatal;
        }
        double n = as<Number>(indexVal);
        auto* e = asObjEnum(as<Obj*>(collectionVal));
        // NaN, +-infinity, and a magnitude outside int's range have no
        // well-defined static_cast<int> result (UB) and can never be a valid
        // field index either way, so report them out of range directly from
        // the Value's own text instead of casting first.
        if (!std::isfinite(n) ||
            n < static_cast<double>(std::numeric_limits<int>::min()) ||
            n > static_cast<double>(std::numeric_limits<int>::max())) {
            runtimeError("Enum field index %s out of range.",
                         stringify(indexVal).c_str());
            return OpResult::Fatal;
        }
        int idx = static_cast<int>(n);
        if (idx < 0 || idx >= static_cast<int>(e->fields.size())) {
            runtimeError("Enum field index %d out of range.", idx);
            return OpResult::Fatal;
        }
        push(e->fields[static_cast<size_t>(idx)]);
        return OpResult::OK;
    }
    return fromThrow(raiseThrowableError(
        "NotIndexableError", "Only lists, strings, and maps can be indexed.",
        stopAtFrameCount));
}

Runtime::OpResult Runtime::opSetIndex(int stopAtFrameCount) {
    Value val = pop();
    Value indexVal = pop();
    Value listVal = pop();
    if (isString(listVal)) {
        runtimeError(
            "Strings are immutable and cannot be indexed for assignment.");
        return OpResult::Fatal;
    }
    if (isMap(listVal)) {
        if (is<Number>(indexVal) && std::isnan(as<Number>(indexVal))) {
            return fromThrow(raiseThrowableError(
                "NaNKeyError", "NaN cannot be used as a map key.",
                stopAtFrameCount));
        }
        if (is<Obj*>(indexVal) && as<Obj*>(indexVal)->type != ObjType::STRING) {
            return fromThrow(raiseThrowableError(
                "InvalidMapKeyError",
                "Map keys must be Bool, Number, Nil, or String.",
                stopAtFrameCount));
        }
        auto* map = asObjMap(as<Obj*>(listVal));
        // Root the map: it was popped and may be a temporary; mapSet can
        // grow the bucket array which triggers GC.
        m_mm.pushTempRoot(map);
        // Root val if it's an object: it was popped off the stack before
        // mapSet, so the GC won't find it through the stack.
        if (is<Obj*>(val)) {
            m_mm.pushTempRoot(as<Obj*>(val));
        }
        map->mapSet(indexVal, val);
        if (is<Obj*>(val)) {
            m_mm.popTempRoot();
        }
        m_mm.popTempRoot();
        push(val);
        return OpResult::OK;
    }
    if (!isList(listVal)) {
        return fromThrow(raiseThrowableError(
            "NotIndexableError",
            "Only lists and maps can be indexed for assignment.",
            stopAtFrameCount));
    }
    if (!is<Number>(indexVal)) {
        return fromThrow(raiseThrowableError("IndexTypeError",
                                             "List index must be a number.",
                                             stopAtFrameCount));
    }
    double n = as<Number>(indexVal);
    if (n != std::floor(n)) {
        return fromThrow(raiseThrowableError("IndexNotIntegerError",
                                             "List index must be an integer.",
                                             stopAtFrameCount));
    }
    auto* list = asObjList(as<Obj*>(listVal));
    int idx = static_cast<int>(n);
    if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
        return fromThrow(raiseThrowableError("IndexOutOfBoundsError",
                                             "List index out of bounds.",
                                             stopAtFrameCount));
    }
    list->elements[idx] = val;
    push(val); // assignment is an expression; its value is the assigned value
    return OpResult::OK;
}

Runtime::OpResult Runtime::opGetIter() {
    // peek(0) keeps iterable on stack as GC root during create<>().
    Value iterable = peek(0);
    if (!isList(iterable) && !isString(iterable) && !isMap(iterable)) {
        runtimeError("Value is not iterable (expected list, string, or map).");
        return OpResult::Fatal;
    }
    Obj* obj = as<Obj*>(iterable);
    ObjIterator* it = m_mm.create<ObjIterator>(
        iterable, 0, isObjMap(obj) ? asObjMap(obj)->version : -1);
    stackTop[-1] = Value{static_cast<Obj*>(it)}; // replace in-place
    return OpResult::OK;
}

Runtime::OpResult Runtime::opIterHasNext() {
    Value top = pop();
    // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
    if (!isIterator(top)) {
        runtimeError("BUG: ITER_HAS_NEXT expects an iterator on the stack.");
        return OpResult::Fatal;
    }
    ObjIterator* it = asObjIterator(as<Obj*>(top));
    bool has;
    if (isList(it->collection)) {
        has = it->index <
              (int)asObjList(as<Obj*>(it->collection))->elements.size();
    } else if (isString(it->collection)) {
        has = it->index <
              (int)asObjString(as<Obj*>(it->collection))->chars.size();
    } else if (isMap(it->collection)) {
        // Fail fast on structural change, as Python does for dicts. A
        // version check also trips a paired erase plus insert that restores
        // the net size, which a size check would miss.
        auto* map = asObjMap(as<Obj*>(it->collection));
        if (map->version != it->expectedVersion) {
            runtimeError("Map changed size during iteration.");
            return OpResult::Fatal;
        }
        // Scan forward from current index for the next occupied bucket.
        int i = it->index;
        while (i < map->map.capacity() &&
               map->map.entryAt(i)->state != MapSlot::OCCUPIED) {
            ++i;
        }
        has = i < map->map.capacity();
    } else {
        runtimeError("BUG: ObjIterator::collection has unexpected type.");
        return OpResult::Fatal;
    }
    push(from<bool>(has));
    return OpResult::OK;
}

Runtime::OpResult Runtime::opIterNext() {
    Value top = pop();
    // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
    if (!isIterator(top)) {
        runtimeError("BUG: ITER_NEXT expects an iterator on the stack.");
        return OpResult::Fatal;
    }
    ObjIterator* it = asObjIterator(as<Obj*>(top));
    if (isList(it->collection)) {
        push(asObjList(as<Obj*>(it->collection))->elements[it->index++]);
    } else if (isString(it->collection)) {
        char ch = asObjString(as<Obj*>(it->collection))->chars[it->index++];
        // ObjIterator is GC-rooted at iterSlot on the stack; GC is
        // non-moving. ch is a plain char copied before makeString (which may
        // trigger GC).
        push(Value{
            static_cast<Obj*>(m_mm.makeString(std::string_view{&ch, 1}))});
    } else if (isMap(it->collection)) {
        // Skip past empty/tombstone buckets to the next occupied one, push
        // its key, then advance the cursor past it.
        auto* map = asObjMap(as<Obj*>(it->collection));
        if (map->version != it->expectedVersion) {
            runtimeError("Map changed size during iteration.");
            return OpResult::Fatal;
        }
        while (it->index < map->map.capacity() &&
               map->map.entryAt(it->index)->state != MapSlot::OCCUPIED) {
            ++it->index;
        }
        // ITER_HAS_NEXT was true, so an occupied slot must exist.
        push(map->map.entryAt(it->index)->key);
        ++it->index;
    } else {
        runtimeError("BUG: ObjIterator::collection has unexpected type.");
        return OpResult::Fatal;
    }
    return OpResult::OK;
}
