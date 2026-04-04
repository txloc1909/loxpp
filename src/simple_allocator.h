#pragma once

#include "allocator.h"
#include "table.h"

#include <cstddef>
#include <vector>

class SimpleAllocator : public Allocator {
  public:
    SimpleAllocator() = default;
    ~SimpleAllocator() override;

    ObjHandle makeString(std::string_view chars) override;
    Obj* deref(ObjHandle handle) const override;
    void collect() override;

  private:
    std::vector<Obj*> m_objects;
    Table m_strings;
    std::size_t m_bytesAllocated = 0;
};
