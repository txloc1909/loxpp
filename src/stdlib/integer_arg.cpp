#include "integer_arg.h"

#include <cmath>

// 2^63 is exact, so it is the first Number above the Int range, and -2^63 is
// the smallest Int itself.
static constexpr double kTwoToThe63 = 9223372036854775808.0;

IntegerArgRead readIntegerArg(const Value& value, Int& out) {
    if (is<Int>(value)) {
        out = as<Int>(value);
        return IntegerArgRead::Ok;
    }
    if (!is<Number>(value)) {
        return IntegerArgRead::NotNumber;
    }
    const Number n = as<Number>(value);
    if (!std::isfinite(n) || n != std::floor(n)) {
        return IntegerArgRead::NotInteger;
    }
    if (n < -kTwoToThe63 || n >= kTwoToThe63) {
        return IntegerArgRead::OutOfInt64;
    }
    out = static_cast<Int>(n);
    return IntegerArgRead::Ok;
}
