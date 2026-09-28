#include "rt_capi.h"
#include "rt_abi.h"
#include "chunk_decoder.h"
#include "runtime.h"
#include "exec_objects.h"

#include <cstdio>
#include <exception>
#include <string>

// Q6: --target qbe requires NaN tagging. A Value must be a single 8-byte
// word for every function above to be a valid C-ABI boundary — see
// rt_capi.h's file comment.
static_assert(sizeof(Value) == 8,
              "libloxrt requires LOXPP_NAN_TAGGING: --target qbe passes "
              "Value across the C ABI as a single 8-byte word");

namespace {

// Every wrapper below funnels a stray C++ exception through this, turning
// it into RT_OP_FATAL instead of calling std::terminate — compiled code has
// no unwinding tables to run a C++ exception through (Q2,
// notes/qbe-backend.md).
//
// Every OTHER path to OpResult::Fatal goes through Runtime::runtimeError(),
// which calls resetStack() before returning, so a caller that observes
// Fatal can always assume stackTop/m_frameCount are already clean. A stray
// C++ exception unwinds straight past whatever partial frame/stack
// bookkeeping its throw point left behind, so this catch restores that
// same invariant itself — the one path to Fatal that does not already run
// through runtimeError() still leaves the Runtime in the state every
// Fatal caller relies on.
template <typename F>
int rtGuard(Runtime* rt, F&& body) noexcept {
    try {
        return static_cast<int>(body());
    } catch (...) {
        rt->resetStack();
        return static_cast<int>(Runtime::OpResult::Fatal);
    }
}

const DecodedFunction* findById(const DecodedFunction& node,
                                const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const DecodedFunction& child : node.nested) {
        if (const DecodedFunction* found = findById(child, id)) {
            return found;
        }
    }
    return nullptr;
}

// S4 (#457), hazard R4 on issue #457: walks the whole rebuilt tree and
// reports the first function with no code attached. See rt_capi.h's own
// comment on rt_startup's requireAllCompiled parameter for why a missing
// desc must fail loudly here instead of surfacing as a dangling,
// never-interpreted CallFrame at whatever later call reaches it.
bool allFunctionsHaveCode(const DecodedFunction& node) {
    if (node.function->code == nullptr) {
        std::fprintf(stderr,
                     "rt_startup: function '%s' has no compiled code "
                     "attached, but requireAllCompiled was set — every "
                     "function in a whole-program --target qbe build must "
                     "be compiled; an interpreted fallback frame would "
                     "never be run to completion.\n",
                     node.id.c_str());
        return false;
    }
    for (const DecodedFunction& child : node.nested) {
        if (!allFunctionsHaveCode(child)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool rt_attach_code(const DecodedFunction& root,
                    const RtFunctionDesc& desc) noexcept {
    const DecodedFunction* node = findById(root, desc.id);
    if (node == nullptr) {
        std::fprintf(stderr,
                     "rt_attach_code: no function with id '%s' in the "
                     "recompiled program.\n",
                     desc.id);
        return false;
    }
    if (node->function->arity != desc.arity) {
        std::fprintf(stderr,
                     "rt_attach_code: function '%s' arity mismatch — "
                     "recompiled %d, code generated against %d.\n",
                     desc.id, node->function->arity, desc.arity);
        return false;
    }
    uint64_t hash = hashChunkBytes(node->function->chunk);
    if (hash != desc.chunkHash) {
        std::fprintf(stderr,
                     "rt_attach_code: function '%s' chunk hash mismatch — "
                     "the embedded source no longer matches the compiled "
                     "code (recompiled 0x%llx, code generated against "
                     "0x%llx).\n",
                     desc.id, static_cast<unsigned long long>(hash),
                     static_cast<unsigned long long>(desc.chunkHash));
        return false;
    }
    node->function->code = desc.code;
    return true;
}

Runtime* rt_startup(const char* source, const RtFunctionDesc* descs,
                    std::size_t nDescs, bool requireAllCompiled) noexcept {
    try {
        auto* rt = new Runtime();
        ObjClosure* closure = rt->loadSource(source != nullptr ? source : "");
        if (closure == nullptr) {
            std::fprintf(stderr, "rt_startup: the embedded program failed to "
                                 "compile.\n");
            delete rt;
            return nullptr;
        }
        DecodedFunction tree = decodeFunctionTree(closure->function);
        for (std::size_t i = 0; i < nDescs; i++) {
            if (!rt_attach_code(tree, descs[i])) {
                delete rt;
                return nullptr;
            }
        }
        if (requireAllCompiled && !allFunctionsHaveCode(tree)) {
            delete rt;
            return nullptr;
        }
        return rt;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rt_startup: %s\n", e.what());
        return nullptr;
    } catch (...) {
        std::fprintf(stderr, "rt_startup: unknown error.\n");
        return nullptr;
    }
}

void rt_shutdown(Runtime* rt) noexcept { delete rt; }

void rt_push(Runtime* rt, Value value) noexcept { rt->push(value); }
Value rt_pop(Runtime* rt) noexcept { return rt->pop(); }
Value rt_peek(Runtime* rt, int distance) noexcept { return rt->peek(distance); }
Value* rt_top(Runtime* rt) noexcept { return rt->top(); }
void rt_set_top(Runtime* rt, Value* top) noexcept { rt->setTop(top); }
Value* rt_stack_base(Runtime* rt) noexcept { return rt->stackBase(); }
int rt_frame_count(Runtime* rt) noexcept { return rt->frameCount(); }

Value rt_new_string(Runtime* rt, const char* chars) noexcept {
    ObjString* s =
        rt->memoryManager().makeString(chars != nullptr ? chars : "");
    return Value{static_cast<Obj*>(s)};
}

Value rt_get_global(Runtime* rt, const char* name) noexcept {
    auto v = rt->getGlobal(name != nullptr ? name : "");
    return v.has_value() ? *v : Value{};
}

int rt_op_print(Runtime* rt) noexcept {
    return rtGuard(rt, [&] {
        // Mirrors Op::PRINT's own body (vm.cpp): clear before stringify()
        // (which can itself call back into stdlib code, e.g. a Map's own
        // to-string), then check after.
        rt->clearNativeError();
        std::string s = stringify(rt->pop());
        std::string errMsg;
        if (rt->takeNativeError(&errMsg)) {
            rt->runtimeError("%s", errMsg.c_str());
            return Runtime::OpResult::Fatal;
        }
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fputc('\n', stdout);
        return Runtime::OpResult::OK;
    });
}

int rt_call(Runtime* rt, int argCount, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opCall(argCount, stopAtFrameCount); });
}

int rt_op_invoke(Runtime* rt, ObjString* name, int argCount,
                 int stopAtFrameCount) noexcept {
    return rtGuard(
        rt, [&] { return rt->opInvoke(name, argCount, stopAtFrameCount); });
}

int rt_op_get_property(Runtime* rt, ObjString* name,
                       int stopAtFrameCount) noexcept {
    return rtGuard(rt,
                   [&] { return rt->opGetProperty(name, stopAtFrameCount); });
}

int rt_op_get_super(Runtime* rt, ObjString* name) noexcept {
    return rtGuard(rt, [&] { return rt->opGetSuper(name); });
}

int rt_op_super_invoke(Runtime* rt, ObjString* name, int argCount,
                       int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] {
        return rt->opSuperInvoke(name, argCount, stopAtFrameCount);
    });
}

