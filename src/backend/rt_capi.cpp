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
template <typename F>
int rtGuard(F&& body) noexcept {
    try {
        return static_cast<int>(body());
    } catch (...) {
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
                    std::size_t nDescs) noexcept {
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
    return rtGuard([&] {
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

int rt_call(Runtime* rt, int argCount) noexcept {
    return rtGuard([&] { return rt->opCall(argCount, 0); });
}

int rt_op_invoke(Runtime* rt, ObjString* name, int argCount,
                 int stopAtFrameCount) noexcept {
    return rtGuard(
        [&] { return rt->opInvoke(name, argCount, stopAtFrameCount); });
}

int rt_op_get_property(Runtime* rt, ObjString* name,
                       int stopAtFrameCount) noexcept {
    return rtGuard([&] { return rt->opGetProperty(name, stopAtFrameCount); });
}

int rt_op_get_super(Runtime* rt, ObjString* name) noexcept {
    return rtGuard([&] { return rt->opGetSuper(name); });
}

int rt_op_super_invoke(Runtime* rt, ObjString* name, int argCount,
                       int stopAtFrameCount) noexcept {
    return rtGuard(
        [&] { return rt->opSuperInvoke(name, argCount, stopAtFrameCount); });
}

int rt_op_inherit(Runtime* rt) noexcept {
    return rtGuard([&] { return rt->opInherit(); });
}

int rt_op_get_index(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard([&] { return rt->opGetIndex(stopAtFrameCount); });
}

int rt_op_set_index(Runtime* rt, int stopAtFrameCount) noexcept {
    return rtGuard([&] { return rt->opSetIndex(stopAtFrameCount); });
}

int rt_op_get_iter(Runtime* rt) noexcept {
    return rtGuard([&] { return rt->opGetIter(); });
}

int rt_op_iter_has_next(Runtime* rt) noexcept {
    return rtGuard([&] { return rt->opIterHasNext(); });
}

int rt_op_iter_next(Runtime* rt) noexcept {
    return rtGuard([&] { return rt->opIterNext(); });
}
