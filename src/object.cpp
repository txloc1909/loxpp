#include "object.h"

#include <cstdio>
#include <string>

#include "memory.h"
#include "table.h"
#include "value.h"

ObjString* makeString(std::vector<std::unique_ptr<Obj>>& objects,
                      std::string_view chars, Table* strings) {
    uint32_t hash = hashString(chars);
    if (strings) {
        ObjString* interned = tableFindString(
            strings, chars.data(), static_cast<int>(chars.size()), hash);
        if (interned)
            return interned;
    }

    ObjString* str = allocateObj<ObjString>(objects);
    str->type = ObjType::STRING;
    str->chars = chars;
    str->hash = hash;

    if (strings)
        tableSet(strings, str, Value{Nil{}});

    return str;
}

std::string stringifyObj(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        return asObjString(obj)->chars;
        // Future types (ObjFunction, ObjClass, etc.) go here.
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }
