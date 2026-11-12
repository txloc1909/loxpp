#include "file_api.h"
#include "host_string_arg.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

// Module-local class pointer used by openNative to stamp new ObjFile instances.
// Per thread: each worker registers the classes of its own heap.
static thread_local ObjClass* s_fileClass = nullptr;

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

// Reads one line from the stream into `line`, taking the bytes an earlier
// call read ahead first. Returns false at the end of the input, where `line`
// is left empty. A trailing line with no newline still counts as a line.
static bool readStreamLine(ObjFile* file, std::string& line) {
    std::string& buf = file->inbuf;
    size_t scanned = 0;
    size_t nl = std::string::npos;
    char chunk[4096];
    while (true) {
        nl = buf.find('\n', scanned);
        if (nl != std::string::npos) {
            break;
        }
        scanned = buf.size();
        size_t got = std::fread(chunk, 1, sizeof(chunk), file->handle);
        if (got == 0) {
            break;
        }
        buf.append(chunk, got);
    }
    if (buf.empty()) {
        return false;
    }
    if (nl != std::string::npos) {
        line.assign(buf, 0, nl);
        buf.erase(0, nl + 1);
    } else {
        // No newline left: the rest of the input is the last line.
        line.swap(buf);
        buf.clear();
    }
    return true;
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
    buf.swap(file->inbuf); // bytes an earlier readline read ahead
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file->handle)) > 0) {
        buf.append(chunk, got);
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
    if (!readStreamLine(file, line)) {
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
    std::string line;
    while (readStreamLine(file, line)) {
        ObjString* s = mm->makeString(line);
        // Protect s across push_back, which can GC on vector resize.
        mm->pushTempRoot(s);
        list->elements.push_back(Value{static_cast<Obj*>(s)});
        mm->popTempRoot();
    }
    mm->popTempRoot();
    // An empty file gives an empty list, not nil.
    return Value{static_cast<Obj*>(list)};
}

// Puts back the bytes a readline or readlines read past, so that a write
// lands where the program's reads stopped. The call also serves the rule C
// gives an update stream: input may not go directly to output without a file
// positioning call. A File opened for writing alone has neither, and a
// stream that cannot seek cannot hold read-ahead bytes because it can only be
// opened for reading.
static void prepareWrite(ObjFile* file) {
    if (!file->readable) {
        return;
    }
    std::fseek(file->handle, -static_cast<long>(file->inbuf.size()), SEEK_CUR);
    file->inbuf.clear();
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
    prepareWrite(file);
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
    prepareWrite(file);
    std::fwrite(s->chars.data(), 1, s->chars.size(), file->handle);
    std::fputc('\n', file->handle);
    return from<Nil>(Nil{});
}

static Value fileFlushNative(int /*argc*/, Value* args) {
    ObjFile* file = checkFile(args, "flush");
    if (!file) {
        return from<Nil>(Nil{});
    }
    // fflush on a stream opened for reading alone has no defined behavior in
    // C (C11 7.21.5.2p2), so the call is skipped rather than made harmless.
    // A File opened for reading alone has nothing to give: this is the
    // answer spec/05-stdlib.md's f.flush() gives for that File.
    if (!file->writable) {
        return from<Nil>(Nil{});
    }
    if (std::fflush(file->handle) != 0) {
        nativeRuntimeError("flush(): cannot flush the file.");
        return from<Nil>(Nil{});
    }
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
    if (stringHoldsNul(pathS, "open")) {
        return from<Nil>(Nil{});
    }
    // An empty path names no file on any host, so no fopen is tried for it.
    FILE* fp = pathS.empty() ? nullptr : std::fopen(pathS.c_str(), cmode);
    if (!fp) {
        std::string msg = "open(): cannot open '";
        msg += pathS;
        msg += "': ";
        msg += pathS.empty() ? "the path is empty" : std::strerror(errno);
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

    reg.addMethod(klass, "read", fileReadNative, 0);
    reg.addMethod(klass, "readline", fileReadlineNative, 0);
    reg.addMethod(klass, "readlines", fileReadlinesNative, 0);
    reg.addMethod(klass, "write", fileWriteNative, 1);
    reg.addMethod(klass, "writeline", fileWritelineNative, 1);
    reg.addMethod(klass, "flush", fileFlushNative, 0);
    reg.addMethod(klass, "close", fileCloseNative, 0);

    s_fileClass = klass;
    reg.defineGlobal("open", openNative, 2);

    reg.mm().popTempRoot(); // klass
    return klass;
}
