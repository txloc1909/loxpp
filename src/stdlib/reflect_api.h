#pragma once
#include "stdlib_registrar.h"

void registerReflectAPI(StdlibRegistrar& reg);

// The name type(v) returns (spec/05-stdlib.md, type).
const char* valueTypeName(const Value& v);
