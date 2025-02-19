#include "value.h"

#include <cstdio>

void printValue(const Value& value) {
    std::visit(
        [](const auto& val) {
            using DecayType = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<DecayType, bool>) {
                std::printf("%s", (val ? "true" : "false"));
            } else if constexpr (std::is_same_v<DecayType, Number>) {
                std::printf("%g", val);
            } else {
                static_assert(std::is_same_v<DecayType, Nil>);
                std::printf("nil");
            }
        },
        value);
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
