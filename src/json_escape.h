#pragma once

#include <cstdio>
#include <string>

// Appends `value` to `out` as a quoted JSON string. Escapes ", \, the four
// short forms \n \r \t, and every other control byte (< 0x20) as \uXXXX.
// Diagnostic messages today use only printable ASCII with single quotes, so
// the quote, backslash, and control-byte branches guard callers that pass
// arbitrary text (a future resolver message, a token lexeme).
inline void appendJsonString(std::string& out, const std::string& value) {
    out += '"';
    for (char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}
