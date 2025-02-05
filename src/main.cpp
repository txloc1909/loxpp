#include "chunk.h"

#include <iostream>

int main() {
    Chunk chunk;

    int constant = chunk.addConstant(1.2);
    chunk.write(Op::CONSTANT, 123);
    chunk.write(constant, 123);

    chunk.write(Op::RETURN, 123);

    chunk.disassemble("test chunk");

    return 0;
}
