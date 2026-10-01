#include "map_api.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../runtime.h"
#include "../vm_allocator.h"
#include "../value.h"

// Module-local class pointer; used indirectly (ObjMap stores it at construction
// via registerMapAPI).
static ObjClass* s_mapClass = nullptr;

// args[-1] = the ObjMap receiver
static ObjMap* checkMap(Value* args) { return asObjMap(as<Obj*>(args[-1])); }

// Map.has/Map.del keep the v1 fatal, single-message disposition for an invalid
// key (unlike an index read/write, a map literal, or `in`, which raise a
// catchable kind-split error). A key the VM rejects is fatal here.
static const char* kInvalidKeyMessage =
    "Map keys must be Bool, Number, Nil, String, or an object with __hash__ "
    "and __eq__. NaN is not allowed.";

static Value mapHasNative(int /*argc*/, Value* args) {
    Runtime* rt = getActiveRuntime();
    if (rt == nullptr || !rt->mapKeyValid(args[0])) {
        nativeRuntimeError(kInvalidKeyMessage);
        return from<Nil>(Nil{});
    }
    ObjMap* map = checkMap(args);
    bool found = false;
    if (!rt->mapHasFromNative(map, args[0], &found)) {
        // A throw inside the key's __hash__/__eq__. callNative() propagates
        // the recorded outcome; this placeholder is not pushed.
        return from<Nil>(Nil{});
    }
    return from<bool>(found);
}

static Value mapDelNative(int /*argc*/, Value* args) {
    Runtime* rt = getActiveRuntime();
    if (rt == nullptr || !rt->mapKeyValid(args[0])) {
        nativeRuntimeError(kInvalidKeyMessage);
        return from<Nil>(Nil{});
    }
    ObjMap* map = checkMap(args);
    if (!rt->mapDelFromNative(map, args[0])) {
        return from<Nil>(Nil{});
    }
    return from<Nil>(Nil{});
}

static Value mapKeysNative(int /*argc*/, Value* args) {
    ObjMap* map = checkMap(args);
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    // key may be an ObjString; it stays alive via the map (map is rooted
    // as the receiver on the VM stack). push_back may GC — list is rooted.
    map->map.forEach(
        [list](const MapEntry& e) { list->elements.push_back(e.key); });
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value mapValuesNative(int /*argc*/, Value* args) {
    ObjMap* map = checkMap(args);
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    map->map.forEach(
        [list](const MapEntry& e) { list->elements.push_back(e.value); });
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value mapEntriesNative(int /*argc*/, Value* args) {
    ObjMap* map = checkMap(args);
    MemoryManager* mm = getActiveMM();
    ObjList* result = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(result);
    map->map.forEach([result, mm](const MapEntry& e) {
        // Build [key, value] pair list
        ObjList* pair = mm->create<ObjList>(VmAllocator<Value>{mm});
        mm->pushTempRoot(pair);
        pair->elements.push_back(e.key);
        pair->elements.push_back(e.value);
        // Keep pair rooted until after push_back: result->elements.push_back
        // may trigger GC, and pair would be unreachable if popped too early.
        result->elements.push_back(Value{static_cast<Obj*>(pair)});
        mm->popTempRoot(); // pair — now safe, stored in result
    });
    mm->popTempRoot(); // result
    return Value{static_cast<Obj*>(result)};
}

ObjClass* registerMapAPI(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("Map");
    reg.mm().pushTempRoot(klass);

    reg.addMethod(klass, "has", mapHasNative, 1);
    reg.addMethod(klass, "del", mapDelNative, 1);
    reg.addMethod(klass, "keys", mapKeysNative, 0);
    reg.addMethod(klass, "values", mapValuesNative, 0);
    reg.addMethod(klass, "entries", mapEntriesNative, 0);

    s_mapClass = klass;

    reg.mm().popTempRoot(); // klass
    return klass;
}
