#include "vm.h"
#include "scanner.h"
#include "compiler.h"

#include <cstdio>
#include <cstdarg>
#include <functional>
#include <string>
#include <iostream>

#define DEBUG_TRACE_EXECUTION

InterpretResult VM::interpret(const std::string& source) {
    auto chunk = compile(source);
    if (chunk == nullptr) {
        return InterpretResult::COMPILE_ERROR;
    }

    m_chunk = chunk.get();
    m_ip = m_chunk->cbegin();
    return run();
}

Byte VM::readByte() { return *m_ip++; }

Value VM::readConstant() { return m_chunk->getConstant(readByte()); }

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
#ifdef DEBUG_TRACE_EXECUTION
        std::printf("          ");
        for (Value* slot = stack; slot < stackTop; slot++) {
            std::printf("[ ");
            printValue(*slot);
            std::printf(" ]");
        }
        std::printf("\n");
        m_chunk->disassembleInstruction(m_ip - m_chunk->cbegin());
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
            auto b = pop();
            auto a = pop();
            push(a == b);
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
            push(from<Number>(as<Number>(pop())));
            break;
        }
        case Op::ADD: {
            BINARY_OP(Number, +);
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
            push(as<bool>(!pop()));
        }
        case Op::RETURN: {
            printValue(pop());
            std::printf("\n");
            return InterpretResult::OK;
        }
        }
    }

#undef BINARY_OP
}

void VM::runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
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
