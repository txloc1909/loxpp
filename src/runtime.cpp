#include "runtime.h"
#include "objects.h"
#include "object.h"
#include "utility.h"
#include "compiler.h"
#include "backend/rt_abi.h"

#include "stdlib/stdlib_registrar.h"
#include "stdlib/globals.h"
#include "stdlib/file_api.h"
#include "stdlib/map_api.h"
#include "stdlib/error_api.h"
#include "stdlib/math_module.h"
#include "stdlib/os_api.h"
#include "stdlib/reflect_api.h"
#include "stdlib/net_api.h"
#include "stdlib/process_api.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

ObjClosure* Runtime::loadSource(const std::string& source,
                                DiagnosticSink* sink) {
    // Guard against dangling class pointers from a prior loadSource() call
    // on this same Runtime (a REPL line, or a second rt_startup() on a
    // process that already ran one program). GC can fire inside compile(),
    // and markRoots() must not dereference a pointer a previous program's
    // class definitions left behind.
    m_fileClass = nullptr;
    m_mapClass = nullptr;
    m_socketClass = nullptr;
    m_serverClass = nullptr;
    m_processClass = nullptr;
    ObjFunction* fn = compile(source, &m_mm, sink);
    if (fn == nullptr) {
        return nullptr;
    }
    // Root fn on the stack before any allocation (defineNatives,
    // create<ObjClosure>) can trigger GC. Without this, fn is unreachable
    // between compile() returning and the closure replacing it below — the
    // Compiler has already been destroyed and m_currentCompiler is nullptr.
    push(Value{static_cast<Obj*>(fn)});
    setActiveContext(&m_stdlibCtx);
    defineNatives();
    ObjClosure* closure = m_mm.create<ObjClosure>(fn);
    stackTop[-1] =
        Value{static_cast<Obj*>(closure)}; // replace fn with its closure
    return closure;
}

Runtime::ThrowOutcome Runtime::call(ObjClosure* closure, int argCount,
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
            if (outcome == ThrowOutcome::HandledContinue ||
                outcome == ThrowOutcome::HandledStop) {
                return outcome;
            }
        }
        // No handler found, or error class not ready; uncaught error.
        runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
        return ThrowOutcome::Uncaught;
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
        // Uncaught falls through to this same return unchanged:
        // raiseThrowableError() already reported it, and m_frameCount is
        // reset to 0 by then (see runtimeError()/resetStack()), so
        // continuing to push a frame below would be reading a torn-down VM.
        return outcome;
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
        return ThrowOutcome::Uncaught;
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
    return ThrowOutcome::Pushed;
}

Runtime::OpResult Runtime::callCompiled(ObjClosure* closure, int argCount,
                                        int stopAtFrameCount, ResultCheck check,
                                        const Value* resultOverride) {
    ThrowOutcome outcome = call(closure, argCount, stopAtFrameCount);
    if (outcome != ThrowOutcome::Pushed) {
        // call() already reported an arity mismatch or stack overflow (and,
        // if a handler was active, already unwound to it) — no frame of
        // ours was pushed, so there is nothing here to pop back off.
        return fromThrow(outcome);
    }
    CallFrame* frame = &m_frames[m_frameCount - 1];
    auto code = reinterpret_cast<RtCompiledFn>(closure->function->code);
    int status = code(this, frame->slots);
    if (status == kRtFatal) {
        // The compiled callee already reported a fatal error through
        // runtimeError() (rt_abi.h's own contract on RtCompiledFn), which
        // already called resetStack() and zeroed m_frameCount for the whole
        // Runtime. There is no frame left here for this call to close out —
        // doing so would double-decrement an already-reset count.
        return OpResult::Fatal;
    }
    if (status == kRtThrow) {
        // The callee's own frame is already gone (see rt_abi.h's own
        // comment on kRtThrow) — nothing here to close/collapse. Translate
        // compiled code's own three-way status back into the OpResult/
        // stopAtFrameCount convention the rest of Runtime already
        // understands, exactly the way fromThrow() does for every other
        // call site.
        return (m_frameCount > stopAtFrameCount) ? OpResult::Resumed
                                                 : OpResult::Stop;
    }
    // The C-ABI return convention for a compiled function, mirroring
    // Op::RETURN (vm.cpp) exactly: before returning 0, the callee leaves
    // its return value as the single value on top of the stack (the same
    // "one value at the top" contract RETURN's own `Value result = pop()`
    // relies on) — not at a fixed slot, and not via rt_set_top alone. This
    // call then performs the same frame-exit RETURN performs: close any
    // upvalue a nested closure captured over this frame's own locals,
    // drop any handler this frame owns, collapse the callee's stack
    // window, and push the return value back at the base of that window —
    // plus, when this call came from dispatchMethod() (`check` not None or
    // `resultOverride` set), the same ResultCheck/override handling
    // Op::RETURN performs (S7, #460): compiled RETURN itself never sees
    // these fields (backend/qbe_emitter.cpp's own lowering has no access to
    // Runtime's private state), so the caller of the compiled code — here —
    // is where that contract must be enforced instead.
    Value result = pop();
    closeUpvalues(frame->slots);
    popHandlersOwnedByCurrentFrame();
    m_frameResultCheck[m_frameCount - 1] = ResultCheck::None;
    m_frameResultOverrideSet[m_frameCount - 1] = false;
    m_frameCount--;
    stackTop = frame->slots;
    push(resultOverride != nullptr ? *resultOverride : result);
    if (check == ResultCheck::Sequence &&
        (isList(result) || isString(result) || isMap(result))) {
        Obj* obj = as<Obj*>(result);
        ObjIterator* it = m_mm.create<ObjIterator>(
            result, 0, isObjMap(obj) ? asObjMap(obj)->version : -1);
        stackTop[-1] = Value{static_cast<Obj*>(it)};
    }
    if (check == ResultCheck::Boolean && !is<bool>(result)) {
        return fromThrow(raiseThrowableError(
            "OperatorResultTypeError", "Operator method must return a Boolean.",
            stopAtFrameCount));
    }
    if (check == ResultCheck::Number && !is<Number>(result)) {
        return fromThrow(raiseThrowableError(
            "OperatorResultTypeError", "Operator method must return a Number.",
            stopAtFrameCount));
    }
    if (check == ResultCheck::Sequence &&
        !(isList(result) || isString(result) || isMap(result))) {
        return fromThrow(raiseThrowableError(
            "OperatorResultTypeError",
            "Operator method must return a sequence.", stopAtFrameCount));
    }
    if (check == ResultCheck::String && !isString(result)) {
        return fromThrow(raiseThrowableError(
            "OperatorResultTypeError", "Operator method must return a String.",
            stopAtFrameCount));
    }
    return OpResult::OK;
}

Runtime::OpResult Runtime::invokeClosure(ObjClosure* closure, int argCount,
                                         int stopAtFrameCount) {
    if (closure->function->code != nullptr) {
        return callCompiled(closure, argCount, stopAtFrameCount);
    }
    return fromThrow(call(closure, argCount, stopAtFrameCount));
}

Runtime::OpResult Runtime::runPushedFrameToCompletion(int entry) {
    if (!runNestedLoop(entry)) {
        return OpResult::Fatal;
    }
    if (m_frameCount != entry) {
        // The throw was resolved by a handler outside this call, so the nested
        // run() stopped early and control belongs to an outer run()
        // invocation. The stack top must not be touched.
        return OpResult::Stop;
    }
    return OpResult::OK;
}

