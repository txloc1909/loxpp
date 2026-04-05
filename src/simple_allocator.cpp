#include "simple_allocator.h"

#include "value.h"

SimpleAllocator::~SimpleAllocator() {
    for (Obj* obj : m_objects) {
        delete obj;
    }
}

ObjHandle SimpleAllocator::makeString(std::string_view chars) {
    uint32_t hash = hashString(chars);

    ObjString* interned = m_strings.findString(
        chars.data(), static_cast<int>(chars.size()), hash);
    if (interned) {
        Value idxVal;
        m_strings.get(interned, idxVal);
        return ObjHandle{static_cast<uint32_t>(as<Number>(idxVal)),
                         ObjType::STRING};
    }

    auto* str = new ObjString();
    str->type = ObjType::STRING;
    str->chars = std::string(chars);
    str->hash = hash;

    auto index = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back(static_cast<Obj*>(str));
    m_strings.set(str, Value{static_cast<Number>(index)});

    return ObjHandle{index, ObjType::STRING};
}

ObjString* SimpleAllocator::findString(std::string_view chars) const {
    uint32_t hash = hashString(chars);
    return m_strings.findString(chars.data(), static_cast<int>(chars.size()),
                                hash);
}

Obj* SimpleAllocator::deref(ObjHandle handle) const {
    return m_objects[handle.index];
}

void SimpleAllocator::collect() {
    // Stub: GC traversal not yet implemented.
}
