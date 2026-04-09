#include "table.h"

#include <cstring>

#include "object.h"

Table::Table(VmAllocator<Entry> alloc) : m_buckets(alloc) {}

Entry* Table::findBucket(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % static_cast<uint32_t>(capacity);
    Entry* tombstone = nullptr;
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == nullptr) {
            if (is<Nil>(entry->value))
                return tombstone != nullptr ? tombstone : entry;
            if (tombstone == nullptr)
                tombstone = entry;
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) % static_cast<uint32_t>(capacity);
    }
}

void Table::adjustCapacity(int newCapacity) {
    Storage fresh(newCapacity, Entry{}, m_buckets.get_allocator());
    int saved = 0;
    for (const Entry& entry : m_buckets) {
        if (entry.key == nullptr)
            continue;
        Entry* dest = findBucket(fresh.data(), newCapacity, entry.key);
        *dest = entry;
        ++saved;
    }
    m_count = saved;
    m_buckets = std::move(fresh);
}

bool Table::set(ObjString* key, Value value) {
    int cap = static_cast<int>(m_buckets.size());
    if (m_count + 1 > static_cast<int>(cap * MAX_LOAD)) {
        int newCap = cap < 8 ? 8 : cap * 2;
        adjustCapacity(newCap);
        cap = static_cast<int>(m_buckets.size());
    }

    Entry* entry = findBucket(m_buckets.data(), cap, key);
    bool isNewKey = (entry->key == nullptr);
    if (isNewKey && is<Nil>(entry->value))
        m_count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

bool Table::get(ObjString* key, Value& out) const {
    if (m_count == 0)
        return false;
    int cap = static_cast<int>(m_buckets.size());
    Entry* entry = findBucket(const_cast<Entry*>(m_buckets.data()), cap, key);
    if (entry->key == nullptr)
        return false;
    out = entry->value;
    return true;
}

bool Table::del(ObjString* key) {
    if (m_count == 0)
        return false;
    int cap = static_cast<int>(m_buckets.size());
    Entry* entry = findBucket(m_buckets.data(), cap, key);
    if (entry->key == nullptr)
        return false;
    entry->key = nullptr;
    entry->value = Value{true};
    return true;
}

void Table::addAll(const Table& from) {
    for (const Entry& entry : from.m_buckets) {
        if (entry.key != nullptr)
            set(entry.key, entry.value);
    }
}

ObjString* Table::findString(const char* chars, int length,
                             uint32_t hash) const {
    if (m_count == 0)
        return nullptr;
    int cap = static_cast<int>(m_buckets.size());
    uint32_t index = hash % static_cast<uint32_t>(cap);
    for (;;) {
        const Entry& entry = m_buckets[index];
        if (entry.key == nullptr) {
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

void Table::removeUnmarkedKeys() {
    for (auto& entry : m_buckets) {
        if (entry.key != nullptr && !entry.key->marked) {
            entry.key = nullptr;
            entry.value = Value{true}; // tombstone (same as del())
        }
    }
}