Runtime::OpResult Runtime::reentrantCall(int argCount, int throwBoundary) {
    // The nested run() must stop once the frame this call pushes (if any) has
    // returned — i.e. when m_frameCount falls back to this call's own entry
    // depth. Captured before opCall() can push anything.
    int entry = m_frameCount;
    OpResult result = opCall(argCount, throwBoundary);
    if (result != OpResult::Resumed) {
        return result; // OK (value on stack), Stop, or Fatal
    }
    // Resumed means opCall() pushed a frame (ThrowOutcome::Pushed) or a throw
    // was caught inside the current run() (HandledContinue). Only the former
    // leaves a frame of ours to run; tell them apart by frame depth, exactly
    // as runPendingDefers() does.
    if (m_frameCount != entry + 1) {
        return OpResult::Resumed;
    }
    return runPushedFrameToCompletion(entry);
}

bool Runtime::runNestedLoop(int entry) {
    if (!m_runLoop) {
        // No interpreter loop is installed: this Runtime is driven by compiled
        // code (the QBE backend), where every callee has attached code and so
        // takes the callCompiled() path instead of pushing an interpreted
        // frame. Reaching here means an interpreted-fallback closure was pushed
        // with nothing to run it — the same dangling-frame hazard
        // rt_startup's requireAllCompiled guards against (backend/rt_capi.h).
        runtimeError("Re-entrant call requires an interpreter loop.");
        return false;
    }
    return m_runLoop(entry) == InterpretResult::OK;
}

Runtime::OpResult Runtime::runReentrantFrame(int entry, Value* frameSlots,
                                             int enclosingBoundary) {
    // The enclosing frame's ip, before the nested run. A caught throw redirects
    // it to a catch block; a normal return leaves it untouched. This is more
    // reliable than comparing stack slots: a map key operation can push its
    // keys at or below the handler checkpoint, so a caught throw need not
    // change where stackTop lands.
    bool haveCaller = entry >= 1;
    decltype(m_frames[0].ip) callerIpBefore{};
    if (haveCaller) {
        callerIpBefore = m_frames[entry - 1].ip;
    }
    if (!runNestedLoop(entry)) {
        return OpResult::Fatal;
    }
    // A handler below the enclosing run()'s own boundary caught the throw;
    // control belongs to a less-nested run().
    if (m_frameCount <= enclosingBoundary) {
        return OpResult::Stop;
    }
    // A handler inside the enclosing run() caught the throw and redirected this
    // frame to its catch block: the enclosing run must reload and continue, not
    // take a result.
    if (haveCaller && m_frames[entry - 1].ip != callerIpBefore) {
        return OpResult::Resumed;
    }
    // A normal return leaves exactly one value at frameSlots[0].
    if (m_frameCount != entry || stackTop != frameSlots + 1) {
        return OpResult::Resumed;
    }
    return OpResult::OK;
}

bool Runtime::invokeCallableFromNative(int argCount, Value* out) {
    int entry = m_frameCount;
    Value* frameSlots = stackTop - argCount - 1;
    OpResult result = opCall(argCount, m_nativeStopAtFrameCount);
    if (result == OpResult::Resumed && m_frameCount == entry + 1) {
        result = runReentrantFrame(entry, frameSlots, m_nativeStopAtFrameCount);
    }
    if (result == OpResult::OK) {
        *out = pop();
        return true;
    }
    m_reentrantOutcome = result;
    return false;
}

bool Runtime::invokeMethodFromNative(ObjClosure* method, int argCount,
                                     Value* out) {
    int entry = m_frameCount;
    Value* frameSlots = stackTop - argCount - 1;
    // The receiver already occupies stackTop[-argCount-1] (the caller pushed
    // it), so dispatchMethod() binds slot 0 to `this` exactly as Op::INVOKE.
    OpResult result = dispatchMethod(method, argCount, m_nativeStopAtFrameCount,
                                     ResultCheck::None);
    if (result == OpResult::Resumed && m_frameCount == entry + 1) {
        result = runReentrantFrame(entry, frameSlots, m_nativeStopAtFrameCount);
    }
    if (result == OpResult::OK) {
        *out = pop();
        return true;
    }
    m_reentrantOutcome = result;
    return false;
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

        // Only these four callable kinds are legal as a deferred call. A
        // class, enum constructor, instance, or non-callable value stays the
        // fatal "unexpected type" it has always been, rather than falling
        // through to reentrantCall()/opCall()'s wider dispatch
        // (tools/check_fault_table.py pins this).
        if (!isBoundMethod(calleeVal) && !isClosure(calleeVal) &&
            !isNative(calleeVal) && !isBoundNative(calleeVal)) {
            runtimeError("Deferred callable has unexpected type.");
            return InterpretResult::RUNTIME_ERROR;
        }

        // reentrantCall() runs the deferred call to completion — a nested
        // run() stopping once m_frameCount returns to frameIndex + 1, the
        // same boundary the old inline m_runLoop() used — and replaces the
        // pushed callee+args with the result on OK. It shares one
        // implementation with every other re-entrant call site; see its own
        // definition. For BoundMethod it replaces the callee slot with the
        // receiver, matching the old inline dispatch.
        OpResult callResult = reentrantCall(argCount, stopAtFrameCount);
        if (callResult == OpResult::Fatal) {
            return InterpretResult::RUNTIME_ERROR;
        }
        if (callResult == OpResult::Stop || callResult == OpResult::Resumed) {
            // The deferred call's own throw was either caught outside this
            // frame, or resolved without pushing a frame of ours (an arity
            // fault caught elsewhere). Either way frameIndex no longer
            // exists — per defer step 5's documented limitation, abandon any
            // remaining sibling defers rather than still running them; the
            // caller must notice m_frameCount changed and stop too.
            return InterpretResult::OK;
        }
        // OK: the result is on the stack. Move on to the next deferred call.
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
        const HandlerRecord& handler = m_handlerStack[i];
        if (handler.frameCount <= m_frameCount) {
            // Found a live handler (innermost one, since we iterate LIFO).
            foundHandler = true;
            handlerToUse = handler;
            handlerIndex = i;
            break;
        }
    }

    // Step 1.5: With no handler and a non-Error value, render the canonical
    // form now, while a frame is still live — __str__ needs one to run. Doing
    // it after the unwind below would leave m_frameCount at 0 and nothing to
    // run the method in.
    std::string uncaughtThrownStr;
    if (!foundHandler && !isError(thrownValue)) {
        int savedBoundary = m_stringifyBoundary;
        m_stringifyBoundary = stopAtFrameCount;
        OpResult savedStatus = m_stringifyStatus;
        m_stringifyStatus = OpResult::OK;
        m_stringifyCanonicalDepth++;
        uncaughtThrownStr = stringify(thrownValue);
        m_stringifyCanonicalDepth--;
        OpResult status = m_stringifyStatus;
        m_stringifyBoundary = savedBoundary;
        m_stringifyStatus = savedStatus;
        if (status == OpResult::Fatal) {
            // The __str__ dispatch itself threw uncaught: that fault already
            // reported itself and unwound everything, so do not report this
            // throw a second time.
            return ThrowOutcome::Uncaught;
        }
    }

    // Step 2: Unwind frame-by-frame, draining defers, to either the
    // handler's frame (if found) or frame 0 (if not found).
    int targetFrameCount = foundHandler ? handlerToUse.frameCount : 0;
    while (m_frameCount > targetFrameCount) {
        int unwoundFrameIndex = m_frameCount - 1;
        closeUpvalues(m_frames[unwoundFrameIndex].slots);
        m_frameResultCheck[unwoundFrameIndex] = ResultCheck::None;
        m_frameResultOverrideSet[unwoundFrameIndex] = false;
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
        // The canonical form was computed before the unwind (step 1.5), while
        // a frame was still live to run __str__.
        runtimeError("%s", uncaughtThrownStr.c_str());
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

Runtime::OpResult Runtime::callNative(ObjNative* native, int argCount,
                                      int stopAtFrameCount) {
    if (native->arity != -1 && argCount != native->arity) {
        runtimeError("Expected %d arguments but got %d.", native->arity,
                     argCount);
        return OpResult::Fatal;
    }
    m_stdlibCtx.clearError();
    // A native may call back into the VM (callMethod -> a closure-backed
    // method). That sets m_reentrantOutcome when the nested call does not
    // return a value; save any outer value so nesting does not clobber it.
    // m_nativeStopAtFrameCount must likewise name the enclosing run()'s own
    // boundary for the duration of this native.
    OpResult savedOutcome = m_reentrantOutcome;
    m_reentrantOutcome = OpResult::OK;
    int savedBoundary = m_nativeStopAtFrameCount;
    m_nativeStopAtFrameCount = stopAtFrameCount;
    Value result = native->function(argCount, stackTop - argCount);
    m_nativeStopAtFrameCount = savedBoundary;
    OpResult outcome = m_reentrantOutcome;
    m_reentrantOutcome = savedOutcome;
    if (outcome != OpResult::OK) {
        // Control left this invocation (a throw handled by an outer run(), an
        // error caught inside this run(), or an uncaught fault that already
        // reset the stack). Do not push the native's placeholder return value.
        return outcome;
    }
    if (m_stdlibCtx.nativeError) {
        runtimeError("%s", m_stdlibCtx.nativeErrorMsg.c_str());
        return OpResult::Fatal;
    }
    stackTop -= argCount + 1; // pop args + callee
    push(result);
    return OpResult::OK;
}

Runtime::OpResult Runtime::callBoundNative(ObjBoundNative* bn, int argCount,
                                           int stopAtFrameCount) {
    ObjNative* fn = bn->native;             // read before the slot changes
    stackTop[-argCount - 1] = bn->receiver; // natives read args[-1]
    return callNative(fn, argCount, stopAtFrameCount);
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
    initProtocolNames();
    StdlibRegistrar reg(m_mm, m_globals);
    registerGlobals(reg);
    m_fileClass = registerFileAPI(reg);
    m_mapClass = registerMapAPI(reg);
    m_errorClass = registerErrorAPI(reg);
    registerMath(reg);
    registerOSAPI(reg, m_mapClass);
    registerReflectAPI(reg);
    NetClasses net = registerNetAPI(reg);
    m_socketClass = net.socket;
    m_serverClass = net.server;
    m_processClass = registerProcessAPI(reg, m_mapClass);
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
    if (m_socketClass) {
        m_mm.markObject(m_socketClass);
    }
    if (m_serverClass) {
        m_mm.markObject(m_serverClass);
    }
    if (m_processClass) {
        m_mm.markObject(m_processClass);
    }
    for (ObjString* name : m_protocolNames) {
        if (name) {
            m_mm.markObject(name);
        }
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
    m_frameResultCheck.fill(ResultCheck::None);
    m_frameResultOverrideSet.fill(false);
}

void Runtime::initProtocolNames() {
    const char* names[] = {
        "__add__",      "__radd__", "__sub__",       "__rsub__",
        "__mul__",      "__rmul__", "__div__",       "__rdiv__",
        "__mod__",      "__rmod__", "__neg__",       "__lt__",
        "__rlt__",      "__gt__",   "__rgt__",       "__eq__",
        "__contains__", "__call__", "__index_get__", "__index_set__",
        "__len__",      "__iter__", "__slice__",     "__hash__",
        "__str__"};
    for (std::size_t i = 0; i < m_protocolNames.size(); i++) {
        m_protocolNames[i] = m_mm.makeString(names[i]);
    }
}

std::optional<Runtime::OpResult>
Runtime::tryBinaryMethod(Protocol proto, int stopAtFrameCount) {
    Value left = peek(1);
    if (!isInstance(left)) {
        return std::nullopt;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(left));
    Value method;
    if (!instance->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(proto)], method)) {
        return std::nullopt;
    }
    return dispatchMethod(asObjClosure(as<Obj*>(method)), 1, stopAtFrameCount,
                          ResultCheck::None);
}

