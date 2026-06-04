#include "math_module.h"
#include "../math.h"

void registerMath(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("Math");
    reg.mm().pushTempRoot(klass);
    ObjInstance* inst = reg.makeInstance(klass);
    reg.mm().pushTempRoot(inst);
    for (std::size_t i = 0; i < kMathFunctionCount; ++i)
        reg.addNativeField(inst, kMathFunctions[i].name, kMathFunctions[i].fn, kMathFunctions[i].arity);
    for (std::size_t i = 0; i < kMathConstantCount; ++i)
        reg.addField(inst, kMathConstants[i].name, from<Number>(kMathConstants[i].value));
    reg.defineGlobalValue("math", Value{static_cast<Obj*>(inst)});
    reg.mm().popTempRoot(); // inst
    reg.mm().popTempRoot(); // klass
}
