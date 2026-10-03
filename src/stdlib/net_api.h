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
