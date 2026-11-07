#pragma once

#include "../value.h"

#include <cstdint>

// A standard-library parameter that spec/05-stdlib.md, "Integer arguments",
// says must be an integer. The spec names both kinds there, so this is the one
// place that accepts an Int where a Number's integer value is expected: no
// other check may name both kinds, because the Int kind and the Number kind do
// not mix (spec/03-types.md, Int).
enum class IntegerArgRead : std::uint8_t {
    Ok,         // an Int or an integer-valued Number; `out` holds the value
    NotNumber,  // neither a Number nor an Int
    NotInteger, // a Number that is not integer-valued: fractional, NaN, inf
    OutOfInt64, // a Number outside the Int range, so outside every range
};

IntegerArgRead readIntegerArg(const Value& value, Int& out);
