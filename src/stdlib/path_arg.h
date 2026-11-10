#pragma once
#include "../exec_objects.h"

#include <string>

// Reports a runtime error and returns true when `path` holds a NUL byte.
//
// The host's file-system interface reads a path only up to its first NUL byte,
// so such a path would name a shorter, different file or directory -- one the
// program cannot name back. Every native that hands a path to the host checks
// the String first, so no call acts on a path the program did not name
// (spec/05-stdlib.md, Paths).
inline bool pathHoldsNul(const std::string& path, const char* function) {
    if (path.find('\0') == std::string::npos) {
        return false;
    }
    std::string msg = function;
    msg += "() path must not contain a NUL byte.";
    nativeRuntimeError(msg.c_str());
    return true;
}
