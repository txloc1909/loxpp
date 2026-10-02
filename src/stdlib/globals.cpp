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

void registerGlobals(StdlibRegistrar& reg) {
    reg.defineGlobal("clock", clockNative, 0);
    reg.defineGlobal("input", inputNative, 0);
}
