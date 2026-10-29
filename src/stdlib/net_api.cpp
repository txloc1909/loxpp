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
#include <algorithm>
#include <climits>
#include <memory>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

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

static std::string closedMessage(const char* method, const char* noun) {
    return std::string("Cannot call '") + method + "' on a closed " + noun +
           ".";
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

static Value runIo(ParkedIo io);

// The state of a connect() between its attempts. The descriptor of the attempt
// in progress belongs to it: no Socket exists yet, so no close can reach it,
// and the end of the task destroys the state.
struct ConnectState {
    std::string host;
    int port{0};
    std::vector<ConnectAddress> addrs;
    std::size_t next{0};
    int fd{-1};
    int lastErrno{0};

    ConnectState() = default;
    ConnectState(const ConnectState&) = delete;
    ConnectState& operator=(const ConnectState&) = delete;
    ~ConnectState() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

static thread_local const ConnectTestHooks* s_connectHooks = nullptr;

void setConnectTestHooks(const ConnectTestHooks* hooks) {
    s_connectHooks = hooks;
}

// Resolves the name on the worker, and runs the connect from the first
// address. The TCP connect is nonblocking: the call parks where it can.
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

    auto state = std::make_shared<ConnectState>();
    state->host = host;
    state->port = port;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        ConnectAddress a;
        std::memset(&a, 0, sizeof(a));
        a.family = p->ai_family;
        a.socktype = p->ai_socktype;
        a.protocol = p->ai_protocol;
        a.len = p->ai_addrlen;
        std::memcpy(&a.addr, p->ai_addr, p->ai_addrlen);
        state->addrs.push_back(a);
    }
    freeaddrinfo(res);
    if (s_connectHooks != nullptr && s_connectHooks->addresses) {
        s_connectHooks->addresses(state->addrs);
    }

    ParkedIo io;
    io.op = IoOp::CONNECT;
    io.method = "connect";
    io.connect = std::move(state);
    return runIo(std::move(io));
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
        fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    p->ai_protocol);
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
        raiseNet(kSocketClosedError, closedMessage(method, "socket"));
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

// Every read-side call is a step function over the socket and a ParkedIo. A
// step does as much as it can without waiting and reports one of three
// outcomes. The caller decides what waiting means: a task that cannot suspend
// waits in the kernel, and a task that can suspend parks and runs the step
// again when the descriptor is ready. A step keeps what it has done in the
// ParkedIo and in the socket, so a call that waits many times loses nothing.
struct IoOutcome {
    enum class Kind : unsigned char { Done, Wait, Fault };
    Kind kind{Kind::Wait};
    bool isNil{false};
    std::string text; // the result; for readlines, the whole stream
    int fd{-1};       // accept: the descriptor of the new connection
    const char* faultKind{nullptr};
    std::string faultMessage;
};

static void setFault(IoOutcome& out, const char* kind, std::string message) {
    out.kind = IoOutcome::Kind::Fault;
    out.faultKind = kind;
    out.faultMessage = std::move(message);
}

static void setDone(IoOutcome& out, std::string text) {
    out.kind = IoOutcome::Kind::Done;
    out.text = std::move(text);
}

enum class Fill : unsigned char { Data, Eof, Block, Failed };

// Reads up to `want` bytes and appends them to `buf`, with EINTR retried.
// `buf` is unchanged unless bytes arrive. A failure other than EINTR and
// EAGAIN is a NetworkError, not an end of stream; `err` holds its message.
static Fill appendRead(int fd, std::string& buf, size_t want,
                       const char* method, std::string& err) {
    size_t old = buf.size();
    buf.resize(old + want);
    for (;;) {
        ssize_t n = ::read(fd, &buf[old], want);
        if (n > 0) {
            buf.resize(old + static_cast<size_t>(n));
            return Fill::Data;
        }
        buf.resize(old);
        if (n == 0) {
            return Fill::Eof;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Fill::Block;
        }
        err = std::string(method) + "(): " + std::strerror(errno);
        return Fill::Failed;
    }
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
    s->noNewlineLen = s->noNewlineLen > n ? s->noNewlineLen - n : 0;
    std::string tail(s->inbuf, n);
    std::string out = std::move(s->inbuf);
    out.resize(n);
    s->inbuf = std::move(tail);
    return out;
}

