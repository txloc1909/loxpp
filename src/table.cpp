#include "table.h"

#include <cstring>

#include "object.h"

static constexpr double TABLE_MAX_LOAD = 0.75;

uint32_t hashString(std::string_view s) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

static int growCapacity(int capacity) {
    return capacity < 8 ? 8 : capacity * 2;
}

// Returns the bucket where `key` belongs, or the first tombstone found along
// the probe sequence if `key` isn't present.  Callers must ensure capacity > 0.
static Entry* findEntry(Entry* entries, int capacity, ObjString* key) {
    uint32_t index = key->hash % static_cast<uint32_t>(capacity);
    Entry* tombstone = nullptr;
    for (;;) {
        Entry* entry = &entries[index];
        if (entry->key == nullptr) {
            if (is<Nil>(entry->value)) {
                // Truly empty — key not present.
                return tombstone != nullptr ? tombstone : entry;
            } else {
                // Tombstone (value == bool true).
                if (tombstone == nullptr)
                    tombstone = entry;
            }
        } else if (entry->key == key) {
            return entry;
        }
        index = (index + 1) % static_cast<uint32_t>(capacity);
    }
}

static void adjustCapacity(Table* table, int capacity) {
    Entry* entries = new Entry[capacity];
    for (int i = 0; i < capacity; ++i) {
        entries[i].key = nullptr;
        entries[i].value = Value{Nil{}};
    }

    // Re-insert all live entries (tombstones are dropped).
    table->count = 0;
    for (int i = 0; i < table->capacity; ++i) {
        Entry* entry = &table->entries[i];
        if (entry->key == nullptr)
            continue;
        Entry* dest = findEntry(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->value = entry->value;
        table->count++;
    }

    delete[] table->entries;
    table->entries = entries;
    table->capacity = capacity;
}

void initTable(Table* table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = nullptr;
}

void freeTable(Table* table) {
    delete[] table->entries;
    initTable(table);
}

bool tableSet(Table* table, ObjString* key, Value value) {
    if (table->count + 1 > table->capacity * TABLE_MAX_LOAD) {
        int capacity = growCapacity(table->capacity);
        adjustCapacity(table, capacity);
    }

    Entry* entry = findEntry(table->entries, table->capacity, key);
    bool isNewKey = (entry->key == nullptr);
    // Only bump count when filling a truly empty bucket (not reusing a tombstone).
    if (isNewKey && is<Nil>(entry->value))
        table->count++;

    entry->key = key;
    entry->value = value;
    return isNewKey;
}

bool tableGet(Table* table, ObjString* key, Value& value) {
    if (table->count == 0)
        return false;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == nullptr)
        return false;

    value = entry->value;
    return true;
}

bool tableDelete(Table* table, ObjString* key) {
    if (table->count == 0)
        return false;

    Entry* entry = findEntry(table->entries, table->capacity, key);
    if (entry->key == nullptr)
        return false;

    // Replace with tombstone: key=null, value=true (bool).
    entry->key = nullptr;
    entry->value = Value{true};
    return true;
}

void tableAddAll(Table* from, Table* to) {
    for (int i = 0; i < from->capacity; ++i) {
        Entry* entry = &from->entries[i];
        if (entry->key != nullptr)
            tableSet(to, entry->key, entry->value);
    }
}

ObjString* tableFindString(Table* table, const char* chars, int length,
                           uint32_t hash) {
    if (table->count == 0)
        return nullptr;

    uint32_t index = hash % static_cast<uint32_t>(table->capacity);
    for (;;) {
        Entry* entry = &table->entries[index];
        if (entry->key == nullptr) {
            // Stop only on a truly empty slot, skip tombstones.
            if (is<Nil>(entry->value))
                return nullptr;
        } else if (static_cast<int>(entry->key->chars.size()) == length &&
                   entry->key->hash == hash &&
                   memcmp(entry->key->chars.c_str(), chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) % static_cast<uint32_t>(table->capacity);
    }
}
