#pragma once

#include "chunk.h"

#include <iosfwd>

class Allocator;

// Disassembles an entire chunk, printing a header followed by each instruction.
void disassembleChunk(const Chunk& chunk, const Allocator& alloc,
                      const char* name, std::ostream& out);

// Disassembles a single instruction at `offset`.
// Returns the offset of the next instruction.
int disassembleInstruction(const Chunk& chunk, const Allocator& alloc,
                           int offset, std::ostream& out);
