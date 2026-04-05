#pragma once

#include "allocator.h"
#include "lox_allocator.h"
#include "table.h"

#include <cstddef>

class SimpleAllocator : public Allocator {
  public:
    SimpleAllocator();
    ~SimpleAllocator() override;

    // Marked final so the compiler can devirtualize calls and clang-tidy's
    // VirtualCall-during-destruction checker is satisfied.
    void* reallocate(void* ptr, size_t oldSize, size_t newSize) final;

    using Allocator::makeString;
    ObjHandle makeString(std::string_view chars) override;
    ObjHandle makeString(std::string&& chars) override;
    ObjString* findString(std::string_view chars) const override;
    Obj* deref(ObjHandle handle) const override;

    void markObject(Obj* obj) override;
    void collect() override;

    size_t bytesAllocated() const override { return m_bytesAllocated; }

  private:
    // Non-virtual core of reallocate; safe to call during destruction.
    void* reallocateImpl(void* ptr, size_t oldSize, size_t newSize);
    void freeObject(Obj* obj);

    LoxVector<Obj*> m_objects;
    Table m_strings;
    std::size_t m_bytesAllocated = 0;
};
