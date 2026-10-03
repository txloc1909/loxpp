#pragma once
#include "stdlib_registrar.h"
#include "../class_objects.h"

// Returns the Process class. Runtime stores it for GET_PROPERTY/INVOKE
// dispatch. `mapClass` stamps the Map run() returns, so it dispatches to the
// shared Map methods.
ObjClass* registerProcessAPI(StdlibRegistrar& reg, ObjClass* mapClass);
