#include "table.h"

#include <cstring>

#include "object.h"

Table::Table(VmAllocator<Entry> alloc) : m_map(alloc) {}

bool Table::set(ObjString* key, Value value) {
    return m_map.set(Entry{key, value});
}

bool Table::get(ObjString* key, Value& out) const {
    const Entry* e =
        m_map.find(key->hash, [key](const Entry& s) { return s.key == key; });
    if (!e)
        return false;
    out = e->value;
    return true;
}

bool Table::del(ObjString* key) {
    return m_map.remove(key->hash,
                        [key](const Entry& s) { return s.key == key; });
}

void Table::addAll(const Table& from) {
    from.m_map.forEach([this](const Entry& e) { set(e.key, e.value); });
}

ObjString* Table::findString(const char* chars, int length,
                             uint32_t hash) const {
    if (m_map.count() == 0)
        return nullptr;
    int cap = m_map.capacity();
    uint32_t index = hash % static_cast<uint32_t>(cap);
    for (;;) {
        const Entry* entry = m_map.entryAt(index);
        if (TablePolicy::isEmpty(*entry))
            return nullptr;
        if (!TablePolicy::isTombstone(*entry) &&
            static_cast<int>(entry->key->chars.size()) == length &&
            entry->key->hash == hash &&
            memcmp(entry->key->chars.data(), chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) % static_cast<uint32_t>(cap);
    }
}

void Table::removeUnmarkedKeys() {
    m_map.removeIf([](const Entry& e) { return !e.key->marked; });
}
