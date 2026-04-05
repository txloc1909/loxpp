#include "table.h"

#include <cstring>

#include "object.h"

using VecEntry = std::vector<Entry, LoxAllocator<Entry>>;

uint32_t hashString(std::string_view s) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

Table::Table(Allocator* alloc) : VecEntry(LoxAllocator<Entry>{alloc}) {}

// Returns the bucket where key belongs (or the first tombstone along the probe
// sequence if key isn't present). Callers must ensure capacity > 0.
Entry* Table::findBucket(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % static_cast<uint32_t>(capacity);
    Entry* tombstone = nullptr;
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == nullptr) {
            if (is<Nil>(entry->value)) {
                // Truly empty — key not present; prefer tombstone slot if any.
                return tombstone != nullptr ? tombstone : entry;
            }
            // Tombstone (key=null, value=true).
            if (tombstone == nullptr)
                tombstone = entry;
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) % static_cast<uint32_t>(capacity);
    }
}

void Table::adjustCapacity(int newCapacity) {
    // Allocate fresh bucket array (same allocator) and re-insert live entries
    // (tombstones dropped).
    VecEntry fresh(newCapacity, get_allocator());
    m_count = 0;
    for (const Entry& entry : static_cast<const VecEntry&>(*this)) {
        if (entry.key == nullptr)
            continue;
        Entry* dest = findBucket(fresh.data(), newCapacity, entry.key);
        dest->key = entry.key;
        dest->value = entry.value;
        m_count++;
    }
    static_cast<VecEntry&>(*this) = std::move(fresh);
}

bool Table::set(ObjString* key, Value value) {
    int cap = static_cast<int>(VecEntry::size());
    if (m_count + 1 > static_cast<int>(cap * MAX_LOAD)) {
        int newCap = cap < 8 ? 8 : cap * 2;
        adjustCapacity(newCap);
        cap = static_cast<int>(VecEntry::size());
    }

    Entry* entry = findBucket(VecEntry::data(), cap, key);
    bool isNewKey = (entry->key == nullptr);
    // Only increment count when filling a truly empty slot (not a tombstone).
    if (isNewKey && is<Nil>(entry->value))
        m_count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

bool Table::get(ObjString* key, Value& out) const {
    if (m_count == 0)
        return false;
    int cap = static_cast<int>(VecEntry::size());
    // const_cast is safe: findBucket doesn't modify the entry on a get path.
    Entry* entry = findBucket(const_cast<Entry*>(VecEntry::data()), cap, key);
    if (entry->key == nullptr)
        return false;
    out = entry->value;
    return true;
}

bool Table::del(ObjString* key) {
    if (m_count == 0)
        return false;
    int cap = static_cast<int>(VecEntry::size());
    Entry* entry = findBucket(VecEntry::data(), cap, key);
    if (entry->key == nullptr)
        return false;
    // Place tombstone: key=null, value=true (bool).
    entry->key = nullptr;
    entry->value = Value{true};
    return true;
}

void Table::addAll(const Table& from) {
    for (const Entry& entry : static_cast<const VecEntry&>(from)) {
        if (entry.key != nullptr)
            set(entry.key, entry.value);
    }
}

ObjString* Table::findString(const char* chars, int length,
                             uint32_t hash) const {
    if (m_count == 0)
        return nullptr;
    int cap = static_cast<int>(VecEntry::size());
    uint32_t index = hash % static_cast<uint32_t>(cap);
    for (;;) {
        const Entry& entry = VecEntry::data()[index];
        if (entry.key == nullptr) {
            // Stop only on a truly empty slot; skip tombstones.
            if (is<Nil>(entry.value))
                return nullptr;
        } else if (static_cast<int>(entry.key->chars.size()) == length &&
                   entry.key->hash == hash &&
                   memcmp(entry.key->chars.data(), chars, length) == 0) {
            return entry.key;
        }
        index = (index + 1) % static_cast<uint32_t>(cap);
    }
}
