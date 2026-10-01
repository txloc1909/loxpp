#pragma once
#include "../memory_manager.h"
#include <string>
#include <vector>

class Runtime;

struct StdlibContext {
    MemoryManager* mm{nullptr};
    // The Runtime that owns this context. Set once by Runtime's constructor so
    // a native can reach the re-entrant call primitive (callMethod). Natives
    // otherwise touch the VM only through this context.
    Runtime* rt{nullptr};
    std::vector<std::string> args; // command-line args after the script name
    bool nativeError{false};
    std::string nativeErrorMsg;
    void reportError(const char* msg) {
        nativeError = true;
        nativeErrorMsg = msg;
    }
    void clearError() { nativeError = false; }
};

void setActiveContext(StdlibContext* ctx);
MemoryManager* getActiveMM(); // returns active context's mm pointer
// Returns the active Runtime, or nullptr outside a native call. Valid only
// while a VM context is active (i.e. inside a native call).
Runtime* getActiveRuntime();
// Returns a reference to the active context's program arguments. The reference
// is valid only while a VM context is active (i.e. inside a native call).
const std::vector<std::string>& getActiveArgs();
