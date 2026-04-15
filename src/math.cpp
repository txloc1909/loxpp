#include "math.h"

#include "native.h"
#include "value.h"

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Template helpers — replace error-prone macros with typed function templates.
// Each template is instantiated once per math function via a function pointer
// parameter, so the validation logic is shared and the compiler inlines freely.
// ---------------------------------------------------------------------------

using UnaryFn = double (*)(double);
using BinaryFn = double (*)(double, double);

template <UnaryFn F>
static Value unaryMath(int /*argc*/, Value* args) {
    if (!is<Number>(args[0])) {
        nativeRuntimeError("math function argument must be a number.");
        return from<Nil>(Nil{});
    }
    return from<Number>(F(as<Number>(args[0])));
}

template <BinaryFn F>
static Value binaryMath(int /*argc*/, Value* args) {
    if (!is<Number>(args[0]) || !is<Number>(args[1])) {
        nativeRuntimeError("math function arguments must be numbers.");
        return from<Nil>(Nil{});
    }
    return from<Number>(F(as<Number>(args[0]), as<Number>(args[1])));
}

// Thin wrappers that resolve overloaded std:: functions to their double
// signatures, giving unambiguous function pointers for template instantiation.
static double wrap_abs(double x) { return std::abs(x); }
static double wrap_ceil(double x) { return std::ceil(x); }
static double wrap_floor(double x) { return std::floor(x); }
static double wrap_round(double x) { return std::round(x); }
static double wrap_sqrt(double x) { return std::sqrt(x); }
static double wrap_cbrt(double x) { return std::cbrt(x); }
static double wrap_exp(double x) { return std::exp(x); }
static double wrap_log(double x) { return std::log(x); }
static double wrap_log2(double x) { return std::log2(x); }
static double wrap_log10(double x) { return std::log10(x); }
static double wrap_sin(double x) { return std::sin(x); }
static double wrap_cos(double x) { return std::cos(x); }
static double wrap_tan(double x) { return std::tan(x); }
static double wrap_asin(double x) { return std::asin(x); }
static double wrap_acos(double x) { return std::acos(x); }
static double wrap_atan(double x) { return std::atan(x); }
static double wrap_pow(double x, double y) { return std::pow(x, y); }
static double wrap_atan2(double y, double x) { return std::atan2(y, x); }
static double wrap_hypot(double x, double y) { return std::hypot(x, y); }
static double wrap_fmin(double x, double y) { return std::fmin(x, y); }
static double wrap_fmax(double x, double y) { return std::fmax(x, y); }

// ---------------------------------------------------------------------------
// Registration tables consumed by VM::defineMathObject()
// ---------------------------------------------------------------------------

const MathFnEntry kMathFunctions[] = {
    {"abs", unaryMath<wrap_abs>, 1},      {"ceil", unaryMath<wrap_ceil>, 1},
    {"floor", unaryMath<wrap_floor>, 1},  {"round", unaryMath<wrap_round>, 1},
    {"sqrt", unaryMath<wrap_sqrt>, 1},    {"cbrt", unaryMath<wrap_cbrt>, 1},
    {"exp", unaryMath<wrap_exp>, 1},      {"log", unaryMath<wrap_log>, 1},
    {"log2", unaryMath<wrap_log2>, 1},    {"log10", unaryMath<wrap_log10>, 1},
    {"sin", unaryMath<wrap_sin>, 1},      {"cos", unaryMath<wrap_cos>, 1},
    {"tan", unaryMath<wrap_tan>, 1},      {"asin", unaryMath<wrap_asin>, 1},
    {"acos", unaryMath<wrap_acos>, 1},    {"atan", unaryMath<wrap_atan>, 1},
    {"pow", binaryMath<wrap_pow>, 2},     {"atan2", binaryMath<wrap_atan2>, 2},
    {"hypot", binaryMath<wrap_hypot>, 2}, {"min", binaryMath<wrap_fmin>, 2},
    {"max", binaryMath<wrap_fmax>, 2},
};
const std::size_t kMathFunctionCount = std::size(kMathFunctions);

const MathConstEntry kMathConstants[] = {
    {"pi", M_PI},
    {"e", M_E},
    {"inf", std::numeric_limits<double>::infinity()},
    {"nan", std::numeric_limits<double>::quiet_NaN()},
};
const std::size_t kMathConstantCount = std::size(kMathConstants);
