#include "os_api.h"
#include "stdlib_context.h"
#include "../poller.h"
#include "../runtime.h"
#include "../worker.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <algorithm>
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
// Per thread: each worker registers the classes of its own heap.
static thread_local ObjClass* s_mapClass = nullptr;

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
    // An Int is already an integer. A Number is truncated toward zero, the
    // C-style integral conversion. Both must fit the signed 32-bit code.
    // 2^63 is exact, so it is the first Number above the Int range.
    static constexpr double kTwoToThe63 = 9223372036854775808.0;
    Int code = 0;
    if (is<Int>(argv[0])) {
        code = as<Int>(argv[0]);
    } else if (is<Number>(argv[0])) {
        const Number raw = as<Number>(argv[0]);
        // Reject any value for which the truncation to an Int is undefined
        // behaviour — a non-finite number or one outside the Int range —
        // before it happens.
        if (!std::isfinite(raw) || raw >= kTwoToThe63 || raw < -kTwoToThe63) {
            nativeRuntimeError("exit() code must be a finite number in the "
                               "integer range.");
            return from<Nil>(Nil{});
        }
        code = static_cast<Int>(std::trunc(raw));
    } else {
        nativeRuntimeError("exit() code must be a number.");
        return from<Nil>(Nil{});
    }
    if (code > INT_MAX || code < INT_MIN) {
        nativeRuntimeError("exit() code must be a finite number in the "
                           "integer range.");
        return from<Nil>(Nil{});
    }
    // Any worker can end the program (spec/04-semantics.md, Ending).
    endProcess(static_cast<int>(code));
    return from<Nil>(Nil{});
}

static Value timeNative(int /*argc*/, Value* /*argv*/) {
    // Sub-second: a program that times out a key or stamps an event in
    // milliseconds needs a wall clock finer than std::time's whole seconds.
    using Seconds = std::chrono::duration<double>;
    return from<Number>(
        Seconds(std::chrono::system_clock::now().time_since_epoch()).count());
}

static Value sleepNative(int /*argc*/, Value* argv) {
    if (!is<Number>(argv[0])) {
        nativeRuntimeError("sleep() duration must be a number.");
        return from<Nil>(Nil{});
    }
    double seconds = as<Number>(argv[0]);
    // Zero, a negative duration and NaN return at once with no suspension.
    if (!(seconds > 0)) {
        return from<Nil>(Nil{});
    }
    const auto deadline = Poller::deadlineAfter(seconds);
    if (getActiveRuntime()->requestWait(WaitEvent::TIMER, -1, deadline)) {
        return from<Nil>(Nil{});
    }
    // The task cannot suspend here, so the worker waits in the kernel.
    std::this_thread::sleep_until(deadline);
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

static Value mkdirNative(int argc, Value* argv) {
    // Variadic so one native serves both spec forms; the count check is here
    // because callNative's arity guard cannot express "1 or 2".
    if (argc != 1 && argc != 2) {
        nativeRuntimeError("Expected 1 or 2 arguments.");
        return from<Nil>(Nil{});
    }
    std::string p;
    if (!asPathString(argv[0], p)) {
        return from<Nil>(Nil{});
    }
    bool parents = false;
    if (argc == 2) {
        std::string mode;
        if (!asPathString(argv[1], mode)) {
            return from<Nil>(Nil{});
        }
        if (mode != "p") {
            nativeRuntimeError("mkdir() mode must be \"p\".");
            return from<Nil>(Nil{});
        }
        parents = true;
    }

    std::error_code ec;
    bool created = parents ? std::filesystem::create_directories(p, ec)
                           : std::filesystem::create_directory(p, ec);
    // An existing path in one-directory form is a fault, but the standard
    // library reports it as "not created, no error" rather than as an error.
    if (ec || (!parents && !created)) {
        std::string msg = "mkdir(): cannot create '" + p + "'";
        if (ec) {
            msg += ": " + ec.message();
        }
        getActiveRuntime()->raiseFromNative("DirectoryError", msg.c_str());
    }
    return from<Nil>(Nil{});
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
    reg.defineGlobal("mkdir", mkdirNative, -1);
}
