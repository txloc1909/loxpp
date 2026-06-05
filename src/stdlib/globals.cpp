#include "globals.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../object.h"
#include "../value.h"

#include <ctime>
#include <iostream>
#include <string>

static Value clockNative(int /*argCount*/, Value* /*args*/) {
    return from<Number>(static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
}

// Read one line from stdin. Returns nil on EOF, otherwise an ObjString.
static Value inputNative(int /*argCount*/, Value* /*args*/) {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return from<Nil>(Nil{});
    }
    ObjString* s = getActiveMM()->makeString(line);
    return Value{static_cast<Obj*>(s)};
}

static Value strNative(int /*argCount*/, Value* args) {
    std::string s = stringify(args[0]);
    ObjString* obj = getActiveMM()->makeString(s);
    return Value{static_cast<Obj*>(obj)};
}

static Value lenNative(int /*argCount*/, Value* args) {
    if (isList(args[0])) {
        auto* list = asObjList(as<Obj*>(args[0]));
        return from<Number>(static_cast<double>(list->elements.size()));
    }
    if (isString(args[0])) {
        auto* s = asObjString(as<Obj*>(args[0]));
        return from<Number>(static_cast<double>(s->chars.size()));
    }
    if (isMap(args[0])) {
        auto* map = asObjMap(as<Obj*>(args[0]));
        return from<Number>(static_cast<double>(map->map.count()));
    }
    nativeRuntimeError("len() argument must be a list, string, or map.");
    return from<Nil>(Nil{});
}

void registerGlobals(StdlibRegistrar& reg) {
    reg.defineGlobal("clock", clockNative, 0);
    reg.defineGlobal("input", inputNative, 0);
    reg.defineGlobal("str", strNative, 1);
    reg.defineGlobal("len", lenNative, 1);
}
