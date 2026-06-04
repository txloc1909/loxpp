#include "stdlib_registrar.h"
#include "../vm_allocator.h"

void StdlibRegistrar::defineGlobal(const char* name, NativeFn fn, int arity) {
    ObjNative* native = m_mm.create<ObjNative>(fn, arity);
    m_mm.pushTempRoot(native);
    ObjString* key = m_mm.makeString(name);
    m_mm.pushTempRoot(key);
    m_globals.set(key, Value{static_cast<Obj*>(native)});
    m_mm.popTempRoot(); // key
    m_mm.popTempRoot(); // native
}

void StdlibRegistrar::defineGlobalValue(const char* name, Value val) {
    if (is<Obj*>(val)) {
        m_mm.pushTempRoot(as<Obj*>(val));
    }
    ObjString* key = m_mm.makeString(name);
    m_mm.pushTempRoot(key);
    m_globals.set(key, val);
    m_mm.popTempRoot(); // key
    if (is<Obj*>(val)) {
        m_mm.popTempRoot(); // val
    }
}

ObjClass* StdlibRegistrar::makeClass(const char* name) {
    ObjString* className = m_mm.makeString(name);
    m_mm.pushTempRoot(className);
    ObjClass* klass = m_mm.create<ObjClass>(className, VmAllocator<Entry>{&m_mm});
    m_mm.popTempRoot(); // className
    return klass;
}

void StdlibRegistrar::addMethod(ObjClass* klass, const char* name, NativeFn fn, int arity) {
    m_mm.pushTempRoot(klass);
    ObjNative* native = m_mm.create<ObjNative>(fn, arity);
    m_mm.pushTempRoot(native);
    ObjString* key = m_mm.makeString(name);
    m_mm.pushTempRoot(key);
    klass->methods.set(key, Value{static_cast<Obj*>(native)});
    m_mm.popTempRoot(); // key
    m_mm.popTempRoot(); // native
    m_mm.popTempRoot(); // klass
}

ObjInstance* StdlibRegistrar::makeInstance(ObjClass* klass) {
    m_mm.pushTempRoot(klass);
    ObjInstance* inst = m_mm.create<ObjInstance>(klass, VmAllocator<Entry>{&m_mm});
    m_mm.popTempRoot(); // klass
    return inst;
}

void StdlibRegistrar::addField(ObjInstance* inst, const char* name, Value val) {
    m_mm.pushTempRoot(inst);
    if (is<Obj*>(val)) {
        m_mm.pushTempRoot(as<Obj*>(val));
    }
    ObjString* key = m_mm.makeString(name);
    m_mm.pushTempRoot(key);
    inst->fields.set(key, val);
    m_mm.popTempRoot(); // key
    if (is<Obj*>(val)) {
        m_mm.popTempRoot(); // val
    }
    m_mm.popTempRoot(); // inst
}

void StdlibRegistrar::addNativeField(ObjInstance* inst, const char* name, NativeFn fn, int arity) {
    m_mm.pushTempRoot(inst);
    ObjNative* native = m_mm.create<ObjNative>(fn, arity);
    m_mm.pushTempRoot(native);
    ObjString* key = m_mm.makeString(name);
    m_mm.pushTempRoot(key);
    inst->fields.set(key, Value{static_cast<Obj*>(native)});
    m_mm.popTempRoot(); // key
    m_mm.popTempRoot(); // native
    m_mm.popTempRoot(); // inst
}
