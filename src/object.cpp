#include "object.h"
#include "objects.h"
#include "table.h"

#include <algorithm>
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
        if (fn->name == nullptr) {
            return "<script>";
        }
        return "<fn " +
               std::string(fn->name->chars.data(), fn->name->chars.size()) +
               ">";
    }
    case ObjType::NATIVE:
        return "<native fn>";
    case ObjType::CLOSURE: {
        auto* fn = static_cast<ObjClosure*>(obj)->function;
        if (fn->name == nullptr) {
            return "<script>";
        }
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
    case ObjType::FILE:
        return "<file>";
    case ObjType::ITERATOR:
        return "<iterator>";
    case ObjType::LIST: {
        auto* list = static_cast<ObjList*>(obj);
        std::string result = "[";
        for (size_t i = 0; i < list->elements.size(); i++) {
            if (i > 0) {
                result += ", ";
            }
            result += stringify(list->elements[i]);
        }
        result += "]";
        return result;
    }
    case ObjType::MAP: {
        auto* map = static_cast<ObjMap*>(obj);
        std::string result = "{";
        bool first = true;
        map->map.forEach([&](const MapEntry& e) {
            if (!first) {
                result += ", ";
            }
            first = false;
            result += stringify(e.key);
            result += ": ";
            result += stringify(e.value);
        });
        result += "}";
        return result;
    }
    case ObjType::ENUM_CTOR: {
        auto* ctor = asObjEnumCtor(obj);
        return "<ctor " +
               std::string(ctor->enumName->chars.data(),
                           ctor->enumName->chars.size()) +
               "::" +
               std::string(ctor->ctorName->chars.data(),
                           ctor->ctorName->chars.size()) +
               ">";
    }
    case ObjType::ENUM: {
        auto* e = asObjEnum(obj);
        std::string s(e->ctor->enumName->chars.data(),
                      e->ctor->enumName->chars.size());
        s += "::";
        s += std::string(e->ctor->ctorName->chars.data(),
                         e->ctor->ctorName->chars.size());
        if (!e->fields.empty()) {
            s += "(";
            bool first = true;
            for (auto& f : e->fields) {
                if (!first) {
                    s += ", ";
                }
                first = false;
                s += stringify(f);
            }
            s += ")";
        }
        return s;
    }
    }
    return "<obj>";
}

void printObject(Obj* obj) { std::printf("%s", stringifyObj(obj).c_str()); }
