#include "memory_manager.h"
#include "value.h"

MemoryManager::MemoryManager() : m_strings(VmAllocator<Entry>{this}) {}

MemoryManager::~MemoryManager() { collectAll(); }

void MemoryManager::release(Obj* obj, std::size_t size) {
    bytesAllocated -= size;
    delete obj;
}

ObjString* MemoryManager::makeString(std::string_view sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr)
        return interned;

    auto* s = create<ObjString>(sv, VmAllocator<char>{this});
    m_strings.set(s, Value{Nil{}});
    return s;
}

ObjString* MemoryManager::makeString(std::string&& sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr)
        return interned;

    auto* s = create<ObjString>(std::string_view{sv}, VmAllocator<char>{this});
    m_strings.set(s, Value{Nil{}});
    return s;
}

ObjString* MemoryManager::findString(std::string_view sv) const {
    uint32_t hash = hashString(sv);
    return m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
}

void MemoryManager::collectAll() {
    for (Obj* o : allObjects) {
        delete o;
    }
    allObjects.clear();
    bytesAllocated = 0;
}
