#pragma once

#include "../exec_objects.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

// Ignores SIGPIPE for its lifetime. A peer that stopped reading makes write(2)
// fail with EPIPE and, with the default disposition, raises SIGPIPE and kills
// the VM. SIGPIPE is ignored only for one call, so the process-wide
// disposition and every spawned child keep the default.
class SigpipeIgnored {
  public:
    SigpipeIgnored() {
        // memset, not `= {}`: clang-format 18 and 22 disagree on the spacing
        // of an empty braced init for a multi-word struct type.
        struct sigaction ign;
        std::memset(&ign, 0, sizeof(ign));
        std::memset(&m_old, 0, sizeof(m_old));
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGPIPE, &ign, &m_old);
    }
    ~SigpipeIgnored() { sigaction(SIGPIPE, &m_old, nullptr); }
    SigpipeIgnored(const SigpipeIgnored&) = delete;
    SigpipeIgnored& operator=(const SigpipeIgnored&) = delete;

  private:
    struct sigaction m_old;
};

// Writes text (and an optional trailing newline) to a stdio stream, then
// flushes. Returns false and sets `message` when the write fails.
inline bool writeStream(FILE* handle, const char* method, std::string_view text,
                        bool newline, std::string& message) {
    bool ok;
    int err;
    {
        SigpipeIgnored guard;
        errno = 0;
        size_t written = std::fwrite(text.data(), 1, text.size(), handle);
        if (newline) {
            std::fputc('\n', handle);
        }
        ok = written == text.size();
        err = errno;
        if (std::fflush(handle) != 0) {
            ok = false;
            if (err == 0) {
                err = errno;
            }
        }
    }

    if (!ok) {
        if (err == 0) {
            err = EPIPE;
        }
        message = std::string(method) + "(): " + std::strerror(err);
        // A failed write leaves the stream error flag set. A program that
        // catches the fault can read the peer's last bytes next, and a read
        // that sees the old flag would raise a fault the network did not.
        std::clearerr(handle);
        return false;
    }
    return true;
}

// Writes every byte of `text` to a socket, retrying on EINTR and on a short
// write, then writes the newline if asked. MSG_NOSIGNAL makes a closed peer
// fail with EPIPE without a SIGPIPE, so the process-wide disposition is not
// touched and two threads cannot race on it. A descriptor has no error flag,
// so a failed write leaves nothing behind for the next read to see. Returns
// false and sets `message` when a write fails.
inline bool writeFd(int fd, const char* method, std::string_view text,
                    bool newline, std::string& message) {
    auto writeAll = [fd](const char* data, size_t size) -> int {
        while (size > 0) {
            ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // The descriptor is nonblocking and the connection is
                    // full. A write blocks its worker, so wait in the kernel.
                    struct pollfd pfd = {fd, POLLOUT, 0};
                    while (::poll(&pfd, 1, -1) < 0) {
                        if (errno != EINTR) {
                            return errno;
                        }
                    }
                    continue;
                }
                return errno;
            }
            data += n;
            size -= static_cast<size_t>(n);
        }
        return 0;
    };
    int err = writeAll(text.data(), text.size());
    if (err == 0 && newline) {
        err = writeAll("\n", 1);
    }
    if (err != 0) {
        message = std::string(method) + "(): " + std::strerror(err);
        return false;
    }
    return true;
}

// writeStream, with a failed write reported as a fatal stdlib error, matching
// the JVM backend's IOException.
inline bool writeStreamOrError(FILE* handle, const char* method,
                               std::string_view text, bool newline) {
    std::string message;
    if (!writeStream(handle, method, text, newline, message)) {
        nativeRuntimeError(message.c_str());
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
