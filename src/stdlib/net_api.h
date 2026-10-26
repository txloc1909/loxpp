#pragma once
#include "stdlib_registrar.h"
#include "../class_objects.h"

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
