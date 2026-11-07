#pragma once

#include "../value.h"

#include <cstdint>

// A standard-library parameter that spec/05-stdlib.md, "Integer arguments",
// says must be an integer. The spec names both kinds there, so a program may
// pass an Int where an integer-valued Number is expected. `exit`'s code is not
// such a parameter — a Number code is truncated toward zero — so it reads its
// own Int case and does not call this function.
enum class IntegerArgRead : std::uint8_t {
    Ok,         // an Int or an integer-valued Number; `out` holds the value
    NotNumber,  // neither a Number nor an Int
    NotInteger, // a Number that is not integer-valued: fractional, NaN, inf
    OutOfInt64, // a Number outside the Int range, so outside every range
};

IntegerArgRead readIntegerArg(const Value& value, Int& out);
