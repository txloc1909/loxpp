#include "container_objects.h"
#include "value.h"

// ---------------------------------------------------------------------------
// ObjMap hash table implementation (delegates to CoreHashMap)
// ---------------------------------------------------------------------------

uint32_t MapPolicy::hashOf(const MapEntry& e) { return e.hash; }

// Scalar/string convenience path: identity equality, hash from hashValue().
// An Instance key must go through mapGetHashed/mapSetHashed/mapDelHashed so
// its class's __hash__/__eq__ run.
bool ObjMap::mapGet(const Value& key, Value& out) const {
    uint32_t hash = hashValue(key);
    const MapEntry* e =
        map.find(hash, [&key](const MapEntry& s) { return s.key == key; });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSet(const Value& key, const Value& value) {
    // map.set may grow (re-allocate via VmAllocator, which may trigger GC).
    // Caller must ensure this ObjMap is rooted before calling mapSet.
    bool inserted =
        map.set(MapEntry{key, value, hashValue(key), MapSlot::OCCUPIED});
    // Count only a new key: an overwrite leaves iteration valid, while a
    // paired erase plus insert must still trip the iterator check.
    if (inserted) {
        ++version;
    }
    return inserted;
}

bool ObjMap::mapDel(const Value& key) {
    bool removed = map.remove(
        hashValue(key), [&key](const MapEntry& s) { return s.key == key; });
    // Count only a real erase: a miss leaves iteration valid.
    if (removed) {
        ++version;
    }
    return removed;
}

bool ObjMap::mapGetHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                          Value& out) const {
    const MapEntry* e =
        map.find(hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSetHashed(const Value& key, const Value& value, uint32_t hash,
                          const KeyEq& eq) {
    bool inserted = map.set(MapEntry{key, value, hash, MapSlot::OCCUPIED},
                            [&](const MapEntry& s) { return eq(s.key, key); });
    if (inserted) {
        ++version;
    }
    return inserted;
}

bool ObjMap::mapDelHashed(const Value& key, uint32_t hash, const KeyEq& eq) {
    bool removed =
        map.remove(hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (removed) {
        ++version;
    }
    return removed;
}
