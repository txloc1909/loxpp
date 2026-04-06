#include "simple_allocator.h"

#include "value.h"

#include <algorithm>
#include <cstring>
#include <new>

void* SimpleAllocator::reallocateImpl(void* ptr, size_t oldSize,
                                      size_t newSize) {
    m_bytesAllocated += newSize;
    m_bytesAllocated -= oldSize;

    if (newSize == 0) {
        ::operator delete(ptr);
        return nullptr;
    }

    void* result = ::operator new(newSize);
    if (ptr != nullptr) {
        std::memcpy(result, ptr, std::min(oldSize, newSize));
        ::operator delete(ptr);
    }
    return result;
}

void* SimpleAllocator::reallocate(void* ptr, size_t oldSize, size_t newSize) {
    return reallocateImpl(ptr, oldSize, newSize);
}

SimpleAllocator::SimpleAllocator()
    : m_objects(LoxAllocator<Obj*>{this}), m_strings(this) {}

SimpleAllocator::~SimpleAllocator() {
    for (Obj* obj : m_objects) {
        freeObject(obj);
    }
}

void SimpleAllocator::freeObject(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        // ~ObjString() destroys LoxString chars; LoxAllocator<char>::deallocate
        // fires automatically, decrementing m_bytesAllocated.
        delete static_cast<ObjString*>(obj);
        break;
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

    auto* str = new ObjString{this};
    str->type = ObjType::STRING;
    str->hash = hash;
    str->chars.assign(chars.data(), chars.size());

    auto index = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back(static_cast<Obj*>(str));
    m_strings.set(str, Value{static_cast<Number>(index)});

    return ObjHandle{index, ObjType::STRING};
}

ObjHandle SimpleAllocator::makeString(std::string&& chars) {
    uint32_t hash = hashString(chars);

    ObjString* interned = m_strings.findString(
        chars.data(), static_cast<int>(chars.size()), hash);
    if (interned) {
        Value idxVal;
        m_strings.get(interned, idxVal);
        return ObjHandle{static_cast<uint32_t>(as<Number>(idxVal)),
                         ObjType::STRING};
    }

    auto* str = new ObjString{this};
    str->type = ObjType::STRING;
    str->hash = hash;
    str->chars.assign(chars.data(), chars.size());

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

void SimpleAllocator::markObject(Obj* obj) {
    // Stub: full traversal will be wired up when collect() is implemented.
    obj->marked = true;
}

void SimpleAllocator::collect() {
    // Stub: GC traversal not yet implemented.
}