int rt_op_inherit(Runtime* rt) noexcept {
    return rtGuard(rt, [&] { return rt->opInherit(); });
}

int rt_op_get_index(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opGetIndex(stopAtFrameCount); });
}

int rt_op_set_index(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opSetIndex(stopAtFrameCount); });
}

int rt_op_get_iter(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opGetIter(stopAtFrameCount); });
}

int rt_op_iter_has_next(Runtime* rt) noexcept {
    return rtGuard(rt, [&] { return rt->opIterHasNext(); });
}

int rt_op_iter_next(Runtime* rt) noexcept {
    return rtGuard(rt, [&] { return rt->opIterNext(); });
}

int rt_op_class(Runtime* rt, ObjString* name) noexcept {
    return rtGuard(rt, [&] {
        rt->opClass(name);
        return Runtime::OpResult::OK;
    });
}

int rt_op_set_property(Runtime* rt, ObjString* name) noexcept {
    return rtGuard(rt, [&] { return rt->opSetProperty(name); });
}

int rt_op_define_method(Runtime* rt, ObjString* name) noexcept {
    return rtGuard(rt, [&] {
        rt->opDefineMethod(name);
        return Runtime::OpResult::OK;
    });
}

int rt_op_build_list(Runtime* rt, int count) noexcept {
    return rtGuard(rt, [&] { return rt->opBuildList(count); });
}

int rt_op_build_map(Runtime* rt, int count, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opBuildMap(count, stopAtFrameCount); });
}

int rt_op_slice(Runtime* rt) noexcept {
    return rtGuard(rt, [&] { return rt->opSlice(); });
}

int rt_op_get_tag(Runtime* rt) noexcept {
    return rtGuard(rt, [&] { return rt->opGetTag(); });
}

int rt_op_match_error(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opMatchError(stopAtFrameCount); });
}

int rt_op_in(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opIn(stopAtFrameCount); });
}

int rt_op_len(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opLen(stopAtFrameCount); });
}

int rt_op_add(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opAdd(stopAtFrameCount); });
}

int rt_op_subtract(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opSubtract(stopAtFrameCount); });
}

int rt_op_multiply(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opMultiply(stopAtFrameCount); });
}

int rt_op_divide(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opDivide(stopAtFrameCount); });
}

int rt_op_modulo(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opModulo(stopAtFrameCount); });
}

int rt_op_negate(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opNegate(stopAtFrameCount); });
}

