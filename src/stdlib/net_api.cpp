#include "net_api.h"
#include "stdlib_context.h"
#include "stream_io.h"
#include "../container_objects.h"
#include "../runtime.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Module-local class pointers, used to stamp new instances and register the
// method tables Runtime::opGetProperty/opInvoke dispatch to.
// Per thread: each worker registers the classes of its own heap.
static thread_local ObjClass* s_socketClass = nullptr;
static thread_local ObjClass* s_serverClass = nullptr;

// A fault of the network or of a closed socket is catchable: it is delivered
// as an Error of the given kind (spec/05-stdlib.md, Socket and Server). An
// argument fault keeps nativeRuntimeError.
static const char* const kNetworkError = "NetworkError";
static const char* const kSocketClosedError = "SocketClosedError";

static void raiseNet(const char* kind, const std::string& msg) {
    getActiveRuntime()->raiseFromNative(kind, msg.c_str());
}

static bool asStringArg(const Value& v, std::string& out) {
    if (!isString(v)) {
        nativeRuntimeError("Expected a string argument.");
        return false;
    }
    auto* s = asObjString(as<Obj*>(v));
    out.assign(s->chars.data(), s->chars.size());
    return true;
}

// Validate a port argument. `minPort` is 1 for connect (0 is never a valid
// remote port) and 0 for listen (0 asks the OS to choose).
static bool asPort(const Value& v, long minPort, int& out) {
    if (!is<Number>(v)) {
        nativeRuntimeError("Port must be a number.");
        return false;
    }
    double raw = as<Number>(v);
    if (!std::isfinite(raw) || raw != std::floor(raw) || raw < minPort ||
        raw > 65535) {
        nativeRuntimeError("Port must be an integer in range.");
        return false;
    }
    out = static_cast<int>(raw);
    return true;
}

// Validate a byte-count argument. `n` must be a non-negative integer Number.
static bool asByteCount(const Value& v, const char* method, size_t& out) {
    if (!is<Number>(v)) {
        std::string msg =
            std::string(method) + "() byte count must be a number.";
        nativeRuntimeError(msg.c_str());
        return false;
    }
    double raw = as<Number>(v);
    if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0 ||
        raw > static_cast<double>(INT_MAX)) {
        std::string msg =
            std::string(method) +
            "() byte count must be an integer in range 0 to 2147483647.";
        nativeRuntimeError(msg.c_str());
        return false;
    }
    out = static_cast<size_t>(raw);
    return true;
}

// Build the "host:port" text the connect error messages name.
static std::string hostPort(const std::string& host, int port) {
    return host + ":" + std::to_string(port);
}

static Value connectNative(int /*argc*/, Value* argv) {
    std::string host;
    if (!asStringArg(argv[0], host)) {
        return from<Nil>(Nil{});
    }
    int port = 0;
    if (!asPort(argv[1], 1, port)) {
        return from<Nil>(Nil{});
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0) {
        std::string msg =
            "connect(): cannot resolve '" + host + "': " + gai_strerror(gai);
        raiseNet(kNetworkError, msg);
        return from<Nil>(Nil{});
    }

    int fd = -1;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd =
            socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (fd < 0) {
            lastErrno = errno;
            continue;
        }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        lastErrno = errno;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        std::string msg = "connect(): cannot connect to '" +
                          hostPort(host, port) +
                          "': " + std::strerror(lastErrno);
        raiseNet(kNetworkError, msg);
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjSocket* sock = mm->create<ObjSocket>(s_socketClass);
    mm->pushTempRoot(sock);
    sock->fd = fd;
    mm->popTempRoot();
    return Value{static_cast<Obj*>(sock)};
}

static Value listenNative(int /*argc*/, Value* argv) {
    std::string host;
    if (!asStringArg(argv[0], host)) {
        return from<Nil>(Nil{});
    }
    int port = 0;
    if (!asPort(argv[1], 0, port)) {
        return from<Nil>(Nil{});
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0) {
        std::string msg =
            "listen(): cannot resolve '" + host + "': " + gai_strerror(gai);
        raiseNet(kNetworkError, msg);
        return from<Nil>(Nil{});
    }

    int fd = -1;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd =
            socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);
        if (fd < 0) {
            lastErrno = errno;
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 &&
            listen(fd, SOMAXCONN) == 0) {
            break;
        }
        lastErrno = errno;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        std::string msg = "listen(): cannot listen on '" +
                          hostPort(host, port) +
                          "': " + std::strerror(lastErrno);
        raiseNet(kNetworkError, msg);
        return from<Nil>(Nil{});
    }

    // Report the port the OS chose when port 0 was requested.
    struct sockaddr_storage addr;
    std::memset(&addr, 0, sizeof(addr));
    socklen_t addrLen = sizeof(addr);
    int boundPort = port;
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) ==
        0) {
        if (addr.ss_family == AF_INET) {
            boundPort =
                ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            boundPort =
                ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
        }
    }

    MemoryManager* mm = getActiveMM();
    ObjServer* server = mm->create<ObjServer>(s_serverClass);
    mm->pushTempRoot(server);
    server->fd = fd;
    server->boundPort = boundPort;
    mm->popTempRoot();
    return Value{static_cast<Obj*>(server)};
}

