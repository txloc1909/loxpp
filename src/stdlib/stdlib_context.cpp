#include "stdlib_context.h"
#include "../exec_objects.h"

static thread_local StdlibContext* t_activeCtx = nullptr;

void setActiveContext(StdlibContext* ctx) { t_activeCtx = ctx; }
MemoryManager* getActiveMM() { return t_activeCtx ? t_activeCtx->mm : nullptr; }

// implements the declaration in exec_objects.h
void nativeRuntimeError(const char* msg) {
    if (t_activeCtx) t_activeCtx->reportError(msg);
}
