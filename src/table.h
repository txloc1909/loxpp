#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "lox_allocator.h"
#include "value.h"

struct ObjString;

struct Entry {
    ObjString* key{nullptr};
    Value value{Nil{}};
};

class Table : private std::vector<Entry, LoxAllocator<Entry>> {
  public:
    explicit Table(Allocator* alloc);

    // Returns true if a new key was inserted (false if existing key updated).
    bool set(ObjString* key, Value value);
    // Returns true and fills `out` if key is found.
    bool get(ObjString* key, Value& out) const;
    // Returns true if the key existed and was deleted (tombstoned).
    bool del(ObjString* key);
    // Copies all live entries from `from` into this table.
    void addAll(const Table& from);
    // String-interning lookup: finds an ObjString by raw chars + hash.
    ObjString* findString(const char* chars, int length, uint32_t hash) const;

  private:
    static constexpr double MAX_LOAD = 0.75;
    int m_count{0}; // live entries + tombstones (drives load-factor check)

    static Entry* findBucket(Entry* entries, int capacity, ObjString* key);
    void adjustCapacity(int newCapacity);
};

uint32_t hashString(std::string_view s);
