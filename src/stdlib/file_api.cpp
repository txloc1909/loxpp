#include "file_api.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

// Module-local class pointer used by openNative to stamp new ObjFile instances.
static ObjClass* s_fileClass = nullptr;

// Validate that args[-1] is an open ObjFile. Returns nullptr and sets a
// runtime error if the file is closed.
static ObjFile* checkFile(Value* args, const char* method) {
    ObjFile* file = asObjFile(as<Obj*>(args[-1]));
    if (!file->handle) {
        std::string msg = "Cannot call '";
        msg += method;
        msg += "' on a closed file.";
        nativeRuntimeError(msg.c_str());
        return nullptr;
    }
    return file;
}

static Value fileReadNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "read");
    if (!file) {
        return from<Nil>(Nil{});
    }
    if (!file->readable) {
        nativeRuntimeError("File is not open for reading.");
        return from<Nil>(Nil{});
    }
    std::string buf;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), file->handle)) {
        buf += chunk;
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value fileReadlineNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "readline");
    if (!file) {
        return from<Nil>(Nil{});
    }
    if (!file->readable) {
        nativeRuntimeError("File is not open for reading.");
        return from<Nil>(Nil{});
    }
    std::string line;
    char chunk[4096];
    bool got = false;
    while (std::fgets(chunk, sizeof(chunk), file->handle)) {
        got = true;
        line += chunk;
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
            break;
        }
    }
    if (!got) {
        return from<Nil>(Nil{});
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
}

static Value fileReadlinesNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "readlines");
    if (!file) {
        return from<Nil>(Nil{});
    }
    if (!file->readable) {
        nativeRuntimeError("File is not open for reading.");
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    char chunk[4096];
    std::string line;
    auto flush = [&]() {
        ObjString* s = mm->makeString(line);
        // Protect s across push_back, which can GC on vector resize.
        mm->pushTempRoot(s);
        list->elements.push_back(Value{static_cast<Obj*>(s)});
        mm->popTempRoot();
        line.clear();
    };
    while (std::fgets(chunk, sizeof(chunk), file->handle)) {
        line += chunk;
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
            flush();
        }
    }
    if (!line.empty()) {
        flush(); // trailing line with no newline
    }
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value fileWriteNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "write");
    if (!file) {
        return from<Nil>(Nil{});
    }
    if (!file->writable) {
        nativeRuntimeError("File is not open for writing.");
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'write' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* s = asObjString(as<Obj*>(args[0]));
    std::fwrite(s->chars.data(), 1, s->chars.size(), file->handle);
    return from<Nil>(Nil{});
}

static Value fileWritelineNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "writeline");
    if (!file) {
        return from<Nil>(Nil{});
    }
    if (!file->writable) {
        nativeRuntimeError("File is not open for writing.");
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'writeline' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* s = asObjString(as<Obj*>(args[0]));
    std::fwrite(s->chars.data(), 1, s->chars.size(), file->handle);
    std::fputc('\n', file->handle);
    return from<Nil>(Nil{});
}

static Value fileCloseNative(int /*argc*/, Value* args) {
    // Idempotent — no error if already closed.
    ObjFile* file = asObjFile(as<Obj*>(args[-1]));
    if (file->handle) {
        std::fclose(file->handle);
        file->handle = nullptr;
    }
    return from<Nil>(Nil{});
}

static Value openNative(int /*argc*/, Value* args) {
    if (!isString(args[0]) || !isString(args[1])) {
        nativeRuntimeError("open() requires string path and mode.");
        return from<Nil>(Nil{});
    }
    auto* pathStr = asObjString(as<Obj*>(args[0]));
    auto* modeStr = asObjString(as<Obj*>(args[1]));
    std::string modeS(modeStr->chars.data(), modeStr->chars.size());

    bool readable = false, writable = false;
    const char* cmode = nullptr;
    if (modeS == "r") {
        readable = true;
        cmode = "r";
    } else if (modeS == "w") {
        writable = true;
        cmode = "w";
    } else if (modeS == "a") {
        writable = true;
        cmode = "a";
    } else if (modeS == "r+") {
        readable = writable = true;
        cmode = "r+";
    } else {
        nativeRuntimeError(
            "open(): invalid mode. Expected \"r\", \"w\", \"a\", or \"r+\".");
        return from<Nil>(Nil{});
    }
    std::string pathS(pathStr->chars.data(), pathStr->chars.size());
    FILE* fp = std::fopen(pathS.c_str(), cmode);
    if (!fp) {
        std::string msg = "open(): cannot open '";
        msg += pathS;
        msg += "': ";
        msg += std::strerror(errno);
        nativeRuntimeError(msg.c_str());
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    ObjFile* file = mm->create<ObjFile>(s_fileClass);
    mm->pushTempRoot(file);
    file->handle = fp;
    file->readable = readable;
    file->writable = writable;
    mm->popTempRoot();
    return Value{static_cast<Obj*>(file)};
}

ObjClass* registerFileAPI(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("File");
    reg.mm().pushTempRoot(klass);

    reg.addMethod(klass, "read",      fileReadNative,      0);
    reg.addMethod(klass, "readline",  fileReadlineNative,  0);
    reg.addMethod(klass, "readlines", fileReadlinesNative, 0);
    reg.addMethod(klass, "write",     fileWriteNative,     1);
    reg.addMethod(klass, "writeline", fileWritelineNative, 1);
    reg.addMethod(klass, "close",     fileCloseNative,     0);

    s_fileClass = klass;
    reg.defineGlobal("open", openNative, 2);

    reg.mm().popTempRoot(); // klass
    return klass;
}
