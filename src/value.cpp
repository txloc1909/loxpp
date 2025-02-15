#include "value.h"

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