// Drop the first byte of the input buffer, which is the newline that ended a
// line.
static void dropFirstInput(ObjSocket* s) { s->inbuf.erase(0, 1); }

// Append one read chunk to the input buffer.
static Fill fillInput(ObjSocket* s, const char* method, IoOutcome& out) {
    Fill f = appendRead(s->fd, s->inbuf, kReadChunk, method, out.faultMessage);
    if (f == Fill::Failed) {
        setFault(out, kNetworkError, std::move(out.faultMessage));
    }
    return f;
}

// Read to the end of the stream into the input buffer, or until the descriptor
// has no more bytes yet. Sets `out` for a fault or an end of stream, and
// leaves it at Wait when the call must wait; the bytes that arrived stay in
// the buffer.
static void fillToEof(ObjSocket* s, const char* method, IoOutcome& out) {
    for (;;) {
        switch (fillInput(s, method, out)) {
        case Fill::Data:
            break;
        case Fill::Eof:
            out.kind = IoOutcome::Kind::Done;
            return;
        case Fill::Block:
            out.kind = IoOutcome::Kind::Wait;
            return;
        case Fill::Failed:
            return;
        }
    }
}

static void stepRead(ObjSocket* s, ParkedIo& io, IoOutcome& out) {
    fillToEof(s, io.method, out);
    if (out.kind == IoOutcome::Kind::Done) {
        setDone(out, takeInput(s, s->inbuf.size()));
    }
}

static void stepReadBytes(ObjSocket* s, ParkedIo& io, IoOutcome& out) {
    size_t take = std::min(s->inbuf.size(), io.want - io.buf.size());
    if (take > 0) {
        if (io.buf.empty()) {
            io.buf = takeInput(s, take);
        } else {
            io.buf.append(takeInput(s, take));
        }
    }
    // Read the rest straight into the result: the want is bounded by the
    // count, so no byte past it is read and none needs buffering.
    while (io.buf.size() < io.want) {
        size_t want = std::min(io.want - io.buf.size(), kReadChunk);
        switch (appendRead(s->fd, io.buf, want, io.method, out.faultMessage)) {
        case Fill::Data:
            continue;
        case Fill::Eof:
            setDone(out, std::move(io.buf));
            return;
        case Fill::Block:
            out.kind = IoOutcome::Kind::Wait;
            return;
        case Fill::Failed:
            // Keep what arrived, so a later read does not lose it.
            io.buf.append(s->inbuf);
            s->inbuf = std::move(io.buf);
            setFault(out, kNetworkError, std::move(out.faultMessage));
            return;
        }
    }
    setDone(out, std::move(io.buf));
}

static void stepReadline(ObjSocket* s, ParkedIo& io, IoOutcome& out) {
    // The scan resumes at `noNewlineLen`, not at the front: a line that
    // arrives in small pieces would otherwise be scanned again in full at
    // each wake-up, and the total cost would grow with the square of the line
    // length. Another task that waits on the same socket can take bytes from
    // the front while this call is parked, which is why the offset lives in
    // the socket and every removal lowers it.
    for (;;) {
        s->scannedBytes += s->inbuf.size() - s->noNewlineLen;
        size_t nl = s->inbuf.find('\n', s->noNewlineLen);
        if (nl != std::string::npos) {
            std::string line = takeInput(s, nl);
            dropFirstInput(s);
            setDone(out, std::move(line));
            return;
        }
        s->noNewlineLen = s->inbuf.size();
        switch (fillInput(s, io.method, out)) {
        case Fill::Data:
            break;
        case Fill::Eof:
            if (s->inbuf.empty()) {
                out.kind = IoOutcome::Kind::Done;
                out.isNil = true;
            } else {
                setDone(out, takeInput(s, s->inbuf.size()));
            }
            return;
        case Fill::Block:
            out.kind = IoOutcome::Kind::Wait;
            return;
        case Fill::Failed:
            return;
        }
    }
}

