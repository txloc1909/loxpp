#include "reflect_api.h"
#include "stdlib_context.h"
#include "../class_objects.h"
#include "../container_objects.h"
#include "../exec_objects.h"
#include "../runtime.h"
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
    case ObjType::SOCKET:
        return "Socket";
    case ObjType::SERVER:
        return "Server";
    case ObjType::PROCESS:
        return "Process";
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
    case ObjType::ERROR:
        return "Error";
    case ObjType::DEFERRED_CALL:
        // Internal implementation detail, never directly observable from Lox++
        return "DeferredCall";
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
// (fields shadow methods). It calls a native, a bound native (such as a Map
// or File method), or a closure-backed method — a user-defined method or a
// closure/bound method stored in a field — through the Runtime's bounded
// re-entrant call primitive (Runtime::invokeCallableFromNative;
// notes/expressiveness-roadmap.md item 1). A Class or Enum constructor value
// stays unsupported, and any other value is not callable at all.
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

    // Class and enum-constructor values are callable via `()`, but callMethod
    // does not support them; a non-callable value is a distinct error.
    if (isClass(callee) || isEnumCtor(callee)) {
        nativeRuntimeError(
            "callMethod does not support class or enum constructor values.");
        return from<Nil>(Nil{});
    }
    if (!isNative(callee) && !isBoundNative(callee) && !isClosure(callee) &&
        !isBoundMethod(callee)) {
        nativeRuntimeError("callMethod requires a callable value.");
        return from<Nil>(Nil{});
    }

    Runtime* rt = getActiveRuntime();
    if (rt == nullptr) {
        nativeRuntimeError("callMethod requires an active VM.");
        return from<Nil>(Nil{});
    }
    Value result{Nil{}};
    if (isClosure(callee) && !viaField) {
        // A method resolved from the class: bind `this` to the receiver, the
        // same as an ordinary `inst.name(...)` invocation. The receiver goes
        // in slot 0 (below the forwarded args).
        rt->push(argv[0]);
        for (int i = 0; i < forwardedCount; i++) {
            rt->push(forwarded[i]);
        }
        if (!rt->invokeMethodFromNative(asObjClosure(as<Obj*>(callee)),
                                        forwardedCount, &result)) {
            return from<Nil>(Nil{});
        }
        return result;
    }
    // A native, bound native, bound method, or a closure stored in a field:
    // the callee value itself is slot 0, so opCall()'s own dispatch applies.
    // Arrange the callee and forwarded args as opCall() expects (callee at
    // stackTop[-argCount-1]); the primitive runs the call to completion and
    // pops the result, restoring the stack.
    rt->push(callee);
    for (int i = 0; i < forwardedCount; i++) {
        rt->push(forwarded[i]);
    }
    if (!rt->invokeCallableFromNative(forwardedCount, &result)) {
        // A throw inside the called body was resolved outside this native, or
        // was uncaught. callNative() propagates the recorded outcome; the
        // placeholder return below is never pushed.
        return from<Nil>(Nil{});
    }
    return result;
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
