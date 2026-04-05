#include "vm.h"
#include "debug.h"
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
    auto chunk = compile(source, m_allocator.get());
    if (chunk == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    m_chunk = chunk.get();
    m_ip = m_chunk->cbegin();
    return run();
}

Byte VM::readByte() { return *m_ip++; }

Value VM::readConstant() { return m_chunk->getConstant(readByte()); }

Value VM::lastResult() const { return m_lastResult; }

std::optional<Value> VM::getGlobal(const std::string& name) const {
    ObjString* key = m_allocator->findString(name);
    if (!key)
        return std::nullopt;
    Value out;
    if (!m_globals.get(key, out))
        return std::nullopt;
    return out;
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

    for (;;) {
#ifdef LOXPP_DEBUG_TRACE_EXECUTION
        int currentOffset = static_cast<int>(m_ip - m_chunk->cbegin());
        bool color = isatty(STDOUT_FILENO) != 0;
        std::printf("[line %d] ", m_chunk->getLine(currentOffset));
        std::printf("          ");
        for (Value* slot = stack; slot < stackTop; slot++) {
            std::printf("[ ");
            printValue(*slot);
            std::printf(" ]");
        }
        std::printf("\n");
        disassembleInstruction(*m_chunk, *m_allocator, currentOffset, std::cout,
                               color);
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
                auto* b_str = asObjString(pop(), *m_allocator);
                auto* a_str = asObjString(pop(), *m_allocator);
                std::string result;
                result.reserve(a_str->chars.size() + b_str->chars.size());
                result.append(a_str->chars);
                result.append(b_str->chars);
                ObjHandle handle = m_allocator->makeString(std::move(result));
                push(Value{handle});
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
            printValue(pop(), *m_allocator);
            std::printf("\n");
            break;
        }
        case Op::POP: {
            m_lastResult = pop();
            break;
        }
        case Op::GET_LOCAL: {
            uint8_t slot = readByte();
            push(stack[slot]);
            break;
        }
        case Op::SET_LOCAL: {
            uint8_t slot = readByte();
            stack[slot] =
                peek(0); // assignment is an expression; leave value on stack
            break;
        }
        case Op::DEFINE_GLOBAL: {
            ObjString* name = asObjString(readConstant(), *m_allocator);
            m_globals.set(name, peek(0));
            pop();
            break;
        }
        case Op::GET_GLOBAL: {
            ObjString* name = asObjString(readConstant(), *m_allocator);
            Value value;
            if (!m_globals.get(name, value)) {
                runtimeError("Undefined variable '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            push(value);
            break;
        }
        case Op::SET_GLOBAL: {
            ObjString* name = asObjString(readConstant(), *m_allocator);
            // set() returns true if the key is *new*; an existing key is valid.
            // An entirely new key means the variable was never declared.
            if (m_globals.set(name, peek(0))) {
                m_globals.del(name); // undo the spurious insertion
                runtimeError("Undefined variable '%s'.", name->chars.c_str());
                return InterpretResult::RUNTIME_ERROR;
            }
            break;
        }
        case Op::RETURN: {
            return InterpretResult::OK;
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

    auto instruction = m_ip - m_chunk->cbegin() - 1;
    int line = m_chunk->getLine(instruction);
    std::fprintf(stderr, "[line %d] in script\n", line);
    resetStack();
}

void VM::resetStack() { stackTop = stack; }

void VM::push(Value value) { *stackTop++ = value; }

Value VM::pop() { return *--stackTop; }

Value VM::peek(int distance) { return stackTop[-1 - distance]; }