static void stepAccept(ObjServer* srv, IoOutcome& out) {
    int cfd;
    do {
        cfd = accept4(srv->fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    } while (cfd < 0 && errno == EINTR);
    if (cfd >= 0) {
        out.kind = IoOutcome::Kind::Done;
        out.fd = cfd;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        out.kind = IoOutcome::Kind::Wait;
    } else {
        setFault(out, kNetworkError,
                 std::string("accept(): ") + std::strerror(errno));
    }
}

enum class Ended : unsigned char { No, Yes, Failed };

// Whether the connect in progress on `fd` has ended, with success or with an
// error. A zero timeout: the answer never waits. On loopback the handshake is
// over before connect(2) returns, so the call then does not park. A failure of
// poll(2) says nothing about the connect, so it is reported as Failed with
// errno kept.
static Ended attemptEnded(int fd) {
    for (;;) {
        int r;
        if (s_connectHooks != nullptr && s_connectHooks->pollCall) {
            r = s_connectHooks->pollCall(fd);
        } else {
            pollfd pfd = {fd, POLLOUT, 0};
            r = ::poll(&pfd, 1, 0);
        }
        if (r > 0) {
            return Ended::Yes;
        }
        if (r == 0) {
            return Ended::No;
        }
        if (errno != EINTR) {
            return Ended::Failed;
        }
    }
}

// Reads the result of the attempt in progress. True when it connected: the
// descriptor moves to `out`. False when it failed: the error is recorded and
// the descriptor is closed.
static bool settleAttempt(ConnectState& c, IoOutcome& out) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        err = errno;
    }
    if (err == 0) {
        out.kind = IoOutcome::Kind::Done;
        out.fd = c.fd;
        c.fd = -1;
        return true;
    }
    c.lastErrno = err;
    ::close(c.fd);
    c.fd = -1;
    return false;
}

// One step of connect: finishes the attempt in progress if there is one, then
// starts attempts in address order until one connects or waits. The attempt
// that waits keeps its descriptor in the state. An attempt that ended is read
// with SO_ERROR; connect(2) is never called twice on one descriptor.
static void stepConnect(ConnectState& c, IoOutcome& out) {
    if (c.fd >= 0) {
        switch (attemptEnded(c.fd)) {
        case Ended::No:
            out.kind = IoOutcome::Kind::Wait;
            return;
        case Ended::Failed:
            c.lastErrno = errno;
            ::close(c.fd);
            c.fd = -1;
            break;
        case Ended::Yes:
            if (settleAttempt(c, out)) {
                return;
            }
            break;
        }
    }
    while (c.next < c.addrs.size()) {
        const ConnectAddress& a = c.addrs[c.next++];
        int fd = socket(a.family, a.socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                        a.protocol);
        if (fd < 0) {
            c.lastErrno = errno;
            continue;
        }
        const auto* sa = reinterpret_cast<const sockaddr*>(&a.addr);
        int r = s_connectHooks != nullptr && s_connectHooks->connectCall
                    ? s_connectHooks->connectCall(fd, sa, a.len)
                    : ::connect(fd, sa, a.len);
        if (r == 0) {
            out.kind = IoOutcome::Kind::Done;
            out.fd = fd;
            return;
        }
        // After EINTR the kernel goes on with the connect, so the call treats
        // it like EINPROGRESS.
        if (errno == EINPROGRESS || errno == EINTR) {
            c.fd = fd;
            switch (attemptEnded(fd)) {
            case Ended::No:
                out.kind = IoOutcome::Kind::Wait;
                return;
            case Ended::Failed:
                c.lastErrno = errno;
                ::close(fd);
                c.fd = -1;
                continue;
            case Ended::Yes:
                if (settleAttempt(c, out)) {
                    return;
                }
                continue;
            }
        }
        c.lastErrno = errno;
        ::close(fd);
    }
    setFault(out, kNetworkError,
             "connect(): cannot connect to '" + hostPort(c.host, c.port) +
                 "': " + std::strerror(c.lastErrno));
}

