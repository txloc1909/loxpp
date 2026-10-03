#include "globals.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../exec_objects.h"
#include "../object.h"
#include "../value.h"

#include <cmath>
#include <ctime>
#include <iostream>
#include <string>

static Value clockNative(int /*argCount*/, Value* /*args*/) {
    return from<Number>(static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
}

// Byte value of a one-byte string. The argument is restricted to a single
// byte so ord() and chr() stay exact inverses over 0..255.
static Value ordNative(int /*argCount*/, Value* args) {
    Value v = args[0];
    if (!isString(v)) {
        nativeRuntimeError("ord() argument must be a string.");
        return from<Nil>(Nil{});
    }
    const LoxString& chars = asObjString(as<Obj*>(v))->chars;
    if (chars.size() != 1) {
        nativeRuntimeError("ord() argument must be a one-character string.");
        return from<Nil>(Nil{});
    }
    return from<Number>(
        static_cast<double>(static_cast<unsigned char>(chars[0])));
}

// One-byte string for a byte value. Requires an integral Number in 0..255 so
// it is the inverse of ord().
static Value chrNative(int /*argCount*/, Value* args) {
    Value v = args[0];
    if (!is<Number>(v)) {
        nativeRuntimeError("chr() argument must be a number.");
        return from<Nil>(Nil{});
    }
    Number n = as<Number>(v);
    if (std::floor(n) != n || n < 0 || n > 255) {
        nativeRuntimeError("chr() argument must be an integer from 0 to 255.");
        return from<Nil>(Nil{});
    }
    auto byte =
        static_cast<char>(static_cast<unsigned char>(static_cast<int>(n)));
    ObjString* s = getActiveMM()->makeString(std::string_view(&byte, 1));
    return Value{static_cast<Obj*>(s)};
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
    reg.defineGlobal("ord", ordNative, 1);
    reg.defineGlobal("chr", chrNative, 1);
}
