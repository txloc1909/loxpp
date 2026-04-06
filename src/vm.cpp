#include "vm.h"
#include "debug.h"
#include "function.h"
#include "memory_manager.h"
#include "object.h"
#include "scanner.h"
#include "compiler.h"

#include <cstdio>
#include <cstdarg>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

InterpretResult VM::interpret(const std::string& source) {
    ObjFunction* fn = compile(source, &m_mm);
    if (fn == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    push(Value{static_cast<Obj*>(fn)});
    call(fn, 0);
    return run();
}

Byte VM::readByte() { return *m_frames[m_frameCount - 1].ip++; }

uint16_t VM::readShort() {
    uint16_t hi = readByte();
    uint16_t lo = readByte();
    return static_cast<uint16_t>((hi << 8) | lo);
}

Value VM::readConstant() {
    return m_frames[m_frameCount - 1].function->chunk.getConstant(readByte());
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

bool VM::call(ObjFunction* fn, int argCount) {
    if (argCount != fn->arity) {
        runtimeError("Expected %d arguments but got %d.", fn->arity, argCount);
        return false;
    }
    if (m_frameCount == FRAMES_MAX) {
        runtimeError("Stack overflow.");
        return false;
    }
    CallFrame* frame = &m_frames[m_frameCount++];
    frame->function = fn;
    frame->ip = fn->chunk.cbegin();
    frame->slots = stackTop - argCount - 1;
    return true;
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
            const Chunk& chunk = frame->function->chunk;
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
            if (!isFunction(callee)) {
                runtimeError("Can only call functions and classes.");
                return InterpretResult::RUNTIME_ERROR;
            }
            if (!call(asObjFunction(callee), argCount)) {
                return InterpretResult::RUNTIME_ERROR;
            }
            frame = &m_frames[m_frameCount - 1];
            break;
        }
        case Op::RETURN: {
            Value result = pop();
            m_frameCount--;
            if (m_frameCount == 0) {
                // Finished executing the top-level script.
                pop(); // remove the script ObjFunction from the stack
                return InterpretResult::OK;
            }
            // Discard the callee's stack window and push return value.
            stackTop = frame->slots;
            push(result);
            frame = &m_frames[m_frameCount - 1];
            break;
        }
        }
    }

#undef BINARY_OP
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
        const CallFrame& f = m_frames[i];
        const Chunk& chunk = f.function->chunk;
        auto offset = static_cast<int>(f.ip - chunk.cbegin()) - 1;
        int line = chunk.getLine(offset);
        std::fprintf(stderr, "[line %d] in ", line);
        if (f.function->name == nullptr) {
            std::fprintf(stderr, "script\n");
        } else {
            std::fprintf(stderr, "%s()\n", f.function->name->chars.c_str());
        }
    }

    resetStack();
}

void VM::resetStack() {
    stackTop = stack;
    m_frameCount = 0;
}

void VM::push(Value value) { *stackTop++ = value; }

Value VM::pop() { return *--stackTop; }

Value VM::peek(int distance) { return stackTop[-1 - distance]; }
