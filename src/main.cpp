#include "chunk.h"

#include <iostream>

int main() {
    Chunk chunk;

    int constant = chunk.addConstant(1.2);
    chunk.write(Op::CONSTANT);
    chunk.write(constant);

    chunk.write(Op::RETURN);

    chunk.disassemble("test chunk");

    return 0;
}