// The socket method natives read their receiver at args[-1], like the File
// methods do. A read on a closed socket and a write after close_write() raise
// SocketClosedError, matching spec/05-stdlib.md's Socket section.
static ObjSocket* checkSocketRead(Value* args, const char* method) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->fd < 0) {
        std::string msg = "Cannot call '";
        msg += method;
        msg += "' on a closed socket.";
        raiseNet(kSocketClosedError, msg);
        return nullptr;
    }
    return s;
}

static ObjSocket* checkSocketWrite(Value* args, const char* method) {
    ObjSocket* s = checkSocketRead(args, method);
    if (s == nullptr) {
        return nullptr;
    }
    if (s->writeClosed) {
        raiseNet(kSocketClosedError,
                 "Cannot write to a socket after close_write().");
        return nullptr;
    }
    return s;
}

// Size of one read from the descriptor.
static constexpr size_t kReadChunk = 4096;

enum class Fill : unsigned char { Data, Eof, Failed };

// read(2) into `dst`, with EINTR retried. A failure, other than EINTR, is a
// NetworkError: it is not an end of stream.
static ssize_t readRetry(int fd, char* dst, size_t size, const char* method) {
    for (;;) {
        ssize_t n = ::read(fd, dst, size);
        if (n >= 0) {
            return n;
        }
        if (errno != EINTR) {
            raiseNet(kNetworkError,
                     std::string(method) + "(): " + std::strerror(errno));
            return -1;
        }
    }
}

// Read up to `want` bytes and append them to `buf`. Returns the count, 0 at
// the end of the stream, or -1 after raising a fault. `buf` is unchanged
// unless bytes arrive.
static ssize_t appendRead(int fd, std::string& buf, size_t want,
                          const char* method) {
    size_t old = buf.size();
    buf.resize(old + want);
    ssize_t n = readRetry(fd, &buf[old], want, method);
    buf.resize(old + (n > 0 ? static_cast<size_t>(n) : 0));
    return n;
}

// The input buffer holds exactly the bytes read from the descriptor and not
// yet returned. A descriptor has no stdio buffer, so readline() keeps what
// follows its line here, and read(), read_bytes(), readlines() and readline()
// consume it first. While readline() builds a line the buffer holds that line
// plus at most one chunk past its end. After any read returns, the buffer
// holds only the unread tail in storage of its own (takeInput moves the old
// storage out with the result), so a socket does not keep the memory of its
// longest line.

// Move the first `n` bytes of the input buffer out as a result, and keep the
// tail. The prefix is moved, not copied, so a long line exists once.
static std::string takeInput(ObjSocket* s, size_t n) {
    std::string tail(s->inbuf, n);
    std::string out = std::move(s->inbuf);
    out.resize(n);
    s->inbuf = std::move(tail);
    return out;
}

// Append one read chunk to the input buffer.
static Fill fillInput(ObjSocket* s, const char* method) {
    ssize_t n = appendRead(s->fd, s->inbuf, kReadChunk, method);
    if (n < 0) {
        return Fill::Failed;
    }
    return n == 0 ? Fill::Eof : Fill::Data;
}

// Read to the end of the stream into the input buffer. Returns false after
// raising a fault; the bytes that arrived stay in the buffer.
static bool fillToEof(ObjSocket* s, const char* method) {
    for (;;) {
        switch (fillInput(s, method)) {
        case Fill::Data:
            break;
        case Fill::Eof:
            return true;
        case Fill::Failed:
            return false;
        }
    }
}

static Value socketReadNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "read");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!fillToEof(s, "read")) {
        return from<Nil>(Nil{});
    }
    std::string buf = takeInput(s, s->inbuf.size());
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value socketReadBytesNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "read_bytes");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    size_t n = 0;
    if (!asByteCount(args[0], "read_bytes", n)) {
        return from<Nil>(Nil{});
    }
    size_t take = s->inbuf.size() < n ? s->inbuf.size() : n;
    std::string out = takeInput(s, take);
    // Read the rest straight into the result: the want is bounded by the
    // count, so no byte past it is read and none needs buffering.
    while (out.size() < n) {
        size_t want = n - out.size();
        if (want > kReadChunk) {
            want = kReadChunk;
        }
        ssize_t got = appendRead(s->fd, out, want, "read_bytes");
        if (got < 0) {
            // Keep what arrived, so a later read does not lose it.
            out.append(s->inbuf);
            s->inbuf = std::move(out);
            return from<Nil>(Nil{});
        }
        if (got == 0) {
            break;
        }
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(out)))};
}

