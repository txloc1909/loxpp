#include "chunk.h"

#include <iostream>

int main() {
    Chunk chunk;
    chunk.write(Op::RETURN);
    chunk.disassemble("test chunk");

    return 0;
}
