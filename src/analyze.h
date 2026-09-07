#pragma once

#include "diagnostic.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Maps a byte offset in a source buffer to a 1-based (line, column) pair.
// Built once from the source; a lookup is a binary search over the line starts.
class LineIndex {
  public:
    explicit LineIndex(const std::string& source);

    // Returns {line, column}, both 1-based. An offset at or past the end of the
    // source maps to the last line and the column just after its last
    // character.
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    locate(std::size_t offset) const;

  private:
    std::vector<std::size_t> m_lineStarts;
};

// Compiles source for diagnostics only: no program runs. Returns every
// error the compiler front-end reports, each with a 1-based line and column
// derived from its source offset. Errors only for now; the resolver adds
// warnings in a later node.
std::vector<Diagnostic> analyze(const std::string& source);