static void step(ParkedIo& io, IoOutcome& out) {
    if (io.op == IoOp::CONNECT) {
        stepConnect(*io.connect, out);
        return;
    }
    if (io.op == IoOp::ACCEPT) {
        auto* srv = asObjServer(io.target);
        if (srv->fd < 0) {
            setFault(out, kSocketClosedError,
                     closedMessage(io.method, "server"));
            return;
        }
        stepAccept(srv, out);
        return;
    }
    auto* s = asObjSocket(io.target);
    if (s->fd < 0) {
        setFault(out, kSocketClosedError, closedMessage(io.method, "socket"));
        return;
    }
    switch (io.op) {
    case IoOp::READ:
    case IoOp::READLINES:
        stepRead(s, io, out);
        return;
    case IoOp::READ_BYTES:
        stepReadBytes(s, io, out);
        return;
    case IoOp::READLINE:
        stepReadline(s, io, out);
        return;
    case IoOp::ACCEPT:
    case IoOp::WRITE:
    case IoOp::CONNECT:
    case IoOp::NONE:
        return;
    }
}

static int descriptorOf(const ParkedIo& io) {
    if (io.op == IoOp::CONNECT) {
        return io.connect->fd;
    }
    return io.op == IoOp::ACCEPT ? asObjServer(io.target)->fd
                                 : asObjSocket(io.target)->fd;
}

// The value of a finished call.
static Value finish(const ParkedIo& io, IoOutcome& out) {
    MemoryManager* mm = getActiveMM();
    if (io.op == IoOp::ACCEPT || io.op == IoOp::CONNECT) {
        ObjSocket* sock = mm->create<ObjSocket>(s_socketClass);
        mm->pushTempRoot(sock);
        sock->fd = out.fd;
        mm->popTempRoot();
        return Value{static_cast<Obj*>(sock)};
    }
    if (out.isNil) {
        return from<Nil>(Nil{});
    }
    if (io.op != IoOp::READLINES) {
        return Value{static_cast<Obj*>(mm->makeString(std::move(out.text)))};
    }
    const std::string& data = out.text;
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        size_t end = nl == std::string::npos ? data.size() : nl;
        // A view, not a substr: a substr would hold a second copy of the line
        // while makeString copies it into the object.
        ObjString* str =
            mm->makeString(std::string_view(data).substr(start, end - start));
        mm->pushTempRoot(str);
        list->elements.emplace_back(static_cast<Obj*>(str));
        mm->popTempRoot();
        start = end + 1;
    }
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

// Waits in the kernel until `fd` is ready for `events`. The worker does not run
// another task meanwhile. False, with `err` set, when poll(2) itself fails.
static bool waitFd(int fd, short events, std::string& err) {
    struct pollfd pfd = {fd, events, 0};
    while (::poll(&pfd, 1, -1) < 0) {
        if (errno != EINTR) {
            err = std::strerror(errno);
            return false;
        }
    }
    return true;
}

// Runs a call to its end, or to the point where the task parks.
static Value runIo(ParkedIo io) {
    Runtime* rt = getActiveRuntime();
    for (;;) {
        IoOutcome out;
        step(io, out);
        switch (out.kind) {
        case IoOutcome::Kind::Done:
            return finish(io, out);
        case IoOutcome::Kind::Fault:
            raiseNet(out.faultKind, out.faultMessage);
            return from<Nil>(Nil{});
        case IoOutcome::Kind::Wait:
            break;
        }
        // The task leaves the run queue; the poller reports the descriptor and
        // resumeParkedIo goes on. The record moves to the task, because this
        // frame is gone by then. A task that cannot suspend waits here.
        const int fd = descriptorOf(io);
        const bool connecting = io.op == IoOp::CONNECT;
        if (rt->parkCurrentTask(
                connecting ? ParkReason::FD_WRITE : ParkReason::FD_READ, fd)) {
            rt->currentTask()->io = std::move(io);
            return from<Nil>(Nil{});
        }
        std::string err;
        if (!waitFd(fd, connecting ? POLLOUT : POLLIN, err)) {
            raiseNet(kNetworkError, std::string(io.method) + "(): " + err);
            return from<Nil>(Nil{});
        }
    }
}

static void pumpWriters(ObjSocket* s);

