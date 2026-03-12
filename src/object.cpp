#include "object.h"
#include "memory.h"

#include <cstdio>

ObjString* makeString(std::vector<std::unique_ptr<Obj>>& objects,
                      std::string_view chars) {
    ObjString* str = allocateObj<ObjString>(objects);
    str->type = ObjType::STRING;
    str->chars = chars;
    return str;
}

void printObject(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        std::printf("%s", asObjString(obj)->chars.c_str());
        break;
    }
}
