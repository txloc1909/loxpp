#pragma once
#include "../memory_manager.h"
#include <string>
#include <vector>

struct StdlibContext {
    MemoryManager* mm{nullptr};
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
// Returns a reference to the active context's program arguments. The reference
// is valid only while a VM context is active (i.e. inside a native call).
const std::vector<std::string>& getActiveArgs();