std::optional<Runtime::OpResult>
Runtime::tryBinaryMethodBool(Protocol proto, int stopAtFrameCount) {
    Value left = peek(1);
    if (!isInstance(left)) {
        return std::nullopt;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(left));
    Value method;
    if (!instance->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(proto)], method)) {
        return std::nullopt;
    }
    return dispatchMethod(asObjClosure(as<Obj*>(method)), 1, stopAtFrameCount,
                          ResultCheck::Boolean);
}

std::optional<Runtime::OpResult>
Runtime::tryReflectedBinaryMethod(Protocol proto, int stopAtFrameCount) {
    Value right = peek(0);
    if (!isInstance(right)) {
        return std::nullopt;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(right));
    Value method;
    if (!instance->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(proto)], method)) {
        return std::nullopt;
    }
    // Swap so the right operand is the receiver (slot 0) and the left operand
    // the argument (slot 1); both values stay on the stack, so the swap is
    // GC-safe.
    Value left = peek(1);
    stackTop[-2] = right;
    stackTop[-1] = left;
    return dispatchMethod(asObjClosure(as<Obj*>(method)), 1, stopAtFrameCount,
                          ResultCheck::None);
}

std::optional<Runtime::OpResult>
Runtime::tryReflectedBinaryMethodBool(Protocol proto, int stopAtFrameCount) {
    Value right = peek(0);
    if (!isInstance(right)) {
        return std::nullopt;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(right));
    Value method;
    if (!instance->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(proto)], method)) {
        return std::nullopt;
    }
    Value left = peek(1);
    stackTop[-2] = right;
    stackTop[-1] = left;
    return dispatchMethod(asObjClosure(as<Obj*>(method)), 1, stopAtFrameCount,
                          ResultCheck::Boolean);
}

Runtime::OpResult Runtime::dispatchMethod(ObjClosure* method, int argCount,
                                          int stopAtFrameCount,
                                          ResultCheck check,
                                          const Value* resultOverride) {
    // S7 (#460): a compiled method makes the same branch invokeClosure()
    // does — there is no VM::run() loop under the QBE backend to pick a
    // bare call()'s pushed-but-not-run frame back up, so calling it
    // directly the way call()+Resumed assumes leaves the frame dangling
    // forever. callCompiled() takes the ResultCheck/resultOverride this
    // dispatch needs and applies them itself once the compiled callee
    // returns (runtime.h's own comment on callCompiled).
    if (method->function->code != nullptr) {
        return callCompiled(method, argCount, stopAtFrameCount, check,
                            resultOverride);
    }
    ThrowOutcome outcome = call(method, argCount, stopAtFrameCount);
    if (outcome != ThrowOutcome::Pushed) {
        return fromThrow(outcome);
    }
    int idx = m_frameCount - 1;
    if (check != ResultCheck::None) {
        m_frameResultCheck[idx] = check;
    }
    if (resultOverride != nullptr) {
        m_frameResultOverride[idx] = *resultOverride;
        m_frameResultOverrideSet[idx] = true;
    }
    return OpResult::Resumed;
}

// --- op*() opcode helpers ------------------------------------------------

