#include "object.h"
#include "function.h"
#include "table.h"

#include <algorithm>
#include <cstdio>
#include <string>

std::string stringifyObj(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING: {
        const auto& s = asObjString(obj)->chars;
        return {s.data(), s.size()};
    }
    case ObjType::FUNCTION: {
        auto* fn = asObjFunction(obj);
        if (fn->name == nullptr)
            return "<script>";
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::NATIVE:
        return "<native fn>";
    case ObjType::CLOSURE: {
        auto* fn = static_cast<ObjClosure*>(obj)->function;
        if (fn->name == nullptr)
            return "<script>";
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::UPVALUE:
        return "<upvalue>";
    case ObjType::CLASS: {
        auto* klass = static_cast<ObjClass*>(obj);
        return std::string(klass->name->chars.data(),
                           klass->name->chars.size());
    }
    case ObjType::INSTANCE: {
        auto* inst = static_cast<ObjInstance*>(obj);
        return std::string(inst->klass->name->chars.data(),
                           inst->klass->name->chars.size()) +
               " instance";
    }
    case ObjType::BOUND_METHOD: {
        auto* bm = static_cast<ObjBoundMethod*>(obj);
        auto* fn = bm->method->function;
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::FILE:
        return "<file>";
    case ObjType::ITERATOR:
        return "<iterator>";
    case ObjType::LIST: {
        auto* list = static_cast<ObjList*>(obj);
        std::string result = "[";
        for (size_t i = 0; i < list->elements.size(); i++) {
            if (i > 0)
                result += ", ";
            result += stringify(list->elements[i]);
        }
        result += "]";
        return result;
    }
    case ObjType::MAP: {
        auto* map = static_cast<ObjMap*>(obj);
        std::string result = "{";
        bool first = true;
        for (const auto& e : map->buckets) {
            if (e.state != MapSlot::OCCUPIED)
                continue;
            if (!first)
                result += ", ";
            first = false;
            result += stringify(e.key);
            result += ": ";
            result += stringify(e.value);
        }
        result += "}";
        return result;
    }
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }

// ---------------------------------------------------------------------------
// ObjMap hash table implementation
// ---------------------------------------------------------------------------

static MapEntry* mapFindBucket(MapEntry* buckets, int capacity,
                               const Value& key) {
    uint32_t idx = hashValue(key) % static_cast<uint32_t>(capacity);
    MapEntry* tombstone = nullptr;
    for (;;) {
        MapEntry& entry = buckets[idx];
        if (entry.state == MapSlot::EMPTY)
            return tombstone != nullptr ? tombstone : &entry;
        if (entry.state == MapSlot::TOMBSTONE) {
            if (!tombstone)
                tombstone = &entry;
        } else if (entry.key == key) {
            return &entry;
        }
        idx = (idx + 1) % static_cast<uint32_t>(capacity);
    }
}

bool ObjMap::mapGet(const Value& key, Value& out) const {
    if (count == 0)
        return false;
    int cap = static_cast<int>(buckets.size());
    MapEntry* entry =
        mapFindBucket(const_cast<MapEntry*>(buckets.data()), cap, key);
    if (entry->state != MapSlot::OCCUPIED)
        return false;
    out = entry->value;
    return true;
}

bool ObjMap::mapSet(const Value& key, const Value& value) {
    int cap = static_cast<int>(buckets.size());
    // Grow when the load factor (live + tombstone) would exceed MAX_LOAD.
    if (bucketUsed + 1 > static_cast<int>(cap * MAX_LOAD)) {
        int newCap = std::max(8, cap * 2);
        // Construct new storage using the same allocator (may trigger GC;
        // caller must ensure this ObjMap is rooted before calling mapSet).
        VmVector<MapEntry> fresh(buckets.get_allocator());
        fresh.resize(newCap);
        int saved = 0;
        for (auto& e : buckets) {
            if (e.state != MapSlot::OCCUPIED)
                continue;
            MapEntry* dest = mapFindBucket(fresh.data(), newCap, e.key);
            *dest = e;
            ++saved;
        }
        bucketUsed = saved; // tombstones are gone after rehash
        buckets = std::move(fresh);
        cap = newCap;
    }

    MapEntry* entry = mapFindBucket(buckets.data(), cap, key);
    bool isNew = (entry->state != MapSlot::OCCUPIED);
    if (isNew) {
        ++count;
        if (entry->state == MapSlot::EMPTY)
            ++bucketUsed; // reusing a tombstone slot doesn't grow bucketUsed
    }
    entry->key = key;
    entry->value = value;
    entry->state = MapSlot::OCCUPIED;
    return isNew;
}

bool ObjMap::mapDel(const Value& key) {
    if (count == 0)
        return false;
    int cap = static_cast<int>(buckets.size());
    MapEntry* entry = mapFindBucket(buckets.data(), cap, key);
    if (entry->state != MapSlot::OCCUPIED)
        return false;
    entry->state = MapSlot::TOMBSTONE;
    entry->key = Value{Nil{}};
    entry->value = Value{Nil{}};
    --count;
    return true;
}
