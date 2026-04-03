#include "object.h"
#include "memory.h"

#include <cstdio>
#include <string>

ObjString* makeString(std::vector<std::unique_ptr<Obj>>& objects,
                      std::string_view chars) {
    ObjString* str = allocateObj<ObjString>(objects);
    str->type = ObjType::STRING;
    str->chars = chars;
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

void printObject(Obj* obj) {
    std::printf("%s", stringifyObj(obj).c_str());
}