Runtime::OpResult Runtime::opCall(int argCount, int stopAtFrameCount) {
    Value callee = peek(argCount);
    if (isNative(callee)) {
        return callNative(asObjNative(callee), argCount, stopAtFrameCount);
    }
    if (isClosure(callee)) {
        return invokeClosure(asObjClosure(callee), argCount, stopAtFrameCount);
    }
    if (isBoundMethod(callee)) {
        ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(callee));
        // Slot 0 of the new frame = receiver (= this).
        stackTop[-argCount - 1] = bound->receiver;
        return invokeClosure(bound->method, argCount, stopAtFrameCount);
    }
    if (isBoundNative(callee)) {
        ObjBoundNative* bn = asObjBoundNative(as<Obj*>(callee));
        return callBoundNative(bn, argCount, stopAtFrameCount);
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
            return invokeClosure(asObjClosure(as<Obj*>(initMethod)), argCount,
                                 stopAtFrameCount);
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
    if (isInstance(callee)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(callee));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Call)],
                method)) {
            // The receiver already sits at stackTop[-argCount-1], which
            // becomes slot 0 of the new frame.
            return dispatchMethod(asObjClosure(as<Obj*>(method)), argCount,
                                  stopAtFrameCount, ResultCheck::None);
        }
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
                return invokeClosure(asObjClosure(as<Obj*>(fieldVal)), argCount,
                                     stopAtFrameCount);
            }
            if (isNative(fieldVal)) {
                return callNative(asObjNative(as<Obj*>(fieldVal)), argCount,
                                  stopAtFrameCount);
            }
            if (isBoundNative(fieldVal)) {
                ObjBoundNative* bn = asObjBoundNative(as<Obj*>(fieldVal));
                return callBoundNative(bn, argCount, stopAtFrameCount);
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
            return callNative(asObjNative(methodObj), argCount,
                              stopAtFrameCount);
        }
        return invokeClosure(asObjClosure(methodObj), argCount,
                             stopAtFrameCount);
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
        return callNative(asObjNative(as<Obj*>(method)), argCount,
                          stopAtFrameCount);
    }
    if (isMap(receiver)) {
        Value method;
        if (!m_mapClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on map.", name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount,
                          stopAtFrameCount);
    }
    if (isSocket(receiver)) {
        Value method;
        if (!m_socketClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on socket.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount,
                          stopAtFrameCount);
    }
    if (isServer(receiver)) {
        Value method;
        if (!m_serverClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on server.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount,
                          stopAtFrameCount);
    }
    if (isProcess(receiver)) {
        Value method;
        if (!m_processClass->methods.get(name, method)) {
            runtimeError("Undefined method '%s' on process.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        return callNative(asObjNative(as<Obj*>(method)), argCount,
                          stopAtFrameCount);
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
    if (isSocket(peek(0))) {
        Value method;
        if (!m_socketClass->methods.get(name, method)) {
            runtimeError("Undefined property '%s' on socket.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        ObjBoundNative* bound =
            m_mm.create<ObjBoundNative>(peek(0), asObjNative(as<Obj*>(method)));
        pop(); // socket
        push(Value{static_cast<Obj*>(bound)});
        return OpResult::OK;
    }
    if (isServer(peek(0))) {
        Value method;
        if (!m_serverClass->methods.get(name, method)) {
            runtimeError("Undefined property '%s' on server.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        ObjBoundNative* bound =
            m_mm.create<ObjBoundNative>(peek(0), asObjNative(as<Obj*>(method)));
        pop(); // server
        push(Value{static_cast<Obj*>(bound)});
        return OpResult::OK;
    }
    if (isProcess(peek(0))) {
        Value method;
        if (!m_processClass->methods.get(name, method)) {
            runtimeError("Undefined property '%s' on process.",
                         name->chars.c_str());
            return OpResult::Fatal;
        }
        ObjBoundNative* bound =
            m_mm.create<ObjBoundNative>(peek(0), asObjNative(as<Obj*>(method)));
        pop(); // process
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
    return invokeClosure(asObjClosure(as<Obj*>(method)), argCount,
                         stopAtFrameCount);
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

bool Runtime::mapKeyValid(const Value& v) const {
    if (!isInstance(v)) {
        return isValidMapKey(v);
    }
    // An Instance key needs both methods: __hash__ places it, and __eq__
    // resolves a collision (the class defines equality for it).
    ObjInstance* inst = asObjInstance(as<Obj*>(v));
    Value ignored;
    return inst->klass->methods.get(
               m_protocolNames[static_cast<std::size_t>(Protocol::Hash)],
               ignored) &&
           inst->klass->methods.get(
               m_protocolNames[static_cast<std::size_t>(Protocol::Eq)],
               ignored);
}

std::optional<Runtime::MapKeyError> Runtime::mapKeyError(Value indexVal) {
    // mapKeyValid() covers the pure scalar/string rule (isValidMapKey,
    // value.cpp) plus the Instance-defining-both rule. Once it says no,
    // is<Number> alone tells which of the two rejection reasons applies: a
    // rejected Number is always NaN, and a rejected Obj* is always a non-key
    // object — those are the only two ways it says no.
    if (mapKeyValid(indexVal)) {
        return std::nullopt;
    }
    if (is<Number>(indexVal)) {
        return MapKeyError{"NaNKeyError", "NaN cannot be used as a map key."};
    }
    return MapKeyError{"InvalidMapKeyError",
                       "Map keys must be Bool, Number, Nil, String, or an "
                       "object with __hash__ and __eq__."};
}

std::optional<Runtime::OpResult> Runtime::checkMapKey(Value indexVal,
                                                      int stopAtFrameCount) {
    if (auto err = mapKeyError(indexVal)) {
        return fromThrow(
            raiseThrowableError(err->kind, err->message, stopAtFrameCount));
    }
    return std::nullopt;
}

std::optional<uint32_t> Runtime::hashMapKey(const Value& key,
                                            int stopAtFrameCount) {
    if (!isInstance(key)) {
        return hashValue(key);
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(key));
    Value method;
    if (!inst->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(Protocol::Hash)],
            method)) {
        // mapKeyValid() rejects this before a hash is asked for; keep a
        // defensive path so a missed check is a clean error, not a crash.
        m_mapKeyStatus = fromThrow(raiseThrowableError(
            "InvalidMapKeyError",
            "Map keys must be Bool, Number, Nil, String, or an object with "
            "__hash__ and __eq__.",
            stopAtFrameCount));
        return std::nullopt;
    }
    push(key);
    Value* frameSlots = stackTop - 1;
    int entry = m_frameCount;
    OpResult r = dispatchMethod(asObjClosure(as<Obj*>(method)), 0,
                                stopAtFrameCount, ResultCheck::Number);
    if (r == OpResult::Resumed && m_frameCount == entry + 1) {
        r = runReentrantFrame(entry, frameSlots, stopAtFrameCount);
    }
    if (r != OpResult::OK) {
        m_mapKeyStatus = r;
        return std::nullopt;
    }
    Number n = as<Number>(pop());
    if (std::isnan(n)) {
        m_mapKeyStatus = fromThrow(raiseThrowableError(
            "NaNKeyError", "NaN cannot be used as a map key.",
            stopAtFrameCount));
        return std::nullopt;
    }
    // Fold the Number to a bucket hash the same way hashValue() does, so two
    // equal __hash__ results (including +0.0 and -0.0) share a bucket.
    if (n == 0.0) {
        n = 0.0;
    }
    uint64_t bits;
    std::memcpy(&bits, &n, sizeof bits);
    return static_cast<uint32_t>(bits ^ (bits >> 32));
}

bool Runtime::mapKeyEq(const Value& stored, const Value& lookup,
                       int stopAtFrameCount) {
    // Only the stored key's class can define key equality. Lox++ has no
    // reflected operator method, so a lookup-only __eq__ never runs.
    if (isInstance(stored)) {
        ObjInstance* inst = asObjInstance(as<Obj*>(stored));
        Value method;
        if (inst->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Eq)],
                method)) {
            push(stored);
            push(lookup);
            Value* frameSlots = stackTop - 2;
            int entry = m_frameCount;
            OpResult r = dispatchMethod(asObjClosure(as<Obj*>(method)), 1,
                                        stopAtFrameCount, ResultCheck::Boolean);
            if (r == OpResult::Resumed && m_frameCount == entry + 1) {
                r = runReentrantFrame(entry, frameSlots, stopAtFrameCount);
            }
            if (r != OpResult::OK) {
                m_mapKeyStatus = r;
                return false;
            }
            return as<bool>(pop());
        }
    }
    return stored == lookup;
}

Runtime::OpResult Runtime::mapGetKey(ObjMap* map, const Value& key, bool& found,
                                     Value& out, int stopAtFrameCount) {
    if (auto err = checkMapKey(key, stopAtFrameCount)) {
        return *err;
    }
    found = false;
    map->keyOpDepth += 1;
    // Root the map and the lookup key before any __hash__/__eq__ can allocate.
    m_mm.pushTempRoot(map);
    if (is<Obj*>(key)) {
        m_mm.pushTempRoot(as<Obj*>(key));
    }
    m_mapKeyStatus = OpResult::OK;
    std::optional<uint32_t> hash = hashMapKey(key, stopAtFrameCount);
    OpResult result = OpResult::OK;
    if (!hash) {
        result = m_mapKeyStatus;
    } else {
        ObjMap::KeyEq eq = [this, stopAtFrameCount](const Value& s,
                                                    const Value& l) {
            return mapKeyEq(s, l, stopAtFrameCount);
        };
        found = map->mapGetHashed(key, *hash, eq, out);
        result = m_mapKeyStatus;
    }
    if (is<Obj*>(key)) {
        m_mm.popTempRoot();
    }
    m_mm.popTempRoot();
    map->keyOpDepth -= 1;
    return result;
}

Runtime::OpResult Runtime::mapSetKey(ObjMap* map, const Value& key,
                                     const Value& value, int stopAtFrameCount) {
    // A write to a map that is already inside a key operation is forbidden:
    // the probe holds raw bucket pointers that a reentrant grow would
    // invalidate.
    if (map->keyOpDepth > 0) {
        return fromThrow(raiseThrowableError(
            "MapChangedError", "Map changed during key hashing or equality.",
            stopAtFrameCount));
    }
    if (auto err = checkMapKey(key, stopAtFrameCount)) {
        return *err;
    }
    map->keyOpDepth += 1;
    m_mm.pushTempRoot(map);
    if (is<Obj*>(key)) {
        m_mm.pushTempRoot(as<Obj*>(key));
    }
    if (is<Obj*>(value)) {
        m_mm.pushTempRoot(as<Obj*>(value));
    }
    m_mapKeyStatus = OpResult::OK;
    std::optional<uint32_t> hash = hashMapKey(key, stopAtFrameCount);
    OpResult result = OpResult::OK;
    if (!hash) {
        result = m_mapKeyStatus;
    } else {
        bool eqFailed = false;
        ObjMap::KeyEq eq = [this, stopAtFrameCount, &eqFailed](const Value& s,
                                                               const Value& l) {
            bool r = mapKeyEq(s, l, stopAtFrameCount);
            if (m_mapKeyStatus != OpResult::OK) {
                eqFailed = true;
            }
            return r;
        };
        map->mapSetHashed(key, value, *hash, eq, eqFailed);
        result = m_mapKeyStatus;
    }
    if (is<Obj*>(value)) {
        m_mm.popTempRoot();
    }
    if (is<Obj*>(key)) {
        m_mm.popTempRoot();
    }
    m_mm.popTempRoot();
    map->keyOpDepth -= 1;
    return result;
}

Runtime::OpResult Runtime::mapDelKey(ObjMap* map, const Value& key,
                                     int stopAtFrameCount) {
    if (map->keyOpDepth > 0) {
        return fromThrow(raiseThrowableError(
            "MapChangedError", "Map changed during key hashing or equality.",
            stopAtFrameCount));
    }
    if (auto err = checkMapKey(key, stopAtFrameCount)) {
        return *err;
    }
    map->keyOpDepth += 1;
    m_mm.pushTempRoot(map);
    if (is<Obj*>(key)) {
        m_mm.pushTempRoot(as<Obj*>(key));
    }
    m_mapKeyStatus = OpResult::OK;
    std::optional<uint32_t> hash = hashMapKey(key, stopAtFrameCount);
    OpResult result = OpResult::OK;
    if (!hash) {
        result = m_mapKeyStatus;
    } else {
        bool eqFailed = false;
        ObjMap::KeyEq eq = [this, stopAtFrameCount, &eqFailed](const Value& s,
                                                               const Value& l) {
            bool r = mapKeyEq(s, l, stopAtFrameCount);
            if (m_mapKeyStatus != OpResult::OK) {
                eqFailed = true;
            }
            return r;
        };
        map->mapDelHashed(key, *hash, eq, eqFailed);
        result = m_mapKeyStatus;
    }
    if (is<Obj*>(key)) {
        m_mm.popTempRoot();
    }
    m_mm.popTempRoot();
    map->keyOpDepth -= 1;
    return result;
}

bool Runtime::mapHasFromNative(ObjMap* map, const Value& key, bool* out) {
    Value ignored;
    bool found = false;
    OpResult r = mapGetKey(map, key, found, ignored, m_nativeStopAtFrameCount);
    if (r != OpResult::OK) {
        m_reentrantOutcome = r;
        return false;
    }
    *out = found;
    return true;
}

bool Runtime::mapDelFromNative(ObjMap* map, const Value& key) {
    OpResult r = mapDelKey(map, key, m_nativeStopAtFrameCount);
    if (r != OpResult::OK) {
        m_reentrantOutcome = r;
        return false;
    }
    return true;
}

std::optional<Runtime::OpResult>
Runtime::checkSequenceIndex(Value indexVal, size_t size, const char* noun,
                            int stopAtFrameCount, int* outIdx) {
    char msg[64];
    if (!is<Number>(indexVal)) {
        snprintf(msg, sizeof(msg), "%s index must be a number.", noun);
        return fromThrow(
            raiseThrowableError("IndexTypeError", msg, stopAtFrameCount));
    }
    double n = as<Number>(indexVal);
    if (n != std::floor(n)) {
        snprintf(msg, sizeof(msg), "%s index must be an integer.", noun);
        return fromThrow(
            raiseThrowableError("IndexNotIntegerError", msg, stopAtFrameCount));
    }
    int idx = static_cast<int>(n);
    if (idx < 0 || idx >= static_cast<int>(size)) {
        snprintf(msg, sizeof(msg), "%s index out of bounds.", noun);
        return fromThrow(raiseThrowableError("IndexOutOfBoundsError", msg,
                                             stopAtFrameCount));
    }
    *outIdx = idx;
    return std::nullopt;
}

Runtime::OpResult Runtime::opGetIndex(int stopAtFrameCount) {
    Value indexVal = pop();
    Value collectionVal = pop();
    if (isList(collectionVal)) {
        auto* list = asObjList(as<Obj*>(collectionVal));
        int idx = 0;
        if (auto err = checkSequenceIndex(indexVal, list->elements.size(),
                                          "List", stopAtFrameCount, &idx)) {
            return *err;
        }
        push(list->elements[idx]);
        return OpResult::OK;
    }
    if (isString(collectionVal)) {
        auto* str = asObjString(as<Obj*>(collectionVal));
        int idx = 0;
        if (auto err = checkSequenceIndex(indexVal, str->chars.size(), "String",
                                          stopAtFrameCount, &idx)) {
            return *err;
        }
        // Copy char before makeString (GC-safe: same pattern as ADD)
        char ch = str->chars[idx];
        push(Value{
            static_cast<Obj*>(m_mm.makeString(std::string_view{&ch, 1}))});
        return OpResult::OK;
    }
    if (isMap(collectionVal)) {
        auto* map = asObjMap(as<Obj*>(collectionVal));
        Value result{Nil{}}; // default nil — returned when key absent
        bool found = false;
        if (OpResult r =
                mapGetKey(map, indexVal, found, result, stopAtFrameCount);
            r != OpResult::OK) {
            return r;
        }
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
    if (isInstance(collectionVal)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(collectionVal));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::IndexGet)],
                method)) {
            // Receiver is the collection, argument is the index.
            push(collectionVal);
            push(indexVal);
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 1,
                                  stopAtFrameCount, ResultCheck::None);
        }
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
        auto* map = asObjMap(as<Obj*>(listVal));
        // mapSetKey roots the map, key, and value for the duration, and
        // hashes the key (which may dispatch __hash__).
        if (OpResult r = mapSetKey(map, indexVal, val, stopAtFrameCount);
            r != OpResult::OK) {
            return r;
        }
        push(val);
        return OpResult::OK;
    }
    if (isInstance(listVal)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(listVal));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::IndexSet)],
                method)) {
            // Receiver is the collection, arguments are the index and the
            // value. The assignment evaluates to `val`, not the method's
            // return, so the result is overridden.
            push(listVal);
            push(indexVal);
            push(val);
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 2,
                                  stopAtFrameCount, ResultCheck::None, &val);
        }
    }
    if (!isList(listVal)) {
        return fromThrow(raiseThrowableError(
            "NotIndexableError",
            "Only lists and maps can be indexed for assignment.",
            stopAtFrameCount));
    }
    auto* list = asObjList(as<Obj*>(listVal));
    int idx = 0;
    if (auto err = checkSequenceIndex(indexVal, list->elements.size(), "List",
                                      stopAtFrameCount, &idx)) {
        return *err;
    }
    list->elements[idx] = val;
    push(val); // assignment is an expression; its value is the assigned value
    return OpResult::OK;
}

