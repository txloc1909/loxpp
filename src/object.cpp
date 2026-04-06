#include "object.h"

#include <cstdio>
#include <string>

std::string stringifyObj(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING: {
        const auto& s = asObjString(obj)->chars;
        return {s.data(), s.size()};
    }
    }
    return "<obj>";
}

void printObject(Obj* obj) {
    // TODO: all callers print to stdout; consider replacing stringifyObj with a
    // direct write (e.g. fputs(chars.data(), stdout)) to avoid the temporary
    // std::string allocation.
    std::printf("%s", stringifyObj(obj).c_str());
}
