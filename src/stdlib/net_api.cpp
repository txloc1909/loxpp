#include "net_api.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
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
static ObjClass* s_socketClass = nullptr;
static ObjClass* s_serverClass = nullptr;

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
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }

    int fd = -1;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
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
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }
    FILE* fp = fdopen(fd, "r+");
    if (fp == nullptr) {
        int err = errno;
        ::close(fd);
        std::string msg = "connect(): cannot open stream for '" +
                          hostPort(host, port) + "': " + std::strerror(err);
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }

    MemoryManager* mm = getActiveMM();
    ObjSocket* sock = mm->create<ObjSocket>(s_socketClass);
    mm->pushTempRoot(sock);
    sock->fd = fd;
    sock->handle = fp;
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
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }

    int fd = -1;
    int lastErrno = 0;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
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
        nativeRuntimeError(msg.c_str());
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
// methods do. A read on a closed socket and a write after close_write() are
// fatal, matching spec/05-stdlib.md's Socket section.
static ObjSocket* checkSocketRead(Value* args, const char* method) {
    ObjSocket* s = asObjSocket(as<Obj*>(args[-1]));
    if (s->handle == nullptr) {
        std::string msg = "Cannot call '";
        msg += method;
        msg += "' on a closed socket.";
        nativeRuntimeError(msg.c_str());
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
        nativeRuntimeError("Cannot write to a socket after close_write().");
        return nullptr;
    }
    return s;
}

static Value socketReadNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "read");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string buf;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), s->handle)) > 0) {
        buf.append(chunk, got);
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value socketReadlineNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readline");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string line;
    bool sawByte = false;
    int c;
    while ((c = std::fgetc(s->handle)) != EOF) {
        sawByte = true;
        if (c == '\n') {
            break;
        }
        line.push_back(static_cast<char>(c));
    }
    if (!sawByte) {
        return from<Nil>(Nil{});
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
}

static Value socketReadlinesNative(int /*argc*/, Value* args) {
    ObjSocket* s = checkSocketRead(args, "readlines");
    if (s == nullptr) {
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    std::string line;
    bool sawByte = false;
    int c;
    bool done = false;
    while (!done) {
        c = std::fgetc(s->handle);
        if (c == EOF) {
            done = true;
        } else {
            sawByte = true;
            if (c == '\n') {
                ObjString* str = mm->makeString(line);
                mm->pushTempRoot(str);
                list->elements.emplace_back(static_cast<Obj*>(str));
                mm->popTempRoot();
                line.clear();
                sawByte = false; // a fresh line begins after a newline
            } else {
                line.push_back(static_cast<char>(c));
            }
        }
    }
    if (sawByte) {
        ObjString* str = mm->makeString(line);
        mm->pushTempRoot(str);
        list->elements.emplace_back(static_cast<Obj*>(str));
        mm->popTempRoot();
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
    std::fwrite(str->chars.data(), 1, str->chars.size(), s->handle);
    std::fflush(s->handle);
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
    std::fwrite(str->chars.data(), 1, str->chars.size(), s->handle);
    std::fputc('\n', s->handle);
    std::fflush(s->handle);
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
    if (s->handle != nullptr) {
        std::fclose(s->handle);
        s->handle = nullptr;
        s->fd = -1;
    }
    return from<Nil>(Nil{});
}

static Value serverAcceptNative(int /*argc*/, Value* args) {
    ObjServer* s = asObjServer(as<Obj*>(args[-1]));
    if (s->fd < 0) {
        nativeRuntimeError("Cannot call 'accept' on a closed server.");
        return from<Nil>(Nil{});
    }
    int cfd;
    do {
        cfd = accept(s->fd, nullptr, nullptr);
    } while (cfd < 0 && errno == EINTR);
    if (cfd < 0) {
        std::string msg = std::string("accept(): ") + std::strerror(errno);
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }
    FILE* fp = fdopen(cfd, "r+");
    if (fp == nullptr) {
        ::close(cfd);
        nativeRuntimeError("accept(): cannot open stream for connection.");
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjSocket* sock = mm->create<ObjSocket>(s_socketClass);
    mm->pushTempRoot(sock);
    sock->fd = cfd;
    sock->handle = fp;
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
