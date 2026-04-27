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
        map->map.forEach([&](const MapEntry& e) {
            if (!first)
                result += ", ";
            first = false;
            result += stringify(e.key);
            result += ": ";
            result += stringify(e.value);
        });
        result += "}";
        return result;
    }
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }

// ---------------------------------------------------------------------------
// ObjMap hash table implementation (delegates to CoreHashMap)
// ---------------------------------------------------------------------------

uint32_t MapPolicy::hashOf(const MapEntry& e) { return hashValue(e.key); }

bool ObjMap::mapGet(const Value& key, Value& out) const {
    const MapEntry* e = map.find(
        hashValue(key), [&key](const MapEntry& s) { return s.key == key; });
    if (!e)
        return false;
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
