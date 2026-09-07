#include "analyze.h"

#include "compiler.h"
#include "memory_manager.h"

#include <algorithm>

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
    // A non-null sink makes compile() a diagnostics-only pass: endCompiler()
    // skips the debug chunk disassembly, so no std::cout write happens here
    // and analyze() holds no global mutable state.
    compile(source, &mm, &sink);

    LineIndex index(source);
    std::vector<Diagnostic> result = std::move(sink.diagnostics());
    for (Diagnostic& diagnostic : result) {
        auto [line, column] = index.locate(diagnostic.offset);
        diagnostic.line = line;
        diagnostic.column = column;
    }
    return result;
}
