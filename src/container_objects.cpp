#include "container_objects.h"
#include "value.h"

// ---------------------------------------------------------------------------
// ObjMap hash table implementation (delegates to CoreHashMap)
// ---------------------------------------------------------------------------

uint32_t MapPolicy::hashOf(const MapEntry& e) { return hashValue(e.key); }

bool ObjMap::mapGet(const Value& key, Value& out) const {
    const MapEntry* e = map.find(
        hashValue(key), [&key](const MapEntry& s) { return s.key == key; });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSet(const Value& key, const Value& value) {
    // map.set may grow (re-allocate via VmAllocator, which may trigger GC).
    // Caller must ensure this ObjMap is rooted before calling mapSet.
    return map.set(MapEntry{key, value, MapSlot::OCCUPIED});
}

bool ObjMap::mapDel(const Value& key) {
    return map.remove(hashValue(key),
                      [&key](const MapEntry& s) { return s.key == key; });
}
