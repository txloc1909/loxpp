#pragma once
#include "stdlib_registrar.h"
#include "../class_objects.h"

// Registers the Coroutine class (its resume/status methods) and the global
// `coroutine` object with its `create` method. Returns the Coroutine class;
// Runtime stores it for GET_PROPERTY/INVOKE dispatch.
ObjClass* registerCoroutineAPI(StdlibRegistrar& reg);
