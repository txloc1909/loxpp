#include "value.h"
#include "overload.h"

#include <cstdio>

void printValue(const Value& value) {
    std::visit(
        overloaded{
            [](bool val) { std::printf("%s", (val ? "true" : "false")); },
            [](Number val) { std::printf("%g", val); },
            [](Nil) { std::printf("nil"); },
        },
        value);
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
