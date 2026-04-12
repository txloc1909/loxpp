#include "memory_manager.h"
#include "compiler.h"
#include "function.h"
#include "native.h"
#include "table.h"
#include "value.h"

#include <cstdio>

#ifdef LOXPP_DEBUG_LOG_GC
static const char* objTypeName(ObjType type) {
    switch (type) {
    case ObjType::STRING:
        return "string";
    case ObjType::FUNCTION:
        return "function";
    case ObjType::NATIVE:
        return "native";
    case ObjType::UPVALUE:
        return "upvalue";
    case ObjType::CLOSURE:
        return "closure";
    case ObjType::CLASS:
        return "class";
    case ObjType::INSTANCE:
        return "instance";
    }
    return "?";
}
#endif

MemoryManager::MemoryManager() : m_strings(VmAllocator<Entry>{this}) {}

MemoryManager::~MemoryManager() { collectAll(); }

void* MemoryManager::rawAlloc(std::size_t bytes) {
    bytesAllocated += bytes;
#ifdef LOXPP_STRESS_GC
    collectGarbage(); // fire on every allocation to surface rooting bugs
#else
    if (bytesAllocated > m_nextGC)
        collectGarbage();
#endif
    return ::operator new(bytes);
}

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

void MemoryManager::setMarkRootsCallback(std::function<void()> cb) {
    m_markRoots = std::move(cb);
}

void MemoryManager::setCurrentCompiler(Compiler* c) { m_currentCompiler = c; }

void MemoryManager::markObject(Obj* obj) {
    if (obj == nullptr || obj->marked)
        return;
    obj->marked = true;
    m_grayStack.push_back(obj);
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] mark   %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
}

void MemoryManager::markValue(const Value& v) {
    if (isObj(v))
        markObject(as<Obj*>(v));
}

void MemoryManager::traceReferences() {
    while (!m_grayStack.empty()) {
        Obj* obj = m_grayStack.back();
        m_grayStack.pop_back();
        traceObject(obj);
    }
}

void MemoryManager::traceObject(Obj* obj) {
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] trace  %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
    switch (obj->type) {
    case ObjType::STRING:
        break;
    case ObjType::NATIVE:
        break;
    case ObjType::UPVALUE:
        markValue(static_cast<ObjUpvalue*>(obj)->closed);
        break;
    case ObjType::FUNCTION: {
        auto* fn = static_cast<ObjFunction*>(obj);
        markObject(fn->name);
        const auto& consts = fn->chunk.constants();
        for (uint8_t i = 0; i < consts.size(); i++)
            markValue(consts.at(i));
        break;
    }
    case ObjType::CLOSURE: {
        auto* cl = static_cast<ObjClosure*>(obj);
        markObject(cl->function);
        for (auto* uv : cl->upvalues)
            markObject(uv);
        break;
    }
    case ObjType::CLASS: {
        auto* klass = static_cast<ObjClass*>(obj);
        markObject(klass->name);
        klass->methods.forEach([this](ObjString* k, Value v) {
            markObject(k);
            markValue(v);
        });
        break;
    }
    case ObjType::INSTANCE: {
        auto* inst = static_cast<ObjInstance*>(obj);
        markObject(inst->klass);
        inst->fields.forEach([this](ObjString* k, Value v) {
            markObject(k);
            markValue(v);
        });
        break;
    }
    }
}

void MemoryManager::removeWhiteStrings() { m_strings.removeUnmarkedKeys(); }

void MemoryManager::sweep() {
    auto it = allObjects.begin();
    while (it != allObjects.end()) {
        if ((*it)->marked) {
            (*it)->marked = false;
            ++it;
        } else {
#ifdef LOXPP_DEBUG_LOG_GC
            // Use type-only log: stringifyObj dereferences fn->name which may
            // already be freed if the name ObjString appeared earlier in
            // allObjects.
            fprintf(stderr, "[GC] free   %p (%s)\n", static_cast<void*>(*it),
                    objTypeName((*it)->type));
#endif
            delete *it;
            it = allObjects.erase(it);
        }
    }
    m_nextGC = bytesAllocated * GC_HEAP_GROW_FACTOR;
}

void MemoryManager::collectGarbage() {
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] begin -- %zu bytes allocated\n", bytesAllocated);
#endif
    if (m_markRoots)
        m_markRoots();
    if (m_currentCompiler)
        m_currentCompiler->markRoots(*this);
    traceReferences();
    removeWhiteStrings();
    sweep();
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] end   -- %zu bytes allocated, next threshold %zu\n",
            bytesAllocated, m_nextGC);
#endif
}
