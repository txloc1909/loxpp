#include "vm.h"
#include "debug.h"
#include "objects.h"
#include "memory_manager.h"
#include "object.h"
#include "scanner.h"
#include "compiler.h"
#include "utility.h"

#include "stdlib/stdlib_context.h"
#include "stdlib/stdlib_registrar.h"
#include "stdlib/globals.h"
#include "stdlib/file_api.h"
#include "stdlib/map_api.h"
#include "stdlib/math_module.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

InterpretResult VM::interpret(const std::string& source) {
    // Guard against dangling class pointers from a prior VM instance. GC can
    // fire inside compile(), and markRoots() must not dereference a pointer
    // that was freed when the previous VM's MemoryManager was destroyed.
    m_fileClass = nullptr;
    m_mapClass = nullptr;
    ObjFunction* fn = compile(source, &m_mm);
    if (fn == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    // Root fn on the stack before any allocation (defineNatives,
    // create<ObjClosure>) can trigger GC. Without this, fn is unreachable
    // between compile() returning and push(closure) — the Compiler has already
    // been destroyed and m_currentCompiler is nullptr.
    push(Value{static_cast<Obj*>(fn)});
    m_stdlibCtx.mm = &m_mm;
    setActiveContext(&m_stdlibCtx);
    defineNatives();
    ObjClosure* closure = m_mm.create<ObjClosure>(fn);
    stackTop[-1] = Value{
        static_cast<Obj*>(closure)}; // replace fn with its closure in-place
    call(closure, 0);
#ifdef LOXPP_PROFILE
    // Count the implicit script call so Op::CALL and Op::RETURN stay balanced.
    m_profilerData.opcodeTable[static_cast<uint8_t>(Op::CALL)].count++;
    ProfileProgramScope programScope(m_profilerData);
#endif
    return run();
}

Byte VM::readByte() { return *m_frames[m_frameCount - 1].ip++; }

uint16_t VM::readShort() {
    uint16_t hi = readByte();
    uint16_t lo = readByte();
    return static_cast<uint16_t>((hi << 8) | lo);
}

Value VM::readConstant() {
    return m_frames[m_frameCount - 1].closure->function->chunk.getConstant(
        readShort());
}

Value VM::lastResult() const { return m_lastResult; }

std::optional<Value> VM::getGlobal(const std::string& name) const {
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

bool VM::call(ObjClosure* closure, int argCount) {
    ObjFunction* fn = closure->function;
    if (argCount != fn->arity) {
        runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
        return false;
    }
    if (m_frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");
        return false;
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
    return true;
}

ObjUpvalue* VM::captureUpvalue(Value* local) {
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

void VM::closeUpvalues(Value* last) {
    while (m_openUpvalues != nullptr && m_openUpvalues->location >= last) {
        ObjUpvalue* uv = m_openUpvalues;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        m_openUpvalues = uv->next;
    }
}

InterpretResult VM::run() {
#define BINARY_OP(valueType, op)                                               \
    do {                                                                       \
        if (!is<Number>(peek(0)) || !is<Number>(peek(1))) {                    \
            runtimeError("Operands must be numbers.");                         \
            return InterpretResult::RUNTIME_ERROR;                             \
        }                                                                      \
        Number b = as<Number>(pop());                                          \
        Number a = as<Number>(pop());                                          \
        push(as<valueType>(a op b));                                           \
    } while (false)

    CallFrame* frame = &m_frames[m_frameCount - 1];

    for (;;) {
#ifdef LOXPP_DEBUG_TRACE_EXECUTION
        {
            const Chunk& chunk = frame->closure->function->chunk;
            int currentOffset = static_cast<int>(frame->ip - chunk.cbegin());
            bool color = isatty(STDOUT_FILENO) != 0;
            std::printf("[line %d] ", chunk.getLine(currentOffset));
            std::printf("          ");
            for (Value* slot = stack; slot < stackTop; slot++) {
                std::printf("[ ");
                printValue(*slot);
                std::printf(" ]");
            }
            std::printf("\n");
            disassembleInstruction(chunk, m_mm, currentOffset, std::cout,
                                   color);
        }
#endif

        Byte instruction = readByte();
#ifdef LOXPP_PROFILE
        m_profilerData.opcodeTable[instruction].count++;
#endif
        switch (toOpcode(instruction)) {
        case Op::CONSTANT: {
            push(readConstant());
            break;
        }
        case Op::NIL: {
            push(from<Nil>(Nil{}));
            break;
        }
        case Op::TRUE: {
            push(from<bool>(true));
            break;
        }
        case Op::FALSE: {
            push(from<bool>(false));
            break;
        }
        case Op::EQUAL: {
            Value b = pop();
            Value a = pop();
            push(from<bool>(a == b));
            break;
        }
        case Op::GREATER: {
            BINARY_OP(bool, >);
            break;
        }
        case Op::LESS: {
            BINARY_OP(bool, <);
            break;
        }
        case Op::NEGATE: {
            if (!is<Number>(peek(0))) {
                runtimeError("Operand must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            push(from<Number>(-as<Number>(pop())));
            break;
        }
        case Op::ADD: {
            if (isString(peek(0)) && isString(peek(1))) {
                auto* b_str = asObjString(pop());
                auto* a_str = asObjString(pop());
                std::string result;
                result.reserve(a_str->chars.size() + b_str->chars.size());
                result.append(a_str->chars.data(), a_str->chars.size());
                result.append(b_str->chars.data(), b_str->chars.size());
                push(Value{
                    static_cast<Obj*>(m_mm.makeString(std::move(result)))});
            } else {
                BINARY_OP(Number, +);
            }
            break;
        }
        case Op::SUBTRACT: {
            BINARY_OP(Number, -);
            break;
        }
        case Op::MULTIPLY: {
            BINARY_OP(Number, *);
            break;
        }
        case Op::DIVIDE: {
            BINARY_OP(Number, /);
            break;
        }
        case Op::MODULO: {
            if (!is<Number>(peek(0)) || !is<Number>(peek(1))) {
                runtimeError("Operands must be numbers.");
                return InterpretResult::RUNTIME_ERROR;
            }
            Number b = as<Number>(pop());
            Number a = as<Number>(pop());
            Number result = std::fmod(a, b);
            // Floor-division semantics: result has same sign as b (Python/Lua
            // behavior)
            if (result != 0 && (result < 0) != (b < 0)) {
                result += b;
            }
            push(from<Number>(result));
            break;
        }
        case Op::NOT: {
            push(from<bool>(!pop()));
            break;
        }
        case Op::PRINT: {
            printValue(pop());
            std::printf("\n");
            break;
        }
        case Op::POP: {
            m_lastResult = pop();
            break;
        }
        case Op::GET_LOCAL: {
            uint8_t slot = readByte();
            push(frame->slots[slot]);
            break;
        }
        case Op::SET_LOCAL: {
            uint8_t slot = readByte();
            // assignment is an expression; leave value on stack
            frame->slots[slot] = peek(0);
            break;
        }
        case Op::DEFINE_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            m_globals.set(name, peek(0));
            pop();
            break;
        }
        case Op::GET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            Value value;
            if (!m_globals.get(name, value)) {
                runtimeError("Undefined variable '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            push(value);
            break;
        }
        case Op::SET_GLOBAL: {
            ObjString* name = asObjString(readConstant());
            // set() returns true if the key is *new*; an existing key is
            // valid. An entirely new key means the variable was never
            // declared.
            if (m_globals.set(name, peek(0))) {
                m_globals.del(name); // undo the spurious insertion
                runtimeError("Undefined variable '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::JUMP: {
            frame->ip += readShort();
            break;
        }
        case Op::JUMP_IF_FALSE: {
            uint16_t offset = readShort();
            if (isFalsy(peek(0))) {
                frame->ip += offset;
            }
            break;
        }
        case Op::LOOP: {
            frame->ip -= readShort();
            break;
        }
        case Op::MATCH_ERROR: {
            runtimeError("MatchError: no matching arm.");
            return InterpretResult::RUNTIME_ERROR;
        }
        case Op::JUMP_TABLE: {
            uint8_t minTag = readByte();
            uint8_t count = readByte();
            auto tableBase = frame->ip; // iterator to entry[0]
            frame->ip += static_cast<int>(count) * 2;
            Value tagVal = pop();
            int tag = static_cast<int>(as<Number>(tagVal));
            int idx = tag - static_cast<int>(minTag);
            if (idx >= 0 && idx < static_cast<int>(count)) {
                uint16_t fwd = static_cast<uint16_t>((tableBase[idx * 2] << 8) |
                                                     tableBase[(idx * 2) + 1]);
                frame->ip += fwd;
            }
            break;
        }
        case Op::GET_TAG: {
            Value val = pop();
            if (!isEnumValue(val)) {
                runtimeError("GET_TAG: expected an enum value.");
                return InterpretResult::RUNTIME_ERROR;
            }
            auto tag = static_cast<double>(asObjEnum(as<Obj*>(val))->ctor->tag);
            push(Value{tag});
            break;
        }
        case Op::IS_SEQ: {
            Value val = pop();
            push(Value{isList(val) || isString(val)});
            break;
        }
        case Op::INSTANCEOF: {
            ObjString* className = asObjString(readConstant());
            Value val = pop();
            Value classVal;
            bool result = false;
            if (m_globals.get(className, classVal) && isClass(classVal)) {
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
            push(Value{result});
            break;
        }
        case Op::CALL: {
            int argCount = readByte();
            Value callee = peek(argCount);
            if (isNative(callee)) {
                if (!callNative(asObjNative(callee), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                frame = &m_frames[m_frameCount - 1];
            } else if (isClosure(callee)) {
                if (!call(asObjClosure(callee), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                frame = &m_frames[m_frameCount - 1];
            } else if (isBoundMethod(callee)) {
                ObjBoundMethod* bound = asObjBoundMethod(as<Obj*>(callee));
                // Slot 0 of the new frame = receiver (= this).
                stackTop[-argCount - 1] = bound->receiver;
                if (!call(bound->method, argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
                frame = &m_frames[m_frameCount - 1];
            } else if (isClass(callee)) {
                ObjClass* klass = asObjClass(as<Obj*>(callee));
                ObjInstance* instance =
                    m_mm.create<ObjInstance>(klass, VmAllocator<Entry>{&m_mm});
                stackTop[-argCount - 1] = Value{static_cast<Obj*>(instance)};
                // Call init() if the class defines one.
                ObjString* initStr = m_mm.findString("init");
                Value initMethod;
                if (initStr && klass->methods.get(initStr, initMethod)) {
                    if (!call(asObjClosure(as<Obj*>(initMethod)), argCount)) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    frame = &m_frames[m_frameCount - 1];
                } else if (argCount != 0) {
                    runtimeError("Expected 0 arguments but got %d.", argCount);
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isEnumCtor(callee)) {
                ObjEnumCtor* ctor = asObjEnumCtor(as<Obj*>(callee));
                if (argCount != static_cast<int>(ctor->arity)) {
                    runtimeError("'%s' expects %d argument(s) but got %d.",
                                 ctor->ctorName->chars.c_str(),
                                 static_cast<int>(ctor->arity), argCount);
                    return InterpretResult::RUNTIME_ERROR;
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
            } else {
                runtimeError("Can only call functions, classes and enums.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::CLASS: {
            ObjString* name = asObjString(readConstant());
            ObjClass* klass =
                m_mm.create<ObjClass>(name, VmAllocator<Entry>{&m_mm});
            push(Value{static_cast<Obj*>(klass)});
            break;
        }
        case Op::GET_PROPERTY: {
            if (isFile(peek(0))) {
                ObjString* name = asObjString(readConstant());
                Value method;
                if (!m_fileClass->methods.get(name, method)) {
                    runtimeError("Undefined property '%s' on file.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                pop();        // file
                push(method); // ObjNative (unbound)
                break;
            }
            if (isMap(peek(0))) {
                ObjString* name = asObjString(readConstant());
                Value method;
                if (!m_mapClass->methods.get(name, method)) {
                    runtimeError("Undefined property '%s' on map.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                pop();        // map
                push(method); // ObjNative (unbound)
                break;
            }
            if (!isInstance(peek(0))) {
                runtimeError("Only instances have properties.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjInstance* instance = asObjInstance(as<Obj*>(peek(0)));
            ObjString* name = asObjString(readConstant());
            Value value;
            if (instance->fields.get(name, value)) {
                pop(); // instance
                push(value);
                break;
            }
            if (!bindMethod(instance->klass, name)) {
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::SET_PROPERTY: {
            if (!isInstance(peek(1))) {
                runtimeError("Only instances have fields.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjInstance* instance = asObjInstance(as<Obj*>(peek(1)));
            ObjString* name = asObjString(readConstant());
            instance->fields.set(name, peek(0));
            Value val = pop(); // value
            pop();             // instance
            push(val);         // assignment is an expression
            break;
        }
        case Op::DEFINE_METHOD: {
            ObjString* name = asObjString(readConstant());
            Value method = peek(0); // ObjClosure* on top
            ObjClass* klass = asObjClass(as<Obj*>(peek(1))); // class below
            klass->methods.set(name, method);
            pop(); // pop closure; leave class on stack for next method
            break;
        }
        case Op::INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            Value receiver = peek(argCount);
            if (isInstance(receiver)) {
                ObjInstance* instance = asObjInstance(as<Obj*>(receiver));
                // A field can shadow a method — check fields first.
                Value fieldVal;
                if (instance->fields.get(name, fieldVal)) {
                    stackTop[-argCount - 1] = fieldVal;
                    if (isClosure(fieldVal)) {
                        if (!call(asObjClosure(as<Obj*>(fieldVal)), argCount)) {
                            return InterpretResult::RUNTIME_ERROR;
                        }
                    } else if (isNative(fieldVal)) {
                        if (!callNative(asObjNative(as<Obj*>(fieldVal)),
                                        argCount)) {
                            return InterpretResult::RUNTIME_ERROR;
                        }
                    } else {
                        runtimeError(
                            "Can only call functions, classes and enums.");
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    frame = &m_frames[m_frameCount - 1];
                    break;
                }
                // Fast path: call the method directly — receiver already sits
                // at stackTop[-argCount-1], which becomes slot 0 (= this) of
                // the new frame.
                Value method;
                if (!instance->klass->methods.get(name, method)) {
                    runtimeError("Undefined property '%s'.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                Obj* methodObj = as<Obj*>(method);
                if (isObjNative(methodObj)) {
                    if (!callNative(asObjNative(methodObj), argCount)) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                } else {
                    if (!call(asObjClosure(methodObj), argCount)) {
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    frame = &m_frames[m_frameCount - 1];
                }
            } else if (isList(receiver)) {
                ObjList* list = asObjList(as<Obj*>(receiver));
                if (name->chars == "append") {
                    if (argCount != 1) {
                        runtimeError("'append' expects 1 argument but got %d.",
                                     argCount);
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    Value val =
                        peek(0); // still on stack — GC-safe during push_back
                    list->elements.push_back(val);
                    pop(); // arg
                    pop(); // receiver
                    push(from<Nil>(Nil{}));
                } else if (name->chars == "pop") {
                    if (argCount != 0) {
                        runtimeError("'pop' expects 0 arguments but got %d.",
                                     argCount);
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    if (list->elements.empty()) {
                        runtimeError("Cannot pop from an empty list.");
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    Value val = list->elements.back();
                    list->elements.pop_back();
                    pop(); // receiver
                    push(val);
                } else if (name->chars == "remove") {
                    if (argCount != 1) {
                        runtimeError("'remove' expects 1 argument but got %d.",
                                     argCount);
                        return InterpretResult::RUNTIME_ERROR;
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
                        return InterpretResult::RUNTIME_ERROR;
                    }
                    pop(); // arg
                    pop(); // receiver
                    push(from<Nil>(Nil{}));
                } else {
                    runtimeError("Undefined method '%s' on list.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isFile(receiver)) {
                Value method;
                if (!m_fileClass->methods.get(name, method)) {
                    runtimeError("Undefined method '%s' on file.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                if (!callNative(asObjNative(as<Obj*>(method)), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else if (isMap(receiver)) {
                Value method;
                if (!m_mapClass->methods.get(name, method)) {
                    runtimeError("Undefined method '%s' on map.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
                if (!callNative(asObjNative(as<Obj*>(method)), argCount)) {
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else {
                runtimeError("Only instances, files, and maps have methods.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::INHERIT: {
            Value superVal = peek(1);
            if (!isClass(superVal)) {
                runtimeError("Superclass must be a class.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjClass* superclass = asObjClass(as<Obj*>(superVal));
            ObjClass* subclass = asObjClass(as<Obj*>(peek(0)));
            subclass->methods.addAll(superclass->methods);
            subclass->superclass = superclass;
            pop(); // pop subclass; superclass stays as "super" local
            break;
        }
        case Op::GET_SUPER: {
            ObjString* name = asObjString(readConstant());
            ObjClass* superclass = asObjClass(as<Obj*>(pop()));
            if (!bindMethod(superclass, name)) {
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::SUPER_INVOKE: {
            ObjString* name = asObjString(readConstant());
            int argCount = readByte();
            ObjClass* superclass = asObjClass(as<Obj*>(pop()));
            Value method;
            if (!superclass->methods.get(name, method)) {
                runtimeError("Undefined property '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!call(asObjClosure(as<Obj*>(method)), argCount)) {
                return InterpretResult::RUNTIME_ERROR;
            }
            frame = &m_frames[m_frameCount - 1];
            break;
        }
        case Op::CLOSURE: {
            ObjFunction* fn = asObjFunction(readConstant());
            ObjClosure* cl = m_mm.create<ObjClosure>(fn);
            push(Value{static_cast<Obj*>(cl)});
            for (int i = 0; i < fn->upvalueCount; i++) {
                uint8_t isLocal = readByte();
                uint8_t index = readByte();
                if (isLocal) {
                    cl->upvalues[i] = captureUpvalue(frame->slots + index);
                } else {
                    cl->upvalues[i] = frame->closure->upvalues[index];
                }
            }
            break;
        }
        case Op::GET_UPVALUE: {
            uint8_t slot = readByte();
            push(*frame->closure->upvalues[slot]->location);
            break;
        }
        case Op::SET_UPVALUE: {
            uint8_t slot = readByte();
            *frame->closure->upvalues[slot]->location = peek(0);
            break;
        }
        case Op::CLOSE_UPVALUE: {
            closeUpvalues(stackTop - 1);
            pop();
            break;
        }
        case Op::RETURN: {
            Value result = pop();
            closeUpvalues(frame->slots);
#ifdef LOXPP_PROFILE
            // Destroy the function scope before decrementing frameCount so the
            // depth index still points to this frame's slot.
            m_profilerScopes[m_frameCount - 1].reset();
#endif
            m_frameCount--;
            if (m_frameCount == 0) {
                // Finished executing the top-level script.
                pop(); // remove the script ObjClosure from the stack
                return InterpretResult::OK;
            }
            // Discard the callee's stack window and push return value.
            stackTop = frame->slots;
            push(result);
            frame = &m_frames[m_frameCount - 1];
            break;
        }
        case Op::BUILD_LIST: {
            uint8_t count = readByte();
            ObjList* list = m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
            m_mm.pushTempRoot(list); // protect across resize's potential GC
            list->elements.resize(count);
            for (int i = count - 1; i >= 0; i--) {
                list->elements[i] = pop();
            }
            m_mm.popTempRoot();
            push(Value{static_cast<Obj*>(list)});
            break;
        }
        case Op::BUILD_MAP: {
            uint8_t count = readByte();
            // Validate all keys before any allocation. Stack (top to bottom):
            //   val_{n-1}, key_{n-1}, ..., val_0, key_0
            for (int i = 0; i < count; i++) {
                Value key = peek(2 * (count - 1 - i) + 1);
                if (!isValidMapKey(key)) {
                    runtimeError(
                        "Map keys must be Bool, Number, Nil, or String. "
                        "NaN is not allowed.");
                    return InterpretResult::RUNTIME_ERROR;
                }
            }
            ObjMap* map =
                m_mm.create<ObjMap>(m_mapClass, VmAllocator<MapEntry>{&m_mm});
            // Values are still on the stack → GC-rooted; map is temp-rooted
            // so it survives any GC triggered by mapSet's grow.
            m_mm.pushTempRoot(map);
            for (int i = 0; i < count; i++) {
                Value key = peek(2 * (count - 1 - i) + 1);
                Value val = peek(2 * (count - 1 - i));
                map->mapSet(key, val);
            }
            m_mm.popTempRoot();
            for (int i = 0; i < 2 * count; i++) {
                pop();
            }
            push(Value{static_cast<Obj*>(map)});
            break;
        }
        case Op::GET_INDEX: {
            Value indexVal = pop();
            Value collectionVal = pop();
            if (isList(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    runtimeError("List index must be a number.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                double n = as<Number>(indexVal);
                if (n != std::floor(n)) {
                    runtimeError("List index must be an integer.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* list = asObjList(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
                    runtimeError("List index out of bounds.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                push(list->elements[idx]);
            } else if (isString(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    runtimeError("String index must be a number.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                double n = as<Number>(indexVal);
                if (n != std::floor(n)) {
                    runtimeError("String index must be an integer.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* str = asObjString(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(str->chars.size())) {
                    runtimeError("String index out of bounds.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                // Copy char before makeString (GC-safe: same pattern as ADD)
                char ch = str->chars[idx];
                push(Value{static_cast<Obj*>(
                    m_mm.makeString(std::string_view{&ch, 1}))});
            } else if (isMap(collectionVal)) {
                if (!isValidMapKey(indexVal)) {
                    runtimeError(
                        "Map keys must be Bool, Number, Nil, or String. "
                        "NaN is not allowed.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* map = asObjMap(as<Obj*>(collectionVal));
                Value result{Nil{}}; // default nil — returned when key absent
                map->mapGet(indexVal, result);
                push(result);
            } else if (isEnumValue(collectionVal)) {
                if (!is<Number>(indexVal)) {
                    runtimeError("Enum field index must be a number.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                double n = as<Number>(indexVal);
                auto* e = asObjEnum(as<Obj*>(collectionVal));
                int idx = static_cast<int>(n);
                if (idx < 0 || idx >= static_cast<int>(e->fields.size())) {
                    runtimeError("Enum field index %d out of range.", idx);
                    return InterpretResult::RUNTIME_ERROR;
                }
                push(e->fields[static_cast<size_t>(idx)]);
            } else {
                runtimeError("Only lists, strings, and maps can be indexed.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::SET_INDEX: {
            Value val = pop();
            Value indexVal = pop();
            Value listVal = pop();
            if (isString(listVal)) {
                runtimeError("Strings are immutable and cannot be indexed for "
                             "assignment.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (isMap(listVal)) {
                if (!isValidMapKey(indexVal)) {
                    runtimeError(
                        "Map keys must be Bool, Number, Nil, or String. "
                        "NaN is not allowed.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* map = asObjMap(as<Obj*>(listVal));
                // Root the map: it was popped and may be a temporary; mapSet
                // can grow the bucket array which triggers GC.
                m_mm.pushTempRoot(map);
                // Root val if it's an object: it was popped off the stack
                // before mapSet, so the GC won't find it through the stack.
                if (is<Obj*>(val)) {
                    m_mm.pushTempRoot(as<Obj*>(val));
                }
                map->mapSet(indexVal, val);
                if (is<Obj*>(val)) {
                    m_mm.popTempRoot();
                }
                m_mm.popTempRoot();
                push(val);
                break;
            }
            if (!isList(listVal)) {
                runtimeError(
                    "Only lists and maps can be indexed for assignment.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(indexVal)) {
                runtimeError("List index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double n = as<Number>(indexVal);
            if (n != std::floor(n)) {
                runtimeError("List index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            auto* list = asObjList(as<Obj*>(listVal));
            int idx = static_cast<int>(n);
            if (idx < 0 || idx >= static_cast<int>(list->elements.size())) {
                runtimeError("List index out of bounds.");
                return InterpretResult::RUNTIME_ERROR;
            }
            list->elements[idx] = val;
            push(val); // assignment is an expression; its value is the assigned
                       // value
            break;
        }
        case Op::SLICE: {
            // Stack (bottom→top): seq, start, end
            Value endVal = peek(0);
            Value startVal = peek(1);
            Value seqVal = peek(2);

            if (!isList(seqVal) && !isString(seqVal)) {
                runtimeError("Slice requires a List or String.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(startVal)) {
                runtimeError("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double startD = as<Number>(startVal);
            if (startD != std::floor(startD)) {
                runtimeError("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (startD < 0.0) {
                runtimeError("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!is<Number>(endVal)) {
                runtimeError("Slice index must be a number.");
                return InterpretResult::RUNTIME_ERROR;
            }
            double endD = as<Number>(endVal);
            if (endD != std::floor(endD)) {
                runtimeError("Slice index must be an integer.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (endD < 0.0) {
                runtimeError("Slice index must be non-negative.");
                return InterpretResult::RUNTIME_ERROR;
            }

            if (isList(seqVal)) {
                auto* src = asObjList(as<Obj*>(seqVal));
                int n = static_cast<int>(src->elements.size());
                int s = static_cast<int>(std::min(startD, (double)n));
                int e = static_cast<int>(std::min(endD, (double)n));
                int count = (s < e) ? e - s : 0;

                ObjList* result =
                    m_mm.create<ObjList>(VmAllocator<Value>{&m_mm});
                // seqVal is still at peek(2) → src is GC-rooted on the stack
                m_mm.pushTempRoot(result);
                result->elements.resize(count); // may trigger GC
                src =
                    asObjList(as<Obj*>(peek(2))); // re-read after potential GC
                for (int i = 0; i < count; i++) {
                    result->elements[i] = src->elements[s + i];
                }
                m_mm.popTempRoot();
                pop();
                pop();
                pop();
                push(Value{static_cast<Obj*>(result)});
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
                pop();
                pop();
                pop();
                push(Value{
                    static_cast<Obj*>(m_mm.makeString(std::move(substr)))});
            }
            break;
        }
        case Op::IN: {
            Value seq = pop();
            Value elem = pop();
            if (isList(seq)) {
                auto* list = asObjList(as<Obj*>(seq));
                bool found = false;
                for (const auto& v : list->elements) {
                    if (v == elem) {
                        found = true;
                        break;
                    }
                }
                push(from<bool>(found));
            } else if (isString(seq)) {
                if (!isString(elem)) {
                    runtimeError(
                        "Left operand of 'in' on a string must be a string.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* haystack = asObjString(as<Obj*>(seq));
                auto* needle = asObjString(as<Obj*>(elem));
                bool found = haystack->chars.find(needle->chars.data(), 0,
                                                  needle->chars.size()) !=
                             LoxString::npos;
                push(from<bool>(found));
            } else if (isMap(seq)) {
                if (!isValidMapKey(elem)) {
                    runtimeError(
                        "Map keys must be Bool, Number, Nil, or String. "
                        "NaN is not allowed.");
                    return InterpretResult::RUNTIME_ERROR;
                }
                auto* map = asObjMap(as<Obj*>(seq));
                Value dummy;
                push(from<bool>(map->mapGet(elem, dummy)));
            } else {
                runtimeError(
                    "Right operand of 'in' must be a list, string, or map.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::GET_ITER: {
            // peek(0) keeps iterable on stack as GC root during create<>().
            // Mirrors the class-instantiation pattern at vm.cpp:556-558.
            Value iterable = peek(0);
            if (!isList(iterable) && !isString(iterable) && !isMap(iterable)) {
                runtimeError(
                    "Value is not iterable (expected list, string, or map).");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjIterator* it = m_mm.create<ObjIterator>(iterable, 0);
            stackTop[-1] = Value{static_cast<Obj*>(it)}; // replace in-place
            break;
        }
        case Op::ITER_HAS_NEXT: {
            Value top = pop();
            // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
            if (!isIterator(top)) {
                runtimeError(
                    "BUG: ITER_HAS_NEXT expects an iterator on the stack.");
                return InterpretResult::RUNTIME_ERROR;
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
                // Scan forward from current index for the next occupied bucket.
                auto* map = asObjMap(as<Obj*>(it->collection));
                int i = it->index;
                while (i < map->map.capacity() &&
                       map->map.entryAt(i)->state != MapSlot::OCCUPIED) {
                    ++i;
                }
                has = i < map->map.capacity();
            } else {
                runtimeError(
                    "BUG: ObjIterator::collection has unexpected type.");
                return InterpretResult::RUNTIME_ERROR;
            }
            push(from<bool>(has));
            break;
        }
        case Op::ITER_NEXT: {
            Value top = pop();
            // Invariant: value must be an ObjIterator (guaranteed by GET_ITER).
            if (!isIterator(top)) {
                runtimeError(
                    "BUG: ITER_NEXT expects an iterator on the stack.");
                return InterpretResult::RUNTIME_ERROR;
            }
            ObjIterator* it = asObjIterator(as<Obj*>(top));
            if (isList(it->collection)) {
                push(
                    asObjList(as<Obj*>(it->collection))->elements[it->index++]);
            } else if (isString(it->collection)) {
                char ch =
                    asObjString(as<Obj*>(it->collection))->chars[it->index++];
                // ObjIterator is GC-rooted at iterSlot on VM stack; GC is
                // non-moving. ch is a plain char copied before makeString
                // (which may trigger GC).
                push(Value{static_cast<Obj*>(
                    m_mm.makeString(std::string_view{&ch, 1}))});
            } else if (isMap(it->collection)) {
                // Skip past empty/tombstone buckets to the next occupied one,
                // push its key, then advance the cursor past it.
                auto* map = asObjMap(as<Obj*>(it->collection));
                while (it->index < map->map.capacity() &&
                       map->map.entryAt(it->index)->state !=
                           MapSlot::OCCUPIED) {
                    ++it->index;
                }
                // ITER_HAS_NEXT was true, so an occupied slot must exist.
                push(map->map.entryAt(it->index)->key);
                ++it->index;
            } else {
                runtimeError(
                    "BUG: ObjIterator::collection has unexpected type.");
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        }
    }

#undef BINARY_OP
}

bool VM::callNative(ObjNative* native, int argCount) {
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

bool VM::bindMethod(ObjClass* klass, ObjString* name) {
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

void VM::defineNatives() {
    StdlibRegistrar reg(m_mm, m_globals);
    registerGlobals(reg);
    m_fileClass = registerFileAPI(reg);
    m_mapClass = registerMapAPI(reg);
    registerMath(reg);
}

void VM::runtimeError(const char* format, ...) {
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

void VM::markRoots() {
    for (Value* slot = stack; slot < stackTop; ++slot) {
        m_mm.markValue(*slot);
    }
    for (int i = 0; i < m_frameCount; ++i) {
        m_mm.markObject(m_frames[i].closure);
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
}

void VM::resetStack() {
    stackTop = stack;
    m_frameCount = 0;
}

void VM::push(Value value) { *stackTop++ = value; }

Value VM::pop() { return *--stackTop; }

Value VM::peek(int distance) { return stackTop[-1 - distance]; }