Runtime::OpResult Runtime::opGetIter(int stopAtFrameCount) {
    // peek(0) keeps iterable on stack as GC root during create<>().
    Value iterable = peek(0);
    if (isInstance(iterable)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(iterable));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Iter)],
                method)) {
            // Receiver is the iterable. The method's result must be a
            // List/String/Map; Op::RETURN builds the iterator from it.
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 0,
                                  stopAtFrameCount, ResultCheck::Sequence);
        }
    }
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

bool Runtime::mapIterationInvalidated(ObjMap* map, int expectedVersion) {
    if (map->version != expectedVersion) {
        runtimeError("Map changed size during iteration.");
        return true;
    }
    return false;
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
        // Fail fast on structural change, as Python does for dicts.
        auto* map = asObjMap(as<Obj*>(it->collection));
        if (mapIterationInvalidated(map, it->expectedVersion)) {
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
        if (mapIterationInvalidated(map, it->expectedVersion)) {
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

// --- classes, methods, aggregates, slicing, match dispatch (S5, #458) -----
//
// Moved out of VM::run() the same way as the property/index/iterator op*()
// methods above (S1's Layer 1), so the QBE backend (backend/rt_capi.h)
// reaches them with no separate implementation. opClass and opDefineMethod
// have no error path in VM::run() either, matching opDefineGlobal's shape.

void Runtime::opClass(ObjString* name) {
    ObjClass* klass = m_mm.create<ObjClass>(name, VmAllocator<Entry>{&m_mm});
    push(Value{static_cast<Obj*>(klass)});
}

Runtime::OpResult Runtime::opSetProperty(ObjString* name) {
    if (!isInstance(peek(1))) {
        runtimeError("Only instances have fields.");
        return OpResult::Fatal;
    }
    ObjInstance* instance = asObjInstance(as<Obj*>(peek(1)));
    instance->fields.set(name, peek(0));
    Value val = pop(); // value
    pop();             // instance
    push(val);         // assignment is an expression
    return OpResult::OK;
}

void Runtime::opDefineMethod(ObjString* name) {
    Value method = peek(0);                          // ObjClosure* on top
    ObjClass* klass = asObjClass(as<Obj*>(peek(1))); // class below
    klass->methods.set(name, method);
    pop(); // pop closure; leave class on stack for next method
}

void Runtime::opDeferRecord(int argCount) {
    // Mirrors vm.cpp's own Op::DEFER_RECORD body exactly (moved here so the
    // QBE backend's rt_op_defer_record, backend/rt_capi.h, reaches it with
    // no duplication — Layer 1, notes/qbe-backend.md).
    Value callee = stackTop[-(argCount + 1)];
    ObjDeferredCall* deferred =
        m_mm.create<ObjDeferredCall>(callee, VmAllocator<Value>{&m_mm});
    m_mm.pushTempRoot(deferred);
    for (int i = argCount - 1; i >= 0; i--) {
        deferred->args.push_back(stackTop[-(i + 1)]);
    }
    m_mm.popTempRoot();
    stackTop -= argCount + 1;
    m_deferLists[m_frameCount - 1].push_back(
        Value{static_cast<Obj*>(deferred)});
}

Runtime::OpResult Runtime::opBuildList(int count) {
    ObjList* list = m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
    m_mm.pushTempRoot(list); // protect across resize's potential GC
    list->elements.resize(static_cast<std::size_t>(count));
    for (int i = count - 1; i >= 0; i--) {
        list->elements[static_cast<std::size_t>(i)] = pop();
    }
    m_mm.popTempRoot();
    push(Value{static_cast<Obj*>(list)});
    return OpResult::OK;
}

Runtime::OpResult Runtime::opBuildMap(int count, int stopAtFrameCount) {
    // Validate all keys before any allocation. Stack (top to bottom):
    //   val_{n-1}, key_{n-1}, ..., val_0, key_0
    for (int i = 0; i < count; i++) {
        Value key = peek(2 * (count - 1 - i) + 1);
        if (auto err = checkMapKey(key, stopAtFrameCount)) {
            return *err;
        }
    }
    ObjMap* map = m_mm.create<ObjMap>(m_mapClass, VmAllocator<MapEntry>{&m_mm});
    // Values are still on the stack -> GC-rooted; mapSetKey temp-roots the
    // map, key, and value, and hashes the key (which may dispatch __hash__).
    for (int i = 0; i < count; i++) {
        Value key = peek(2 * (count - 1 - i) + 1);
        Value val = peek(2 * (count - 1 - i));
        if (OpResult r = mapSetKey(map, key, val, stopAtFrameCount);
            r != OpResult::OK) {
            return r;
        }
    }
    for (int i = 0; i < 2 * count; i++) {
        pop();
    }
    push(Value{static_cast<Obj*>(map)});
    return OpResult::OK;
}

Runtime::OpResult Runtime::opSlice(int stopAtFrameCount) {
    // Stack (bottom -> top): seq, start, end
    Value endVal = peek(0);
    Value startVal = peek(1);
    Value seqVal = peek(2);

    if (!isList(seqVal) && !isString(seqVal)) {
        if (isInstance(seqVal)) {
            ObjInstance* instance = asObjInstance(as<Obj*>(seqVal));
            Value method;
            if (instance->klass->methods.get(
                    m_protocolNames[static_cast<std::size_t>(Protocol::Slice)],
                    method)) {
                // The stack is already [seq, start, end]: the receiver sits
                // at stackTop[-3] and the two arguments at [-2], [-1], the
                // exact layout dispatchMethod expects for argCount 2.
                return dispatchMethod(asObjClosure(as<Obj*>(method)), 2,
                                      stopAtFrameCount, ResultCheck::None);
            }
        }
        runtimeError("Slice requires a List or String.");
        return OpResult::Fatal;
    }
    if (!is<Number>(startVal)) {
        runtimeError("Slice index must be a number.");
        return OpResult::Fatal;
    }
    double startD = as<Number>(startVal);
    if (startD != std::floor(startD)) {
        runtimeError("Slice index must be an integer.");
        return OpResult::Fatal;
    }
    if (startD < 0.0) {
        runtimeError("Slice index must be non-negative.");
        return OpResult::Fatal;
    }
    if (!is<Number>(endVal)) {
        runtimeError("Slice index must be a number.");
        return OpResult::Fatal;
    }
    double endD = as<Number>(endVal);
    if (endD != std::floor(endD)) {
        runtimeError("Slice index must be an integer.");
        return OpResult::Fatal;
    }
    if (endD < 0.0) {
        runtimeError("Slice index must be non-negative.");
        return OpResult::Fatal;
    }

    if (isList(seqVal)) {
        auto* src = asObjList(as<Obj*>(seqVal));
        int n = static_cast<int>(src->elements.size());
        int s = static_cast<int>(std::min(startD, static_cast<double>(n)));
        int e = static_cast<int>(std::min(endD, static_cast<double>(n)));
        int count = (s < e) ? e - s : 0;

        ObjList* result = m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
        // seqVal is still at peek(2) -> src is GC-rooted on the stack.
        m_mm.pushTempRoot(result);
        result->elements.resize(static_cast<std::size_t>(count)); // may GC
        src = asObjList(as<Obj*>(peek(2))); // re-read after potential GC
        for (int i = 0; i < count; i++) {
            result->elements[static_cast<std::size_t>(i)] =
                src->elements[static_cast<std::size_t>(s + i)];
        }
        m_mm.popTempRoot();
        pop();
        pop();
        pop();
        push(Value{static_cast<Obj*>(result)});
    } else {
        // String -- copy chars to a local buffer while src is still on stack.
        auto* src = asObjString(as<Obj*>(seqVal));
        int n = static_cast<int>(src->chars.size());
        int s = static_cast<int>(std::min(startD, static_cast<double>(n)));
        int e = static_cast<int>(std::min(endD, static_cast<double>(n)));
        std::string substr = (s < e)
                                 ? std::string(src->chars.data() + s,
                                               static_cast<std::size_t>(e - s))
                                 : std::string{};
        pop();
        pop();
        pop();
        push(Value{static_cast<Obj*>(m_mm.makeString(std::move(substr)))});
    }
    return OpResult::OK;
}

Runtime::OpResult Runtime::opGetTag() {
    Value val = pop();
    if (!isEnumValue(val)) {
        runtimeError("GET_TAG: expected an enum value.");
        return OpResult::Fatal;
    }
    auto tag = static_cast<double>(asObjEnum(as<Obj*>(val))->ctor->tag);
    push(Value{tag});
    return OpResult::OK;
}

Runtime::OpResult Runtime::opMatchError(int stopAtFrameCount) {
    return fromThrow(raiseThrowableError("MatchError",
                                         "No matching arm in match expression.",
                                         stopAtFrameCount));
}

void Runtime::opNot() { push(Value{!pop()}); }

void Runtime::opIsSeq() {
    Value val = pop();
    push(Value{isList(val) || isString(val)});
}

void Runtime::opInstanceof(ObjString* className) {
    Value val = pop();
    Value classVal;
    bool result = false;
    if (m_globals.get(className, classVal) && isClass(classVal)) {
        ObjClass* target = asObjClass(as<Obj*>(classVal));
        if (isInstance(val)) {
            ObjClass* klass = asObjInstance(as<Obj*>(val))->klass;
            const ObjClass* found = walkChain<ObjClass>(
                klass, [target](const ObjClass* k) { return k == target; },
                [](const ObjClass* k) { return k->superclass; });
            result = found != nullptr;
        }
    }
    push(Value{result});
}

// --- arithmetic / comparison / containment op*() helpers ------------------
//
// Each one owns its opcode's slow path: VM::run() inlines the double-double
// number fast path (and, for ADD, nothing else — string concat is not a
// number fast path, so it lives here for QBE reuse), so these are reached
// only when that fast path failed. They try the operator-overloading method
// on the operand, then fall back to the same error the opcode raised before
// operator overloading existed.

Runtime::OpResult Runtime::opAdd(int stopAtFrameCount) {
    if (isString(peek(0)) && isString(peek(1))) {
        auto* b_str = asObjString(pop());
        auto* a_str = asObjString(pop());
        std::string result;
        result.reserve(a_str->chars.size() + b_str->chars.size());
        result.append(a_str->chars.data(), a_str->chars.size());
        result.append(b_str->chars.data(), b_str->chars.size());
        push(Value{static_cast<Obj*>(m_mm.makeString(std::move(result)))});
        return OpResult::OK;
    }
    if (auto r = tryBinaryMethod(Protocol::Add, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethod(Protocol::RAdd, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ConcatenationTypeError",
        "Operands must be two numbers, two strings, or a string and a number.",
        stopAtFrameCount));
}

Runtime::OpResult Runtime::opSubtract(int stopAtFrameCount) {
    if (auto r = tryBinaryMethod(Protocol::Sub, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethod(Protocol::RSub, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ArithmeticTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opMultiply(int stopAtFrameCount) {
    if (auto r = tryBinaryMethod(Protocol::Mul, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethod(Protocol::RMul, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ArithmeticTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opDivide(int stopAtFrameCount) {
    if (auto r = tryBinaryMethod(Protocol::Div, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethod(Protocol::RDiv, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ArithmeticTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opModulo(int stopAtFrameCount) {
    // Unlike ADD/SUBTRACT/MULTIPLY/DIVIDE, MODULO's number case is not a plain
    // double-double computation — its floor-division sign correction means the
    // QBE backend does not inline it (notes/qbe-backend.md Q5), so the full
    // number case lives here rather than in VM::run().
    if (is<Number>(peek(0)) && is<Number>(peek(1))) {
        Number b = as<Number>(pop());
        Number a = as<Number>(pop());
        Number result = std::fmod(a, b);
        if (result != 0 && (result < 0) != (b < 0)) {
            result += b;
        }
        push(from<Number>(result));
        return OpResult::OK;
    }
    if (auto r = tryBinaryMethod(Protocol::Mod, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethod(Protocol::RMod, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ArithmeticTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opNegate(int stopAtFrameCount) {
    Value operand = peek(0);
    if (isInstance(operand)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(operand));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Neg)],
                method)) {
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 0,
                                  stopAtFrameCount, ResultCheck::None);
        }
    }
    return fromThrow(raiseThrowableError(
        "ArithmeticTypeError", "Operand must be a number.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opLess(int stopAtFrameCount) {
    Value b = peek(0);
    Value a = peek(1);
    if (isString(a) && isString(b)) {
        // Bytewise lexicographic order; char_traits compares each char as an
        // unsigned byte, matching the JVM backend's ISO-8859-1 char compare.
        bool result = asObjString(as<Obj*>(a))
                          ->chars.compare(asObjString(as<Obj*>(b))->chars) < 0;
        pop();
        pop();
        push(from<bool>(result));
        return OpResult::OK;
    }
    if (auto r = tryBinaryMethodBool(Protocol::Lt, stopAtFrameCount)) {
        return *r;
    }
    if (auto r =
            tryReflectedBinaryMethodBool(Protocol::RLt, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ComparisonTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opGreater(int stopAtFrameCount) {
    Value b = peek(0);
    Value a = peek(1);
    if (isString(a) && isString(b)) {
        bool result = asObjString(as<Obj*>(a))
                          ->chars.compare(asObjString(as<Obj*>(b))->chars) > 0;
        pop();
        pop();
        push(from<bool>(result));
        return OpResult::OK;
    }
    if (auto r = tryBinaryMethodBool(Protocol::Gt, stopAtFrameCount)) {
        return *r;
    }
    if (auto r =
            tryReflectedBinaryMethodBool(Protocol::RGt, stopAtFrameCount)) {
        return *r;
    }
    return fromThrow(raiseThrowableError(
        "ComparisonTypeError", "Operands must be numbers.", stopAtFrameCount));
}

Runtime::OpResult Runtime::opEqual(int stopAtFrameCount) {
    Value b = peek(0);
    Value a = peek(1);
    if (auto r = tryBinaryMethodBool(Protocol::Eq, stopAtFrameCount)) {
        return *r;
    }
    if (auto r = tryReflectedBinaryMethodBool(Protocol::Eq, stopAtFrameCount)) {
        return *r;
    }
    pop();
    pop();
    push(from<bool>(a == b));
    return OpResult::OK;
}

Runtime::OpResult Runtime::opIn(int stopAtFrameCount) {
    Value seq = peek(0);
    Value elem = peek(1);
    if (isList(seq)) {
        auto* list = asObjList(as<Obj*>(seq));
        bool found = false;
        for (const auto& v : list->elements) {
            if (v == elem) {
                found = true;
                break;
            }
        }
        pop();
        pop();
        push(from<bool>(found));
        return OpResult::OK;
    }
    if (isString(seq)) {
        if (!isString(elem)) {
            runtimeError("Left operand of 'in' on a string must be a string.");
            return OpResult::Fatal;
        }
        auto* haystack = asObjString(as<Obj*>(seq));
        auto* needle = asObjString(as<Obj*>(elem));
        bool found =
            haystack->chars.find(needle->chars.data(), 0,
                                 needle->chars.size()) != LoxString::npos;
        pop();
        pop();
        push(from<bool>(found));
        return OpResult::OK;
    }
    if (isMap(seq)) {
        auto* map = asObjMap(as<Obj*>(seq));
        Value dummy{Nil{}};
        bool found = false;
        if (OpResult r = mapGetKey(map, elem, found, dummy, stopAtFrameCount);
            r != OpResult::OK) {
            return r;
        }
        pop();
        pop();
        push(from<bool>(found));
        return OpResult::OK;
    }
    if (isInstance(seq)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(seq));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Contains)],
                method)) {
            // __contains__ lives on the container, which is the RIGHT operand
            // of `x in c`. Swap so the container becomes the receiver (slot 0)
            // and the element the argument (slot 1).
            stackTop[-2] = seq;
            stackTop[-1] = elem;
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 1,
                                  stopAtFrameCount, ResultCheck::Boolean);
        }
    }
    runtimeError("Right operand of 'in' must be a list, string, or map.");
    return OpResult::Fatal;
}

Runtime::OpResult Runtime::opLen(int stopAtFrameCount) {
    // `len(x)` (Op::LEN): pop the operand, push its length. The built-in
    // List/String/Map fast path first, then `__len__` on an Instance.
    Value operand = peek(0);
    if (isList(operand)) {
        auto* list = asObjList(as<Obj*>(operand));
        stackTop[-1] = from<Number>(static_cast<double>(list->elements.size()));
        return OpResult::OK;
    }
    if (isString(operand)) {
        auto* s = asObjString(as<Obj*>(operand));
        stackTop[-1] = from<Number>(static_cast<double>(s->chars.size()));
        return OpResult::OK;
    }
    if (isMap(operand)) {
        auto* map = asObjMap(as<Obj*>(operand));
        stackTop[-1] = from<Number>(static_cast<double>(map->map.count()));
        return OpResult::OK;
    }
    if (isInstance(operand)) {
        ObjInstance* instance = asObjInstance(as<Obj*>(operand));
        Value method;
        if (instance->klass->methods.get(
                m_protocolNames[static_cast<std::size_t>(Protocol::Len)],
                method)) {
            return dispatchMethod(asObjClosure(as<Obj*>(method)), 0,
                                  stopAtFrameCount, ResultCheck::Number);
        }
    }
    runtimeError("len() argument must be a list, string, or map.");
    return OpResult::Fatal;
}

Runtime::OpResult Runtime::opStr(int stopAtFrameCount) {
    // The operand is peeked, not popped, so it stays a GC root while stringify
    // runs (a __str__ dispatch can allocate and collect otherwise-unrooted
    // values). It is replaced in place by the resulting ObjString.
    Value operand = peek(0);
    m_stdlibCtx.clearError();
    // Save/restore the stringify state, not just the boundary: a __str__ may
    // itself call str()/print (a nested opStr), and a failure caught inside
    // that nested call must not leak back into THIS opStr's result.
    int savedBoundary = m_stringifyBoundary;
    m_stringifyBoundary = stopAtFrameCount;
    OpResult savedStatus = m_stringifyStatus;
    m_stringifyStatus = OpResult::OK;
    m_stringifyCanonicalDepth++;
    std::string s = stringify(operand);
    m_stringifyCanonicalDepth--;
    m_stringifyBoundary = savedBoundary;
    if (m_stringifyStatus != OpResult::OK) {
        OpResult status = m_stringifyStatus;
        m_stringifyStatus = savedStatus;
        return status;
    }
    m_stringifyStatus = savedStatus;
    if (m_stdlibCtx.nativeError) {
        runtimeError("%s", m_stdlibCtx.nativeErrorMsg.c_str());
        return OpResult::Fatal;
    }
    ObjString* result = m_mm.makeString(s);
    stackTop[-1] = Value{static_cast<Obj*>(result)};
    return OpResult::OK;
}

std::string Runtime::stringifyInstanceStr(ObjInstance* instance) {
    if (m_stringifyStatus != OpResult::OK) {
        // A previous __str__ in this same stringify already threw (or was
        // caught elsewhere); stop dispatching so the rest of the render is
        // inert and the outcome already recorded propagates unchanged.
        return "...";
    }
    Value method;
    if (!instance->klass->methods.get(
            m_protocolNames[static_cast<std::size_t>(Protocol::Str)], method)) {
        return std::string(instance->klass->name->chars.data(),
                           instance->klass->name->chars.size()) +
               " instance";
    }
    push(Value{static_cast<Obj*>(instance)});
    Value* frameSlots = stackTop - 1;
    int entry = m_frameCount;
    OpResult r = dispatchMethod(asObjClosure(as<Obj*>(method)), 0,
                                m_stringifyBoundary, ResultCheck::String);
    if (r == OpResult::Resumed && m_frameCount == entry + 1) {
        r = runReentrantFrame(entry, frameSlots, m_stringifyBoundary);
    }
    if (r != OpResult::OK) {
        m_stringifyStatus = r;
        return "...";
    }
    Value result = pop();
    auto* s = asObjString(as<Obj*>(result));
    return std::string(s->chars.data(), s->chars.size());
}

Runtime::OpResult Runtime::opGetGlobal(ObjString* name, int stopAtFrameCount) {
    Value value;
    if (!m_globals.get(name, value)) {
        return fromThrow(raiseThrowableError(
            "UndefinedVariableError", "Undefined variable.", stopAtFrameCount));
    }
    push(value);
    return OpResult::OK;
}

Runtime::OpResult Runtime::opSetGlobal(ObjString* name, int stopAtFrameCount) {
    // set() returns true if the key is *new*; an existing key is valid. An
    // entirely new key means the variable was never declared.
    if (m_globals.set(name, peek(0))) {
        m_globals.del(name); // undo the spurious insertion
        return fromThrow(raiseThrowableError(
            "UndefinedVariableError", "Undefined variable.", stopAtFrameCount));
    }
    return OpResult::OK;
}
