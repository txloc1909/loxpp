#include "reflect_api.h"
#include "stdlib_context.h"
#include "../class_objects.h"
#include "../container_objects.h"
#include "../exec_objects.h"
#include "../value.h"
#include "../vm_allocator.h"

#include <string>

// type(x) switches over the same ObjType enum object.cpp's stringifyObj does,
// but groups differently: stringifyObj distinguishes "<fn name>" (CLOSURE,
// BOUND_METHOD) from "<native fn>" (NATIVE, BOUND_NATIVE) since that split is
// visible in printed output. type() collapses across it — closures and
// natives are both "Function", bound methods and bound natives are both
// "BoundMethod" — because the NATIVE/CLOSURE/BOUND_NATIVE split is a C++
// implementation detail, not part of the language's type vocabulary
// (spec/03-types.md has one Function heading and one BoundMethod heading).
static const char* typeNameOf(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        return "String";
    case ObjType::FUNCTION:
    case ObjType::NATIVE:
    case ObjType::CLOSURE:
        return "Function";
    case ObjType::UPVALUE:
        return "Upvalue"; // never a visible Value; kept distinct for
                          // exhaustiveness, not observable from Lox++
    case ObjType::CLASS:
        return "Class";
    case ObjType::INSTANCE:
        return "Instance";
    case ObjType::BOUND_METHOD:
    case ObjType::BOUND_NATIVE:
        return "BoundMethod";
    case ObjType::FILE:
        return "File";
    case ObjType::ITERATOR:
        return "Iterator";
    case ObjType::LIST:
        return "List";
    case ObjType::MAP:
        return "Map";
    case ObjType::ENUM_CTOR:
        return "EnumConstructor";
    case ObjType::ENUM:
        return "Enum";
    }
    return "Unknown";
}

static Value typeNative(int /*argc*/, Value* argv) {
    const Value& v = argv[0];
    const char* name;
    if (is<Nil>(v)) {
        name = "Nil";
    } else if (is<bool>(v)) {
        name = "Boolean";
    } else if (is<Number>(v)) {
        name = "Number";
    } else {
        name = typeNameOf(as<Obj*>(v));
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(name))};
}

// Re-interns the chars of a String value as a Table key. Safe regardless of
// whether the incoming ObjString* happens to already be interned.
static bool asFieldName(const Value& v, ObjString*& out) {
    if (!isString(v)) {
        nativeRuntimeError("Field name must be a string.");
        return false;
    }
    ObjString* s = asObjString(as<Obj*>(v));
    out = getActiveMM()->makeString(
        std::string_view(s->chars.data(), s->chars.size()));
    return true;
}

