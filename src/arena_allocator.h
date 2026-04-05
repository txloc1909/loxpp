#pragma once

#include "allocator.h"
#include "object.h"
#include "table.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// ArenaAllocator allocates ObjString objects from pre-allocated memory blocks
// using placement new, avoiding one heap allocation per string object compared
// to SimpleAllocator's individual new/delete approach.
//
// Layout: objects are bump-allocated within fixed-size Blocks. When a Block is
// exhausted a new one is appended. Stable ObjString* pointers into the blocks
// are kept in m_objects for O(1) deref and orderly destruction.
//
// ObjString::chars (std::string) still individually heap-allocates; only the
// ObjString struct memory comes from the arena.
class ArenaAllocator : public Allocator {
  public:
    static constexpr std::size_t BLOCK_CAPACITY = 64;

    ArenaAllocator() = default;
    ~ArenaAllocator() override;

    using Allocator::makeString;
    ObjHandle makeString(std::string_view chars) override;
    ObjHandle makeString(std::string&& chars) override;
    ObjString* findString(std::string_view chars) const override;
    Obj* deref(ObjHandle handle) const override;
    void collect() override;

  private:
    struct Block {
        alignas(ObjString) char data[BLOCK_CAPACITY * sizeof(ObjString)];
        std::size_t count = 0;
    };

    // Bump-allocate one ObjString-sized slot from the current block,
    // allocating a fresh block when the current one is full.
    // Returns uninitialised memory — caller must use placement new.
    ObjString* allocSlot();

    ObjHandle internHandle(ObjString* str);

    std::vector<std::unique_ptr<Block>> m_blocks;
    std::vector<ObjString*>
        m_objects; // stable pointers for deref + destruction
    Table m_strings;
};
