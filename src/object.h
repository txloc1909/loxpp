#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class ObjType { STRING };

struct Obj {
    ObjType type;
    bool marked = false;
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    std::string chars;
    uint32_t hash{0};
};

struct Table; // Forward declaration to break circular dependency with table.h

ObjString* makeString(std::vector<std::unique_ptr<Obj>>& objects,
                      std::string_view chars, Table* strings = nullptr);

std::string stringifyObj(Obj* obj);
void printObject(Obj* obj);

inline bool isObjType(Obj* obj, ObjType type) { return obj->type == type; }
inline ObjString* asObjString(Obj* obj) { return static_cast<ObjString*>(obj); }
inline const char* asCString(Obj* obj) {
    return static_cast<ObjString*>(obj)->chars.c_str();
}
