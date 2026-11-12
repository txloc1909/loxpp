#pragma once
#include "../exec_objects.h"

#include <string>

// Reports a runtime error and returns true when `value` holds a NUL byte.
//
// The host reads a String that names something outside the program — a path, an
// environment variable, a program, or a host — only up to its first NUL byte,
// so such a String would name a shorter, different thing: one the program
// cannot name back. Every native that hands such a String to the host checks it
// first, so no call acts on a name the program did not write
// (spec/05-stdlib.md, Strings the host reads).
inline bool stringHoldsNul(const std::string& value, const char* function) {
    if (value.find('\0') == std::string::npos) {
        return false;
    }
    std::string msg = function;
    msg += "() argument must not contain a NUL byte.";
    nativeRuntimeError(msg.c_str());
    return true;
}
