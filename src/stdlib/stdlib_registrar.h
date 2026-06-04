#pragma once
#include "../objects.h"
#include "../table.h"
#include "../memory_manager.h"

class StdlibRegistrar {
  public:
    StdlibRegistrar(MemoryManager& mm, Table& globals) : m_mm(mm), m_globals(globals) {}

    void defineGlobal(const char* name, NativeFn fn, int arity);
    void defineGlobalValue(const char* name, Value val);
    ObjClass* makeClass(const char* name);
    void addMethod(ObjClass* klass, const char* name, NativeFn fn, int arity);
    ObjInstance* makeInstance(ObjClass* klass);
    void addField(ObjInstance* inst, const char* name, Value val);
    void addNativeField(ObjInstance* inst, const char* name, NativeFn fn, int arity);
    MemoryManager& mm() { return m_mm; }

  private:
    MemoryManager& m_mm;
    Table& m_globals;
};
