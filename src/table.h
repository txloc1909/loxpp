#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "value.h"
#include "vm_allocator.h"

struct Entry {
    ObjString* key{nullptr};
    Value value{Nil{}};
};

class Table {
    using Storage = std::vector<Entry, VmAllocator<Entry>>;
    Storage m_buckets;
    int m_count{0};

  public:
    explicit Table(VmAllocator<Entry> alloc);

    bool set(ObjString* key, Value value);
    bool get(ObjString* key, Value& out) const;
    bool del(ObjString* key);
    void addAll(const Table& from);
    ObjString* findString(const char* chars, int length, uint32_t hash) const;
    void removeUnmarkedKeys();

    template <typename F>
    void forEach(F&& fn) const {
        for (const auto& entry : m_buckets)
            if (entry.key != nullptr)
                fn(entry.key, entry.value);
    }

  private:
    static constexpr double MAX_LOAD = 0.75;
    static Entry* findBucket(Entry* entries, int capacity, ObjString* key);
    void adjustCapacity(int newCapacity);
};

// hashString is defined inline in object.h; declared here for callers that
// include table.h without object.h.
uint32_t hashString(std::string_view s);
