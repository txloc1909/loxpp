#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core_hash_map.h"
#include "value.h"
#include "vm_allocator.h"

struct Entry {
    ObjString* key{nullptr};
    Value value{Nil{}};
};

struct TablePolicy {
    static bool isEmpty(const Entry& e) {
        return e.key == nullptr && is<Nil>(e.value);
    }
    static bool isTombstone(const Entry& e) {
        return e.key == nullptr && !is<Nil>(e.value);
    }
    static void makeTombstone(Entry& e) {
        e.key = nullptr;
        e.value = Value{true};
    }
    // forEach only yields live entries, so e.key is never null here.
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    static uint32_t hashOf(const Entry& e) { return e.key->hash; }
    static bool keyMatch(const Entry& slot, const Entry& needle) {
        return slot.key == needle.key;
    }
};

class Table {
    using Map = CoreHashMap<Entry, TablePolicy, VmAllocator<Entry>>;
    Map m_map;

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
        m_map.forEach([&fn](const Entry& e) { fn(e.key, e.value); });
    }
};

// hashString is defined inline in object.h; declared here for callers that
// include table.h without object.h.
uint32_t hashString(std::string_view s);