static Value socketReadlineNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readline");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    size_t scanned = 0;
    for (;;) {
        size_t nl = s->inbuf.find('\n', scanned);
        if (nl != std::string::npos) {
            std::string line = takeInput(s, nl);
            s->inbuf.erase(0, 1); // the newline
            return Value{
                static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
        }
        scanned = s->inbuf.size();
        Fill f = fillInput(s, "readline");
        if (f == Fill::Failed) {
            return from<Nil>(Nil{});
        }
        if (f == Fill::Eof) {
            if (s->inbuf.empty()) {
                return from<Nil>(Nil{});
            }
            std::string line = takeInput(s, s->inbuf.size());
            return Value{
                static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
        }
    }
}

static Value socketReadlinesNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readlines");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!fillToEof(s, "readlines")) {
        return from<Nil>(Nil{});
    }
    std::string data = takeInput(s, s->inbuf.size());
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        size_t end = nl == std::string::npos ? data.size() : nl;
        ObjString* str = mm->makeString(data.substr(start, end - start));
        mm->pushTempRoot(str);
        list->elements.emplace_back(static_cast<Obj*>(str));
        mm->popTempRoot();
        start = end + 1;
    }
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value socketWriteNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketWrite(args, "write");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'write' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* str = asObjString(as<Obj*>(args[0]));
    std::string message;
    if (!writeFd(s->fd, "write",
                 std::string_view(str->chars.data(), str->chars.size()), false,
                 message)) {
        raiseNet(kNetworkError, message);
    }
    return from<Nil>(Nil{});
}

static Value socketWritelineNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketWrite(args, "writeline");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'writeline' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* str = asObjString(as<Obj*>(args[0]));
    std::string message;
    if (!writeFd(s->fd, "writeline",
                 std::string_view(str->chars.data(), str->chars.size()), true,
                 message)) {
        raiseNet(kNetworkError, message);
    }
    return from<Nil>(Nil{});
}

static Value socketCloseWriteNative(int /*argc*/, Value* args) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->fd >= 0 && !s->writeClosed) {
        shutdown(s->fd, SHUT_WR);
        s->writeClosed = true;
    }
    return from<Nil>(Nil{});
}

static Value socketCloseNative(int /*argc*/, Value* args) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->fd >= 0) {
        ::close(s->fd);
        s->fd = -1;
    }
    return from<Nil>(Nil{});
}

static Value serverAcceptNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    if (s->fd < 0) {
        raiseNet(kSocketClosedError,
                 "Cannot call 'accept' on a closed server.");
        return from<Nil>(Nil{});
    }
    int cfd;
    do {
        cfd = accept4(s->fd, nullptr, nullptr, SOCK_CLOEXEC);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) {
        std::string msg = std::string("accept(): ") + std::strerror(errno);
        raiseNet(kNetworkError, msg);
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjSocket* sock = mm->create<ObjSocket>(s_socketClass);
    mm->pushTempRoot(sock);
    sock->fd = cfd;
    mm->popTempRoot();
    return Value{static_cast<Obj*>(sock)};
}

static Value serverPortNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    return from<Number>(static_cast<Number>(s->boundPort));
}

static Value serverCloseNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    if (s->fd >= 0) {
        ::close(s->fd);
        s->fd = -1;
    }
    return from<Nil>(Nil{});
}

NetClasses registerNetAPI(StdlibRegistrar& reg) {
    // Both classes stay rooted until this function returns: creating the
    // Server class allocates, and GC would otherwise collect Socket after its
    // own root is gone. Runtime stores both pointers only after the return.
    ObjClass* socket = reg.makeClass("Socket");
    reg.mm().pushTempRoot(socket);
    reg.addMethod(socket, "read", socketReadNative, 0);
    reg.addMethod(socket, "read_bytes", socketReadBytesNative, 1);
    reg.addMethod(socket, "readline", socketReadlineNative, 0);
    reg.addMethod(socket, "readlines", socketReadlinesNative, 0);
    reg.addMethod(socket, "write", socketWriteNative, 1);
    reg.addMethod(socket, "writeline", socketWritelineNative, 1);
    reg.addMethod(socket, "close_write", socketCloseWriteNative, 0);
    reg.addMethod(socket, "close", socketCloseNative, 0);

    ObjClass* server = reg.makeClass("Server");
    reg.mm().pushTempRoot(server);
    reg.addMethod(server, "accept", serverAcceptNative, 0);
    reg.addMethod(server, "port", serverPortNative, 0);
    reg.addMethod(server, "close", serverCloseNative, 0);

    reg.defineGlobal("connect", connectNative, 2);
    reg.defineGlobal("listen", listenNative, 2);

    s_socketClass = socket;
    s_serverClass = server;
    reg.mm().popTempRoot(); // server
    reg.mm().popTempRoot(); // socket
    return NetClasses{socket, server};
}
