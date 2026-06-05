#pragma once

#include "exec_objects.h"

#include <cstddef>

struct MathFnEntry {
    const char* name;
    NativeFn fn;
    int arity;
};

struct MathConstEntry {
    const char* name;
    double value;
};

extern const MathFnEntry kMathFunctions[];
extern const std::size_t kMathFunctionCount;

extern const MathConstEntry kMathConstants[];
extern const std::size_t kMathConstantCount;
