#include "error_api.h"
#include "stdlib_registrar.h"
#include "../class_objects.h"

ObjClass* registerErrorAPI(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("Error");
    reg.mm().pushTempRoot(klass);
    // Error has no methods — just read-only message and kind fields.
    reg.mm().popTempRoot(); // klass
    return klass;
}