static Value fieldsNative(int /*argc*/, Value* argv) {
    if (!isInstance(argv[0])) {
        nativeRuntimeError("Expected an instance.");
        return from<Nil>(Nil{});
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(argv[0]));
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    inst->fields.forEach([list](ObjString* key, const Value& /*value*/) {
        list->elements.emplace_back(static_cast<Obj*>(key));
    });
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value methodsNative(int /*argc*/, Value* argv) {
    if (!isClass(argv[0])) {
        nativeRuntimeError("Expected a class.");
        return from<Nil>(Nil{});
    }
    ObjClass* klass = asObjClass(as<Obj*>(argv[0]));
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    klass->methods.forEach([list](ObjString* key, const Value& /*value*/) {
        list->elements.emplace_back(static_cast<Obj*>(key));
    });
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

// getField/hasField/setField are deliberately fields-only: unlike the `.`
// operator's GET_PROPERTY, they never fall back to a bound method when the
// field is absent — fields() already promises "the field table," and a
// method-fallback here would make hasField() disagree with fields()'s own
// enumeration.
static Value getFieldNative(int /*argc*/, Value* argv) {
    if (!isInstance(argv[0])) {
        nativeRuntimeError("Only instances have properties.");
        return from<Nil>(Nil{});
    }
    ObjString* name;
    if (!asFieldName(argv[1], name)) {
        return from<Nil>(Nil{});
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(argv[0]));
    Value value;
    if (inst->fields.get(name, value)) {
        return value;
    }
    return from<Nil>(Nil{});
}

static Value hasFieldNative(int /*argc*/, Value* argv) {
    if (!isInstance(argv[0])) {
        nativeRuntimeError("Only instances have properties.");
        return from<Nil>(Nil{});
    }
    ObjString* name;
    if (!asFieldName(argv[1], name)) {
        return from<Nil>(Nil{});
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(argv[0]));
    Value dummy;
    return from<bool>(inst->fields.get(name, dummy));
}

static Value setFieldNative(int /*argc*/, Value* argv) {
    if (!isInstance(argv[0])) {
        nativeRuntimeError("Only instances have fields.");
        return from<Nil>(Nil{});
    }
    ObjString* name;
    if (!asFieldName(argv[1], name)) {
        return from<Nil>(Nil{});
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(argv[0]));
    inst->fields.set(name, argv[2]);
    return argv[2]; // assignment is an expression, per Property Set semantics
}

// callMethod(inst, name, ...args) mirrors Op::INVOKE's resolution order
// (fields shadow methods), but is capped to natives-only: the VM has no
// bounded re-entrant call path letting a native call back into the bytecode
// interpreter for a closure-backed method (see notes/expressiveness-roadmap.md
// item 1). This restriction is deliberately mirrored on the JVM and CLR
// backends too — neither has this limitation, but must match native's
// behavior for the differential test suite to stay green. Lift all three
// together if that changes.
static Value callMethodNative(int argCount, Value* argv) {
    if (argCount < 2) {
        nativeRuntimeError("Expected at least 2 arguments.");
        return from<Nil>(Nil{});
    }
    if (!isInstance(argv[0])) {
        nativeRuntimeError("Only instances have methods.");
        return from<Nil>(Nil{});
    }
    ObjString* name;
    if (!asFieldName(argv[1], name)) {
        return from<Nil>(Nil{});
    }
    ObjInstance* inst = asObjInstance(as<Obj*>(argv[0]));

    Value callee;
    bool viaField = inst->fields.get(name, callee);
    if (!viaField && !inst->klass->methods.get(name, callee)) {
        std::string msg = "Undefined property '" +
                          std::string(name->chars.data(), name->chars.size()) +
                          "'.";
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }

    int forwardedCount = argCount - 2;
    Value* forwarded = argv + 2;

    if (isNative(callee) || isBoundNative(callee)) {
        ObjBoundNative* bn = isBoundNative(callee)
                                 ? asObjBoundNative(as<Obj*>(callee))
                                 : nullptr;
        ObjNative* native =
            bn != nullptr ? bn->native : asObjNative(as<Obj*>(callee));
        if (native->arity != -1 && forwardedCount != native->arity) {
            std::string msg = "Expected " + std::to_string(native->arity) +
                              " arguments but got " +
                              std::to_string(forwardedCount) + ".";
            nativeRuntimeError(msg.c_str());
            return from<Nil>(Nil{});
        }
        if (bn != nullptr) {
            // The forwarded args' [-1] slot must hold the bound native's own
            // receiver (e.g. the Map/File the method is bound to) — NOT
            // callMethod's own `inst` argument. The "name" slot right before
            // the forwarded args is no longer needed, so reuse it.
            argv[1] = bn->receiver;
        }
        return native->function(forwardedCount, forwarded);
    }
    if (isClosure(callee) || isBoundMethod(callee) || isClass(callee) ||
        isEnumCtor(callee)) {
        // Class and enum-constructor values are callable via `()`, but
        // callMethod supports natives only — same restriction as
        // closures/bound methods, not the "not callable at all" case below.
        nativeRuntimeError(
            "callMethod does not support user-defined methods yet.");
        return from<Nil>(Nil{});
    }
    nativeRuntimeError("callMethod requires a callable value.");
    return from<Nil>(Nil{});
}

void registerReflectAPI(StdlibRegistrar& reg) {
    reg.defineGlobal("type", typeNative, 1);
    reg.defineGlobal("fields", fieldsNative, 1);
    reg.defineGlobal("methods", methodsNative, 1);
    reg.defineGlobal("getField", getFieldNative, 2);
    reg.defineGlobal("setField", setFieldNative, 3);
    reg.defineGlobal("hasField", hasFieldNative, 2);
    reg.defineGlobal("callMethod", callMethodNative, -1);
}
