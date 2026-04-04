#pragma once

#include "object.h"

#include <cstdint>
#include <string_view>

struct ObjHandle {
    uint32_t index;
    ObjType type;
    bool operator==(const ObjHandle& other) const {
        return index == other.index && type == other.type;
    }
    bool operator!=(const ObjHandle& other) const { return !(*this == other); }
};

class Allocator {
  public:
    virtual ObjHandle makeString(std::string_view chars) = 0;
    virtual Obj* deref(ObjHandle handle) const = 0;
    virtual void collect() = 0;
    virtual ~Allocator() = default;
};
