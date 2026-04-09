#include "object.h"
#include "function.h"

#include <cstdio>
#include <string>

std::string stringifyObj(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING: {
        const auto& s = asObjString(obj)->chars;
        return {s.data(), s.size()};
    }
    case ObjType::FUNCTION: {
        auto* fn = asObjFunction(obj);
        if (fn->name == nullptr)
            return "<script>";
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::NATIVE:
        return "<native fn>";
    case ObjType::CLOSURE: {
        auto* fn = static_cast<ObjClosure*>(obj)->function;
        if (fn->name == nullptr)
            return "<script>";
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::UPVALUE:
        return "<upvalue>";
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }
