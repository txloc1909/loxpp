#include "vm.h"

#include <cstdio>

#define DEBUG_TRACE_EXECUTION

InterpretResult VM::interpret(Chunk const* chunk) {
    m_chunk = chunk;
    m_ip = chunk->cbegin();
    return run();
}

Byte VM::readByte() { return *m_ip++; }

Value VM::readConstant() { return m_chunk->getConstant(readByte()); }

InterpretResult VM::run() {
#ifdef DEBUG_TRACE_EXECUTION
    std::printf("          ");
    for (Value* slot = stack; slot < stackTop; slot++) {
        std::printf("[ %g ]", *slot);
    }
    std::printf("\n");
    m_chunk->disassembleInstruction(m_ip - m_chunk->cbegin());
#endif

    for (;;) {
        Byte instruction = readByte();
        switch (toOpcode(instruction)) {
        case Op::CONSTANT: {
            push(readConstant());
            break;
        }
        case Op::RETURN: {
            std::printf("%g\n", pop());
            return InterpretResult::OK;
        }
        }
    }
}

void VM::resetStack() { stackTop = stack; }

void VM::push(Value value) { *stackTop++ = value; }

Value VM::pop() { return *--stackTop; }
