#include "simple_allocator.h"

#include "value.h"

#include <algorithm>
#include <cstdlib>
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

// Allocates and constructs a new ObjString whose char buffer is tracked via
// reallocate. Returns the index of the new object in m_objects.
static ObjString* allocString(SimpleAllocator& alloc, const char* data,
                              uint32_t length, uint32_t hash) {
    auto* str = static_cast<ObjString*>(
        alloc.reallocate(nullptr, 0, sizeof(ObjString)));
    ::new (str) ObjString{};
    str->type = ObjType::STRING;
    str->length = length;
    str->hash = hash;
    str->chars = static_cast<char*>(alloc.reallocate(nullptr, 0, length + 1));
    std::memcpy(str->chars, data, length);
    str->chars[length] = '\0';
    return str;
}

SimpleAllocator::~SimpleAllocator() {
    for (Obj* obj : m_objects) {
        freeObject(obj);
    }
}

void SimpleAllocator::freeObject(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING: {
        auto* str = static_cast<ObjString*>(obj);
        reallocateImpl(str->chars, str->length + 1, 0);
        str->~ObjString();
        reallocateImpl(str, sizeof(ObjString), 0);
        break;
    }
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

    ObjString* str = allocString(*this, chars.data(),
                                 static_cast<uint32_t>(chars.size()), hash);
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

    ObjString* str = allocString(*this, chars.data(),
                                 static_cast<uint32_t>(chars.size()), hash);
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
