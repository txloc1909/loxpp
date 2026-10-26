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

enum class Fill { Data, Eof, Failed };

// read(2) with EINTR retried. A failure, other than EINTR, is a NetworkError:
// it is not an end of stream.
static ssize_t readRetry(ObjSocket* s, char* dst, size_t size,
                         const char* method) {
    for (;;) {
        ssize_t n = ::read(s->fd, dst, size);
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

// Drop the bytes already returned, so the buffer holds only the unread tail.
static void compactInput(ObjSocket* s) {
    if (s->inPos > 0) {
        s->inbuf.erase(0, s->inPos);
        s->inPos = 0;
    }
}

// Append one read chunk to the input buffer. The buffer is the only place a
// read can hold bytes past the end of the line it returns, because a
// descriptor has no stdio buffer. So readline() keeps what follows the line
// here, and read(), read_bytes() and readlines() consume it first. The
// buffer holds the line being built plus at most one chunk past its end.
static Fill fillInput(ObjSocket* s, const char* method) {
    char chunk[kReadChunk];
    ssize_t n = readRetry(s, chunk, sizeof(chunk), method);
    if (n < 0) {
        return Fill::Failed;
    }
    if (n == 0) {
        return Fill::Eof;
    }
    s->inbuf.append(chunk, static_cast<size_t>(n));
    return Fill::Data;
}

// Read to the end of the stream into the input buffer. Returns false after
// raising a fault.
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
    compactInput(s);
    if (!fillToEof(s, "read")) {
        return from<Nil>(Nil{});
    }
    std::string buf = std::move(s->inbuf);
    s->inbuf.clear();
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
    std::string out;
    size_t buffered = s->inbuf.size() - s->inPos;
    size_t take = buffered < n ? buffered : n;
    out.assign(s->inbuf, s->inPos, take);
    s->inPos += take;
    // Read the rest straight into the result: the want is bounded by the
    // count, so no byte past it is read and none needs buffering.
    char chunk[kReadChunk];
    while (out.size() < n) {
        size_t want = n - out.size();
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        ssize_t got = readRetry(s, chunk, want, "read_bytes");
        if (got < 0) {
            // Keep what arrived, so a later read does not lose it.
            compactInput(s);
            s->inbuf.insert(0, out);
            return from<Nil>(Nil{});
        }
        if (got == 0) {
            break;
        }
        out.append(chunk, static_cast<size_t>(got));
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(out)))};
}

static Value socketReadlineNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readline");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    compactInput(s);
    size_t scanned = 0;
    for (;;) {
        size_t nl = s->inbuf.find('\n', scanned);
        if (nl != std::string::npos) {
            std::string line = s->inbuf.substr(0, nl);
            s->inPos = nl + 1;
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
            std::string line = std::move(s->inbuf);
            s->inbuf.clear();
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
    compactInput(s);
    if (!fillToEof(s, "readlines")) {
        return from<Nil>(Nil{});
    }
    std::string data = std::move(s->inbuf);
    s->inbuf.clear();
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