void resumeParkedIo(Runtime& rt, ObjTask* task) {
    ParkedIo& io = task->io;
    if (io.op == IoOp::WRITE) {
        // The pump finishes this call too when its turn and the room allow.
        pumpWriters(asObjSocket(io.target));
        if (task->state == TaskState::PARKED) {
            rt.rearmPark(task, descriptorOf(io));
        }
        return;
    }
    IoOutcome out;
    step(io, out);
    switch (out.kind) {
    case IoOutcome::Kind::Done:
        rt.completePark(task, finish(io, out));
        return;
    case IoOutcome::Kind::Fault:
        rt.failPark(task, out.faultKind, out.faultMessage.c_str());
        return;
    case IoOutcome::Kind::Wait:
        rt.rearmPark(task, descriptorOf(io));
        return;
    }
}

static Value socketReadNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "read");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    ParkedIo io;
    io.op = IoOp::READ;
    io.target = s;
    io.method = "read";
    return runIo(std::move(io));
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
    ParkedIo io;
    io.op = IoOp::READ_BYTES;
    io.target = s;
    io.method = "read_bytes";
    io.want = n;
    return runIo(std::move(io));
}

static Value socketReadlineNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readline");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    ParkedIo io;
    io.op = IoOp::READLINE;
    io.target = s;
    io.method = "readline";
    return runIo(std::move(io));
}

static Value socketReadlinesNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readlines");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    ParkedIo io;
    io.op = IoOp::READLINES;
    io.target = s;
    io.method = "readlines";
    return runIo(std::move(io));
}

// Sends the bytes of a write call that the descriptor has not taken yet: the
// rest of the String and then the newline, in one system call. MSG_NOSIGNAL
// makes a peer that stopped reading fail with EPIPE and no signal. A
// descriptor has no error flag, so a failed write leaves nothing that a later
// read could see.
static void stepWrite(int fd, ParkedIo& io, IoOutcome& out) {
    auto* str = asObjString(io.text);
    const std::string_view text(str->chars.data(), str->chars.size());
    const size_t total = text.size() + (io.newline ? 1 : 0);
    while (io.offset < total) {
        struct iovec iov[2];
        int count = 0;
        if (io.offset < text.size()) {
            iov[count].iov_base = const_cast<char*>(text.data() + io.offset);
            iov[count].iov_len = text.size() - io.offset;
            ++count;
        }
        if (io.newline) {
            static char newline = '\n';
            iov[count].iov_base = &newline;
            iov[count].iov_len = 1;
            ++count;
        }
        struct msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = static_cast<size_t>(count);
        ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n >= 0) {
            io.offset += static_cast<size_t>(n);
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            out.kind = IoOutcome::Kind::Wait;
            return;
        }
        setFault(out, kNetworkError,
                 std::string(io.method) + "(): " + std::strerror(errno));
        return;
    }
    out.kind = IoOutcome::Kind::Done;
}

// A queue entry is live when its task still waits in a write on this socket.
static bool liveWriter(const ObjTask* task, const ObjSocket* s) {
    return task->state == TaskState::PARKED && task->io.op == IoOp::WRITE &&
           task->io.target == s;
}

// Sends for the parked writers in the order their calls began, and ends the
// calls that finish. It stops at the first writer that must wait for room, so
// a later writer never sends before an earlier one is done. Any task may call
// it: the one that was woken, or one that blocks its worker and cannot wait
// for the queue to move on its own.
static void pumpWriters(ObjSocket* s) {
    Runtime* rt = getActiveRuntime();
    while (s->hasWriters()) {
        ObjTask* task = s->frontWriter();
        if (!liveWriter(task, s)) {
            s->popWriter();
            continue;
        }
        IoOutcome out;
        stepWrite(s->fd, task->io, out);
        if (out.kind == IoOutcome::Kind::Wait) {
            return;
        }
        s->popWriter();
        if (out.kind == IoOutcome::Kind::Fault) {
            rt->failPark(task, out.faultKind, out.faultMessage.c_str());
        } else {
            rt->completePark(task, from<Nil>(Nil{}));
        }
    }
}

