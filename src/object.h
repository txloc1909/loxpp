#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class ObjType { STRING };

struct Obj {
    ObjType type;
    bool    marked = false;
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    std::string chars;
};

ObjString* makeString(std::vector<std::unique_ptr<Obj>>& objects,
                      std::string_view chars);

void printObject(Obj* obj);

inline bool        isObjType(Obj* obj, ObjType type) { return obj->type == type; }
inline ObjString*  asObjString(Obj* obj) { return static_cast<ObjString*>(obj); }
inline const char* asCString(Obj* obj)   { return static_cast<ObjString*>(obj)->chars.c_str(); }
