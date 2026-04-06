#pragma once

#include "chunk.h"

#include <iosfwd>

class MemoryManager;

// Disassembles an entire chunk, printing a header followed by each instruction.
// Pass color=true to emit ANSI escape codes; only do so when output is a TTY.
void disassembleChunk(const Chunk& chunk, const MemoryManager& mm,
                      const char* name, std::ostream& out, bool color = false);

// Disassembles a single instruction at `offset`.
// Returns the offset of the next instruction.
// Pass color=true to emit ANSI escape codes; only do so when output is a TTY.
int disassembleInstruction(const Chunk& chunk, const MemoryManager& mm,
                           int offset, std::ostream& out, bool color = false);
