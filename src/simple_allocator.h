#pragma once

#include "allocator.h"
#include "table.h"

#include <cstddef>
#include <vector>

class SimpleAllocator : public Allocator {
  public:
    SimpleAllocator() = default;
    ~SimpleAllocator() override;

    void* reallocate(void* ptr, size_t oldSize, size_t newSize) override;

    using Allocator::makeString;
    ObjHandle makeString(std::string_view chars) override;
    ObjHandle makeString(std::string&& chars) override;
    ObjString* findString(std::string_view chars) const override;
    Obj* deref(ObjHandle handle) const override;
    void collect() override;
    size_t bytesAllocated() const override { return m_bytesAllocated; }

  private:
    // Non-virtual core of reallocate; safe to call during destruction.
    void* reallocateImpl(void* ptr, size_t oldSize, size_t newSize);
    void freeObject(Obj* obj);

    std::vector<Obj*> m_objects;
    Table m_strings;
    std::size_t m_bytesAllocated = 0;
};
