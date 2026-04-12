#include "object.h"
#include "function.h"
#include "table.h"

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
    case ObjType::CLASS: {
        auto* klass = static_cast<ObjClass*>(obj);
        return std::string(klass->name->chars.data(),
                           klass->name->chars.size());
    }
    case ObjType::INSTANCE: {
        auto* inst = static_cast<ObjInstance*>(obj);
        return std::string(inst->klass->name->chars.data(),
                           inst->klass->name->chars.size()) +
               " instance";
    }
    case ObjType::BOUND_METHOD: {
        auto* bm = static_cast<ObjBoundMethod*>(obj);
        auto* fn = bm->method->function;
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::LIST: {
        auto* list = static_cast<ObjList*>(obj);
        std::string result = "[";
        for (size_t i = 0; i < list->elements.size(); i++) {
            if (i > 0)
                result += ", ";
            result += stringify(list->elements[i]);
        }
        result += "]";
        return result;
    }
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }
