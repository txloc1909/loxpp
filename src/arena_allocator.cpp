#include "arena_allocator.h"

#include "value.h"

ArenaAllocator::~ArenaAllocator() {
    for (ObjString* p : m_objects)
        p->~ObjString();
    // Block memory is freed by the unique_ptr destructors.
}

ObjString* ArenaAllocator::allocSlot() {
    if (m_blocks.empty() || m_blocks.back()->count >= BLOCK_CAPACITY)
        m_blocks.push_back(std::make_unique<Block>());

    auto& block = *m_blocks.back();
    auto* slot = reinterpret_cast<ObjString*>(block.data +
                                              block.count * sizeof(ObjString));
    ++block.count;
    return slot;
}

ObjHandle ArenaAllocator::internHandle(ObjString* str) {
    auto index = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back(str);
    m_strings.set(str, Value{static_cast<Number>(index)});
    return ObjHandle{index, ObjType::STRING};
}

ObjHandle ArenaAllocator::makeString(std::string_view chars) {
    uint32_t hash = hashString(chars);

    ObjString* interned = m_strings.findString(
        chars.data(), static_cast<int>(chars.size()), hash);
    if (interned) {
        Value idxVal;
        m_strings.get(interned, idxVal);
        return ObjHandle{static_cast<uint32_t>(as<Number>(idxVal)),
                         ObjType::STRING};
    }

    ObjString* str = new (allocSlot()) ObjString();
    str->type = ObjType::STRING;
    str->chars = std::string(chars);
    str->hash = hash;
    return internHandle(str);
}

ObjHandle ArenaAllocator::makeString(std::string&& chars) {
    uint32_t hash = hashString(chars);

    ObjString* interned = m_strings.findString(
        chars.data(), static_cast<int>(chars.size()), hash);
    if (interned) {
        Value idxVal;
        m_strings.get(interned, idxVal);
        return ObjHandle{static_cast<uint32_t>(as<Number>(idxVal)),
                         ObjType::STRING};
    }

    ObjString* str = new (allocSlot()) ObjString();
    str->type = ObjType::STRING;
    str->chars = std::move(chars);
    str->hash = hash;
    return internHandle(str);
}

ObjString* ArenaAllocator::findString(std::string_view chars) const {
    uint32_t hash = hashString(chars);
    return m_strings.findString(chars.data(), static_cast<int>(chars.size()),
                                hash);
}

Obj* ArenaAllocator::deref(ObjHandle handle) const {
    return m_objects[handle.index];
}

void ArenaAllocator::collect() {
    // Stub: GC traversal not yet implemented.
}
