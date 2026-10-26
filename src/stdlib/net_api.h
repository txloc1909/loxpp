#pragma once
#include "stdlib_registrar.h"
#include "../class_objects.h"

#include <functional>
#include <sys/socket.h>
#include <vector>

// Classes the two TCP globals build. Runtime stores both so its
// GET_PROPERTY/INVOKE dispatch can find each type's method table.
struct NetClasses {
    ObjClass* socket;
    ObjClass* server;
};

NetClasses registerNetAPI(StdlibRegistrar& reg);

class Runtime;
struct ObjTask;

// The poller reported the descriptor of a PARKED task that waits in a network
// call. Goes on with the call: the task completes with the call's value, fails
// with its fault, or waits again.
void resumeParkedIo(Runtime& rt, ObjTask* task);

// Closes the descriptor that a PARKED task waits on, at the end of the run.
void closeParkedDescriptor(Runtime& rt, ObjTask* task);

// One address that connect() may try.
struct ConnectAddress {
    int family;
    int socktype;
    int protocol;
    sockaddr_storage addr;
    socklen_t len;
};

// Replacements for the points of connect() that a test cannot reach on a real
// host: the list of resolved addresses, the connect(2) call, and the zero
// timeout poll(2). An empty member means the real call. The hooks of the
// calling thread apply; null clears them.
struct ConnectTestHooks {
    std::function<void(std::vector<ConnectAddress>&)> addresses;
    std::function<int(int, const sockaddr*, socklen_t)> connectCall;
    std::function<int(int)>
        pollCall; // returns what poll(2) returns for POLLOUT
};
void setConnectTestHooks(const ConnectTestHooks* hooks);
