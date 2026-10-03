#pragma once

#include "../exec_objects.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// Writes text (and an optional trailing newline) to a stdio stream, then
// flushes. A peer that stopped reading makes write(2) fail with EPIPE and,
// with the default disposition, raises SIGPIPE and kills the VM. SIGPIPE is
// ignored only for this call, so the process-wide disposition and every
// spawned child keep the default. A failed write is reported as a fatal
// stdlib error, matching the JVM backend's IOException.
inline bool writeStreamOrError(FILE* handle, const char* method,
                               std::string_view text, bool newline) {
    // memset, not `= {}`: clang-format 18 and 22 disagree on the spacing of an
    // empty braced init for a multi-word struct type.
    struct sigaction ign;
    std::memset(&ign, 0, sizeof(ign));
    struct sigaction old;
    std::memset(&old, 0, sizeof(old));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, &old);

    errno = 0;
    size_t written = std::fwrite(text.data(), 1, text.size(), handle);
    if (newline) {
        std::fputc('\n', handle);
    }
    bool ok = written == text.size();
    int err = errno;
    if (std::fflush(handle) != 0) {
        ok = false;
        if (err == 0) {
            err = errno;
        }
    }
    sigaction(SIGPIPE, &old, nullptr);

    if (!ok) {
        if (err == 0) {
            err = EPIPE;
        }
        std::string msg = std::string(method) + "(): " + std::strerror(err);
        nativeRuntimeError(msg.c_str());
        return false;
    }
    return true;
}

// Reads up to `n` bytes from a stdio stream. Blocks until `n` bytes have been
// read or the stream reaches end of file, then returns everything read. At end
// of file with no byte remaining, returns "". The caller validates the stream
// (still open, not reaped) before calling.
inline std::string readStreamBytes(FILE* handle, size_t n) {
    std::string out;
    char chunk[4096];
    while (out.size() < n) {
        size_t want = n - out.size();
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        size_t got = std::fread(chunk, 1, want, handle);
        if (got == 0) {
            break;
        }
        out.append(chunk, got);
    }
    return out;
}
