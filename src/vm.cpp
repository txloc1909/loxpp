#include "vm.h"
#include "debug.h"
#include "function.h"
#include "memory_manager.h"
#include "native.h"
#include "object.h"
#include "scanner.h"
#include "compiler.h"

#include "math.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Native function implementations
// ---------------------------------------------------------------------------

static Value clockNative(int /*argCount*/, Value* /*args*/) {
    return from<Number>(static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
}

// Read one line from stdin. Returns nil on EOF, otherwise an ObjString.
// The VM pointer is smuggled via a thread-local so native functions can
// allocate managed strings without changing the NativeFn signature.
static MemoryManager* s_currentMM = nullptr;

// Natives cannot call VM::runtimeError directly. They set this flag instead;
// callNative() checks it after the native returns and reports the error.
static bool s_nativeError = false;
static std::string s_nativeErrorMsg;

void nativeRuntimeError(const char* msg) {
    s_nativeError = true;
    s_nativeErrorMsg = msg;
}

static Value inputNative(int /*argCount*/, Value* /*args*/) {
    std::string line;
    if (!std::getline(std::cin, line))
        return from<Nil>(Nil{});
    ObjString* s = s_currentMM->makeString(line);
    return Value{static_cast<Obj*>(s)};
}

static Value strNative(int /*argCount*/, Value* args) {
    std::string s = stringify(args[0]);
    ObjString* obj = s_currentMM->makeString(s);
    return Value{static_cast<Obj*>(obj)};
}

static Value lenNative(int /*argCount*/, Value* args) {
    if (isList(args[0])) {
        auto* list = asObjList(as<Obj*>(args[0]));
        return from<Number>(static_cast<double>(list->elements.size()));
    }
    if (isString(args[0])) {
        auto* s = asObjString(as<Obj*>(args[0]));
        return from<Number>(static_cast<double>(s->chars.size()));
    }
    nativeRuntimeError("len() argument must be a list or string.");
    return from<Nil>(Nil{});
}

InterpretResult VM::interpret(const std::string& source) {
    ObjFunction* fn = compile(source, &m_mm);
    if (fn == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    // Root fn on the stack before any allocation (defineNatives,
    // create<ObjClosure>) can trigger GC. Without this, fn is unreachable
    // between compile() returning and push(closure) — the Compiler has already
    // been destroyed and m_currentCompiler is nullptr.
    push(Value{static_cast<Obj*>(fn)});
    defineNatives();
    s_currentMM = &m_mm;
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
        readByte());
}

Value VM::lastResult() const { return m_lastResult; }

std::optional<Value> VM::getGlobal(const std::string& name) const {
    ObjString* key = m_mm.findString(name);
    if (!key)
        return std::nullopt;
    Value out;
    if (!m_globals.get(key, out))
        return std::nullopt;
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
    if (cur != nullptr && cur->location == local)
        return cur;
    ObjUpvalue* uv = m_mm.create<ObjUpvalue>(local);
    uv->next = cur;
    if (prev == nullptr)
        m_openUpvalues = uv;
    else
        prev->next = uv;
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
            if (result != 0 && (result < 0) != (b < 0))
                result += b;
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
            if (isFalsy(peek(0)))
                frame->ip += offset;
            break;
        }
        case Op::LOOP: {
            frame->ip -= readShort();
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
            } else {
                runtimeError("Can only call functions and classes.");
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
            if (!bindMethod(instance->klass, name))
                return InterpretResult::RUNTIME_ERROR;
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
                        if (!call(asObjClosure(as<Obj*>(fieldVal)), argCount))
                            return InterpretResult::RUNTIME_ERROR;
                    } else if (isNative(fieldVal)) {
                        if (!callNative(asObjNative(as<Obj*>(fieldVal)),
                                        argCount))
                            return InterpretResult::RUNTIME_ERROR;
                    } else {
                        runtimeError("Can only call functions and classes.");
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
                if (!call(asObjClosure(as<Obj*>(method)), argCount))
                    return InterpretResult::RUNTIME_ERROR;
                frame = &m_frames[m_frameCount - 1];
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
                } else {
                    runtimeError("Undefined method '%s' on list.",
                                 name->chars.c_str());
                    return InterpretResult::RUNTIME_ERROR;
                }
            } else {
                runtimeError("Only instances have methods.");
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
            pop(); // pop subclass; superclass stays as "super" local
            break;
        }
        case Op::GET_SUPER: {
            ObjString* name = asObjString(readConstant());
            ObjClass* superclass = asObjClass(as<Obj*>(pop()));
            if (!bindMethod(superclass, name))
                return InterpretResult::RUNTIME_ERROR;
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
            if (!call(asObjClosure(as<Obj*>(method)), argCount))
                return InterpretResult::RUNTIME_ERROR;
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
            for (int i = count - 1; i >= 0; i--)
                list->elements[i] = pop();
            m_mm.popTempRoot();
            push(Value{static_cast<Obj*>(list)});
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
            } else {
                runtimeError("Only lists and strings can be indexed.");
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
            if (!isList(listVal)) {
                runtimeError("Only lists can be indexed for assignment.");
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
            } else {
                runtimeError("Right operand of 'in' must be a list or string.");
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
    s_nativeError = false;
    Value result = native->function(argCount, stackTop - argCount);
    if (s_nativeError) {
        runtimeError("%s", s_nativeErrorMsg.c_str());
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

void VM::defineNative(const char* name, NativeFn fn, int arity) {
    ObjNative* native = m_mm.create<ObjNative>(fn, arity);
    push(
        Value{static_cast<Obj*>(native)}); // root native across makeString's GC
    ObjString* key = m_mm.makeString(name);
    push(Value{static_cast<Obj*>(key)}); // root key across m_globals.set's GC
    m_globals.set(key, Value{static_cast<Obj*>(native)});
    pop(); // key
    pop(); // native
}

void VM::defineNatives() {
    defineNative("clock", clockNative, 0);
    defineNative("input", inputNative, 0);
    defineNative("str", strNative, 1);
    defineNative("len", lenNative, 1);
    defineMathObject();
}

void VM::defineMathObject() {
    // a. Create the Math class (just a nominal holder for the instance)
    ObjString* className = m_mm.makeString("Math");
    push(Value{static_cast<Obj*>(className)}); // root className
    ObjClass* klass =
        m_mm.create<ObjClass>(className, VmAllocator<Entry>{&m_mm});
    push(Value{static_cast<Obj*>(klass)}); // root klass

    // b. Create the instance
    ObjInstance* instance =
        m_mm.create<ObjInstance>(klass, VmAllocator<Entry>{&m_mm});
    push(Value{static_cast<Obj*>(instance)}); // root instance

    // c. Add function fields — each push/pop pair keeps objects rooted during
    //    any GC triggered by makeString or fields.set.
    for (std::size_t i = 0; i < kMathFunctionCount; ++i) {
        const auto& e = kMathFunctions[i];
        ObjNative* native = m_mm.create<ObjNative>(e.fn, e.arity);
        push(Value{static_cast<Obj*>(native)}); // root native
        ObjString* key = m_mm.makeString(e.name);
        push(Value{static_cast<Obj*>(key)}); // root key
        instance->fields.set(key, Value{static_cast<Obj*>(native)});
        pop(); // key
        pop(); // native
    }

    // d. Add constant fields
    for (std::size_t i = 0; i < kMathConstantCount; ++i) {
        const auto& c = kMathConstants[i];
        ObjString* key = m_mm.makeString(c.name);
        push(Value{static_cast<Obj*>(key)}); // root key
        instance->fields.set(key, from<Number>(c.value));
        pop(); // key
    }

    // e-f. Register instance as global "math"
    ObjString* globalKey = m_mm.makeString("math");
    push(Value{static_cast<Obj*>(globalKey)}); // root key
    m_globals.set(globalKey, Value{static_cast<Obj*>(instance)});
    pop(); // globalKey

    // g. Pop instance, klass, className
    pop(); // instance
    pop(); // klass
    pop(); // className
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
    for (Value* slot = stack; slot < stackTop; ++slot)
        m_mm.markValue(*slot);
    for (int i = 0; i < m_frameCount; ++i)
        m_mm.markObject(m_frames[i].closure);
    for (ObjUpvalue* uv = m_openUpvalues; uv != nullptr; uv = uv->next)
        m_mm.markObject(uv);
    m_globals.forEach([this](ObjString* key, Value val) {
        m_mm.markObject(key);
        m_mm.markValue(val);
    });
}

void VM::resetStack() {
    stackTop = stack;
    m_frameCount = 0;
}

void VM::push(Value value) { *stackTop++ = value; }

Value VM::pop() { return *--stackTop; }

Value VM::peek(int distance) { return stackTop[-1 - distance]; }