// Runs a write call to its end, or to the point where the task parks. A call
// that finds a writer ahead of it waits behind that writer, also when the
// descriptor has room.
static Value runWrite(ObjSocket* s, ParkedIo io) {
    Runtime* rt = getActiveRuntime();
    if (asObjString(io.text)->chars.empty() && !io.newline) {
        return from<Nil>(Nil{});
    }
    for (;;) {
        pumpWriters(s);
        if (!s->hasWriters()) {
            IoOutcome out;
            stepWrite(s->fd, io, out);
            if (out.kind == IoOutcome::Kind::Done) {
                return from<Nil>(Nil{});
            }
            if (out.kind == IoOutcome::Kind::Fault) {
                raiseNet(out.faultKind, out.faultMessage);
                return from<Nil>(Nil{});
            }
        }
        if (rt->parkCurrentTask(ParkReason::FD_WRITE, s->fd)) {
            ObjTask* task = rt->currentTask();
            task->io = std::move(io);
            s->pushWriter(task);
            return from<Nil>(Nil{});
        }
        std::string err;
        if (!waitFd(s->fd, POLLOUT, err)) {
            raiseNet(kNetworkError, std::string(io.method) + "(): " + err);
            return from<Nil>(Nil{});
        }
    }
}

static Value socketWriteCall(Value* args, const char* method, bool newline) {
    ObjSocket* s = checkSocketWrite(args, method);
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        std::string msg =
            std::string("'") + method + "' argument must be a string.";
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }
    ParkedIo io;
    io.op = IoOp::WRITE;
    io.target = s;
    io.method = method;
    io.text = as<Obj*>(args[0]);
    io.newline = newline;
    return runWrite(s, std::move(io));
}

static Value socketWriteNative(int /*argc*/, Value* args) {
    return socketWriteCall(args, "write", false);
}

static Value socketWritelineNative(int /*argc*/, Value* args) {
    return socketWriteCall(args, "writeline", true);
}

static Value socketCloseWriteNative(int /*argc*/, Value* args) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->fd >= 0 && !s->writeClosed) {
        // A write that waits cannot finish after this, so it fails now, in the
        // order the calls began, as a write after close_write() does.
        Runtime* rt = getActiveRuntime();
        std::deque<ObjTask*> pending = s->takeWriters();
        s->writeClosed = true;
        for (ObjTask* task : pending) {
            if (liveWriter(task, s)) {
                rt->failPark(task, kSocketClosedError,
                             "Cannot write to a socket after close_write().");
            }
        }
        shutdown(s->fd, SHUT_WR);
    }
    return from<Nil>(Nil{});
}

// Closes a descriptor that tasks of this worker may wait on. The waits end
// before the descriptor does: epoll keeps its reference to the open file after
// close(2), and the next socket can get the same number. Each parked call
// fails with the fault of a call on a closed socket, in the order the calls
// began to wait.
static void closeDescriptor(int& fd, const char* noun) {
    Runtime* rt = getActiveRuntime();
    std::vector<ObjTask*> waiting;
    rt->cancelFdWaits(fd, waiting);
    ::close(fd);
    fd = -1;
    for (ObjTask* task : waiting) {
        rt->failPark(task, kSocketClosedError,
                     closedMessage(task->io.method, noun).c_str());
    }
}

void closeParkedDescriptor(Runtime& rt, ObjTask* task) {
    if (task->io.op == IoOp::CONNECT) {
        // The attempt has no Socket. The end of the task closes it.
        return;
    }
    int& fd = task->io.op == IoOp::ACCEPT ? asObjServer(task->io.target)->fd
                                          : asObjSocket(task->io.target)->fd;
    if (task->io.op != IoOp::ACCEPT) {
        asObjSocket(task->io.target)->clearWriters();
    }
    if (fd < 0) {
        return;
    }
    std::vector<ObjTask*> waiting;
    rt.cancelFdWaits(fd, waiting);
    ::close(fd);
    fd = -1;
}

static Value socketCloseNative(int /*argc*/, Value* args) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->fd >= 0) {
        s->clearWriters();
        closeDescriptor(s->fd, "socket");
    }
    return from<Nil>(Nil{});
}

static Value serverAcceptNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    if (s->fd < 0) {
        raiseNet(kSocketClosedError, closedMessage("accept", "server"));
        return from<Nil>(Nil{});
    }
    ParkedIo io;
    io.op = IoOp::ACCEPT;
    io.target = s;
    io.method = "accept";
    return runIo(std::move(io));
}

static Value serverPortNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    return from<Number>(static_cast<Number>(s->boundPort));
}

static Value serverCloseNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    if (s->fd >= 0) {
        closeDescriptor(s->fd, "server");
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
