#include "os_api.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

// Module-local Map class pointer, used by statNative to stamp the returned
// metadata map so it dispatches to the shared Map methods.
static ObjClass* s_mapClass = nullptr;

// Convert a filesystem clock time to seconds since the Unix epoch. The
// file_clock epoch is implementation-defined, so shift by the live offset
// between the file_clock and the system clock rather than assuming they align.
static double
unixSecondsFromFileTime(const std::filesystem::file_time_type& t) {
    using namespace std::chrono;
    const auto clockOffset =
        system_clock::now().time_since_epoch() -
        std::filesystem::file_time_type::clock::now().time_since_epoch();
    return duration<double>(t.time_since_epoch() + clockOffset).count();
}

// Convert a Value to a std::string. Returns false (and reports a runtime
// error) if the value is not a String.
static bool asPathString(const Value& v, std::string& out) {
    if (!isString(v)) {
        nativeRuntimeError("Expected a string argument.");
        return false;
    }
    auto* s = asObjString(as<Obj*>(v));
    out.assign(s->chars.data(), s->chars.size());
    return true;
}

static Value argsNative(int /*argc*/, Value* /*argv*/) {
    MemoryManager* mm = getActiveMM();
    auto* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    for (const std::string& a : getActiveArgs()) {
        ObjString* s = mm->makeString(a);
        mm->pushTempRoot(s);
        list->elements.emplace_back(static_cast<Obj*>(s));
        mm->popTempRoot();
    }
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value envNative(int /*argc*/, Value* argv) {
    std::string key;
    if (!asPathString(argv[0], key)) {
        return from<Nil>(Nil{});
    }
    const char* val = std::getenv(key.c_str());
    if (val == nullptr) {
        return from<Nil>(Nil{});
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(val))};
}

static Value exitNative(int /*argc*/, Value* argv) {
    if (!is<Number>(argv[0])) {
        nativeRuntimeError("exit() code must be a number.");
        return from<Nil>(Nil{});
    }
    double raw = as<Number>(argv[0]);
    // Truncate toward zero, the C-style integral conversion. Reject any value
    // for which that conversion is undefined behaviour — a non-finite number
    // or one outside the int range — before it happens.
    if (!std::isfinite(raw) || raw > INT_MAX || raw < INT_MIN) {
        nativeRuntimeError("exit() code must be a finite number in the "
                           "integer range.");
        return from<Nil>(Nil{});
    }
    std::exit(static_cast<int>(std::trunc(raw)));
    return from<Nil>(Nil{});
}

static Value timeNative(int /*argc*/, Value* /*argv*/) {
    return from<Number>(static_cast<Number>(std::time(nullptr)));
}

static Value sleepNative(int /*argc*/, Value* argv) {
    if (!is<Number>(argv[0])) {
        nativeRuntimeError("sleep() duration must be a number.");
        return from<Nil>(Nil{});
    }
    double seconds = as<Number>(argv[0]);
    if (seconds > 0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
    return from<Nil>(Nil{});
}

static Value existsNative(int /*argc*/, Value* argv) {
    std::string p;
    if (!asPathString(argv[0], p)) {
        return from<Nil>(Nil{});
    }
    std::error_code ec;
    return from<bool>(std::filesystem::exists(std::filesystem::path(p), ec));
}

static Value isDirNative(int /*argc*/, Value* argv) {
    std::string p;
    if (!asPathString(argv[0], p)) {
        return from<Nil>(Nil{});
    }
    std::error_code ec;
    return from<bool>(
        std::filesystem::is_directory(std::filesystem::path(p), ec));
}

static Value isFileNative(int /*argc*/, Value* argv) {
    std::string p;
    if (!asPathString(argv[0], p)) {
        return from<Nil>(Nil{});
    }
    std::error_code ec;
    return from<bool>(
        std::filesystem::is_regular_file(std::filesystem::path(p), ec));
}

static Value statNative(int /*argc*/, Value* argv) {
    std::string p;
    if (!asPathString(argv[0], p)) {
        return from<Nil>(Nil{});
    }
    std::filesystem::path fs(p);
    std::error_code ec;
    if (!std::filesystem::exists(fs, ec)) {
        return from<Nil>(Nil{});
    }

    MemoryManager* mm = getActiveMM();
    auto* map = mm->create<ObjMap>(s_mapClass, VmAllocator<MapEntry>{mm});
    mm->pushTempRoot(map);

    std::filesystem::file_status st = std::filesystem::status(fs, ec);
    bool isDir = st.type() == std::filesystem::file_type::directory;
    bool isFile = st.type() == std::filesystem::file_type::regular;

    auto setBool = [&](const char* key, bool val) {
        ObjString* ks = mm->makeString(key);
        mm->pushTempRoot(ks);
        map->mapSet(Value{static_cast<Obj*>(ks)}, from<bool>(val));
        mm->popTempRoot();
    };
    setBool("exists", true);
    setBool("is_dir", isDir);
    setBool("is_file", isFile);

    if (isFile) {
        std::error_code ecSize;
        auto size = std::filesystem::file_size(fs, ecSize);
        ObjString* ks = mm->makeString("size");
        mm->pushTempRoot(ks);
        map->mapSet(Value{static_cast<Obj*>(ks)},
                    from<Number>(static_cast<Number>(size)));
        mm->popTempRoot();
    }

    std::error_code ecTime;
    auto mtime = std::filesystem::last_write_time(fs, ecTime);
    if (!ecTime) {
        ObjString* ks = mm->makeString("mtime");
        mm->pushTempRoot(ks);
        map->mapSet(Value{static_cast<Obj*>(ks)},
                    from<Number>(unixSecondsFromFileTime(mtime)));
        mm->popTempRoot();
    }

    mm->popTempRoot();
    return Value{static_cast<Obj*>(map)};
}

void registerOSAPI(StdlibRegistrar& reg, ObjClass* mapClass) {
    s_mapClass = mapClass;
    reg.defineGlobal("args", argsNative, 0);
    reg.defineGlobal("env", envNative, 1);
    reg.defineGlobal("exit", exitNative, 1);
    reg.defineGlobal("time", timeNative, 0);
    reg.defineGlobal("sleep", sleepNative, 1);
    reg.defineGlobal("exists", existsNative, 1);
    reg.defineGlobal("is_dir", isDirNative, 1);
    reg.defineGlobal("is_file", isFileNative, 1);
    reg.defineGlobal("stat", statNative, 1);
}
