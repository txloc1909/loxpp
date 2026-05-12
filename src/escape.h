#pragma once
#include <array>
#include <optional>

struct EscapeEntry {
    char code;
    char value;
};

constexpr std::array<EscapeEntry, 6> kEscapeSequences{{
    {'"', '"'},
    {'\\', '\\'},
    {'n', '\n'},
    {'t', '\t'},
    {'r', '\r'},
    {'0', '\0'},
}};

// Returns the decoded value for `code`, or nullopt if not a valid escape char.
constexpr std::optional<char> decodeEscape(char code) {
    for (auto& e : kEscapeSequences) {
        if (e.code == code) {
            return e.value;
}
}
    return std::nullopt;
}
