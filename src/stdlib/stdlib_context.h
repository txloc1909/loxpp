#pragma once
#include "../memory_manager.h"
#include <string>

struct StdlibContext {
    MemoryManager* mm{nullptr};
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
