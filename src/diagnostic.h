#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class Severity : std::uint8_t {
    Error,
    Warning,
    Info,
};

// One static problem in a source buffer. offset and length are the byte span
// (see Token in token.h); line is 1-based and comes straight from the token.
// column is 1-based and is filled by analyze() from offset, not from line,
// because the parser tracks only line. A hand-built token has no source span,
// so offset and length are 0 and line stays whatever the token carried.
struct Diagnostic {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t line = 0;
    std::size_t column = 0;
    Severity severity = Severity::Error;
    std::string message;
};

// A plain collector. Parser::errorAt pushes into it instead of printing when a
// sink is set; a null sink keeps the stderr behaviour.
class DiagnosticSink {
  public:
    void push(Diagnostic diagnostic) {
        m_diagnostics.push_back(std::move(diagnostic));
    }

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return m_diagnostics;
    }
    [[nodiscard]] std::vector<Diagnostic>& diagnostics() {
        return m_diagnostics;
    }
    [[nodiscard]] bool empty() const { return m_diagnostics.empty(); }

  private:
    std::vector<Diagnostic> m_diagnostics;
};
