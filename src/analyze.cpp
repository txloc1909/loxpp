#include "analyze.h"

#include "compiler.h"
#include "memory_manager.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <streambuf>

LineIndex::LineIndex(const std::string& source) {
    m_lineStarts.push_back(0);
    for (std::size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\n') {
            m_lineStarts.push_back(i + 1);
        }
    }
}

std::pair<std::size_t, std::size_t>
LineIndex::locate(std::size_t offset) const {
    // The last line start that is not after offset.
    auto it =
        std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), offset);
    auto lineIndex = static_cast<std::size_t>(it - m_lineStarts.begin()) - 1;
    std::size_t column = offset - m_lineStarts[lineIndex] + 1;
    return {lineIndex + 1, column};
}

std::vector<Diagnostic> analyze(const std::string& source) {
    DiagnosticSink sink;
    MemoryManager mm;
    {
        // A debug build (LOXPP_DEBUG_PRINT_CODE) disassembles every chunk to
        // std::cout from inside compile(). The --check output must be the
        // diagnostics and nothing else, so mute std::cout for the compile.
        std::ostringstream muted;
        std::streambuf* saved = std::cout.rdbuf(muted.rdbuf());
        compile(source, &mm, &sink);
        std::cout.rdbuf(saved);
    }

    LineIndex index(source);
    std::vector<Diagnostic> result = std::move(sink.diagnostics());
    for (Diagnostic& diagnostic : result) {
        auto [line, column] = index.locate(diagnostic.offset);
        diagnostic.line = line;
        diagnostic.column = column;
    }
    return result;
}