int rt_op_less(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opLess(stopAtFrameCount); });
}

int rt_op_greater(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opGreater(stopAtFrameCount); });
}

int rt_op_equal(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opEqual(stopAtFrameCount); });
}

int rt_op_define_global(Runtime* rt, ObjString* name) noexcept {
    return rtGuard(rt, [&] {
        rt->opDefineGlobal(name);
        return Runtime::OpResult::OK;
    });
}

int rt_op_get_global(Runtime* rt, ObjString* name,
                     int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opGetGlobal(name, stopAtFrameCount); });
}

int rt_op_set_global(Runtime* rt, ObjString* name,
                     int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] { return rt->opSetGlobal(name, stopAtFrameCount); });
}

Value rt_current_closure(Runtime* rt) noexcept {
    return Value{static_cast<Obj*>(rt->currentClosure())};
}

Value rt_constant_at(Runtime*, Value closure, int constantIndex) noexcept {
    return asObjClosure(closure)->function->chunk.getConstant(
        static_cast<uint16_t>(constantIndex));
}

Value rt_new_closure(Runtime* rt, Value functionConstant) noexcept {
    ObjFunction* fn = asObjFunction(functionConstant);
    auto* cl = rt->memoryManager().create<ObjClosure>(fn);
    return Value{static_cast<Obj*>(cl)};
}

void rt_capture_local_upvalue(Runtime* rt, Value closure, int upvalueIndex,
                              Value* localSlot) noexcept {
    asObjClosure(closure)->upvalues[static_cast<std::size_t>(upvalueIndex)] =
        rt->captureUpvalue(localSlot);
}

void rt_forward_upvalue(Runtime*, Value closure, int upvalueIndex,
                        Value parentClosure, int parentUpvalueIndex) noexcept {
    asObjClosure(closure)->upvalues[static_cast<std::size_t>(upvalueIndex)] =
        asObjClosure(parentClosure)
            ->upvalues[static_cast<std::size_t>(parentUpvalueIndex)];
}

Value rt_get_upvalue(Runtime*, Value closure, int index) noexcept {
    return *asObjClosure(closure)
                ->upvalues[static_cast<std::size_t>(index)]
                ->location;
}

void rt_set_upvalue(Runtime*, Value closure, int index, Value v) noexcept {
    *asObjClosure(closure)
         ->upvalues[static_cast<std::size_t>(index)]
         ->location = v;
}

void rt_close_upvalues(Runtime* rt, Value* last) noexcept {
    rt->closeUpvalues(last);
}

int rt_check_stack(Runtime* rt, Value* neededTop,
                   int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] {
        return rt->checkStackOverflow(neededTop, stopAtFrameCount);
    });
}

void rt_push_handler(Runtime* rt, Value* checkpointTop) noexcept {
    // catchIp is meaningful only to the interpreter's own dispatch loop
    // (qbe_emitter.cpp's own file comment explains why compiled code never
    // reads it back) — this frame's own chunk-begin is a harmless, always-
    // valid placeholder.
    rt->pushHandler(checkpointTop,
                    rt->currentClosure()->function->chunk.cbegin());
}

int rt_pop_handler(Runtime* rt) noexcept {
    return rtGuard(rt, [&] {
        if (rt->handlerStackDepth() == 0) {
            rt->runtimeError("BUG: POP_HANDLER with empty handler stack.");
            return Runtime::OpResult::Fatal;
        }
        rt->popTopHandler();
        return Runtime::OpResult::OK;
    });
}

int rt_throw(Runtime* rt, Value thrownValue, int stopAtFrameCount) noexcept {
    return rtGuard(
        rt, [&] { return rt->handleThrowOp(thrownValue, stopAtFrameCount); });
}

int rt_op_defer_record(Runtime* rt, int argCount) noexcept {
    return rtGuard(rt, [&] {
        rt->opDeferRecord(argCount);
        return Runtime::OpResult::OK;
    });
}

int rt_run_defers(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard(rt, [&] {
        int frameIndex = rt->frameCount() - 1;
        rt->popHandlersOwnedByCurrentFrame();
        InterpretResult result =
            rt->runPendingDefers(frameIndex, stopAtFrameCount);
        if (result != InterpretResult::OK) {
            // Hard error during a deferred call, already reported.
            return Runtime::OpResult::Fatal;
        }
        if (rt->frameCount() != frameIndex + 1) {
            // A deferred call's own throw propagated past this frame (or
            // past a still-live caller) — this frame's own handlers are
            // already gone (popHandlersOwnedByCurrentFrame above), so
            // there is no local catch to attempt here; only propagate.
            return (rt->frameCount() > stopAtFrameCount)
                       ? Runtime::OpResult::Resumed
                       : Runtime::OpResult::Stop;
        }
        return Runtime::OpResult::OK;
    });
}

void rt_set_frame_offset(Runtime* rt, int offset) noexcept {
    rt->setCurrentFrameOffset(offset);
}
