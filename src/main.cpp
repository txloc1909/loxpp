#include "chunk.h"
#include "vm.h"

#include <iostream>

int main() {
    Chunk chunk;

    int constant = chunk.addConstant(1.2);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    constant = chunk.addConstant(3.4);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    chunk.write(Op::ADD, 123);

    constant = chunk.addConstant(5.6);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    chunk.write(Op::MULTIPLY, 123);

    constant = chunk.addConstant(2.0);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    chunk.write(Op::DIVIDE, 123);

    constant = chunk.addConstant(1.0);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    chunk.write(Op::SUBTRACT, 123);

    chunk.write(Op::RETURN, 123);
    chunk.disassemble("test chunk");

    VM vm;
    vm.interpret(&chunk);

    return 0;
}
