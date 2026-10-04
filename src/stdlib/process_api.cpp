#include "process_api.h"
#include "stdlib_context.h"
#include "stream_io.h"
#include "../container_objects.h"
#include "../vm_allocator.h"
#include "../value.h"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static ObjClass* s_processClass = nullptr;
static ObjClass* s_mapClass = nullptr;

static bool asStringArg(const Value& v, std::string& out) {
    if (!isString(v)) {
        nativeRuntimeError("Expected a string argument.");
        return false;
    }
    auto* s = asObjString(as<Obj*>(v));
    out.assign(s->chars.data(), s->chars.size());
    return true;
}

// Collect a List of Strings into a vector. A non-List or a non-String element
// is a runtime error.
static bool asStringList(const Value& v, std::vector<std::string>& out) {
    if (!isList(v)) {
        nativeRuntimeError("args must be a list of strings.");
        return false;
    }
    ObjList* list = asObjList(as<Obj*>(v));
    out.reserve(list->elements.size());
    for (const Value& e : list->elements) {
        if (!isString(e)) {
            nativeRuntimeError("args must be a list of strings.");
            return false;
        }
        auto* s = asObjString(as<Obj*>(e));
        out.emplace_back(s->chars.data(), s->chars.size());
    }
    return true;
}

// Validate a byte-count argument. `n` must be a non-negative integer Number.
static bool asByteCount(const Value& v, const char* method, size_t& out) {
    if (!is<Number>(v)) {
        std::string msg =
            std::string(method) + "() byte count must be a number.";
        nativeRuntimeError(msg.c_str());
        return false;
    }
    double raw = as<Number>(v);
    if (!std::isfinite(raw) || raw != std::floor(raw) || raw < 0 ||
        raw > static_cast<double>(INT_MAX)) {
        std::string msg =
            std::string(method) +
            "() byte count must be an integer in range 0 to 2147483647.";
        nativeRuntimeError(msg.c_str());
        return false;
    }
    out = static_cast<size_t>(raw);
    return true;
}

// Turn a wait(2) status word into the language-level status: the exit code,
// or 128 + signal when the child was signalled.
static int statusFromWait(int st) {
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    if (WIFSIGNALED(st)) {
        return 128 + WTERMSIG(st);
    }
    return 1;
}

// Start a child with three pipes. On success, returns an ObjProcess (not yet
// rooted) and leaves `errMsg` untouched. On failure, returns nullptr and sets
// `errMsg` to a message naming the program.
//
// A CLOEXEC error pipe lets the parent tell a failed exec apart from a
// successful one: the child writes errno to it if execvp fails, and it closes
// on exec, so the parent's read returns 0 on success.
static ObjProcess* spawnProcess(const std::string& program,
                                const std::vector<std::string>& args,
                                std::string& errMsg, MemoryManager* mm) {
    std::vector<std::string> argvStorage;
    argvStorage.reserve(args.size() + 1);
    argvStorage.push_back(program);
    for (const std::string& a : args) {
        argvStorage.push_back(a);
    }
    std::vector<char*> argv;
    argv.reserve(argvStorage.size() + 1);
    for (std::string& s : argvStorage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int execPipe[2] = {-1, -1};
    if (pipe2(inPipe, O_CLOEXEC) != 0 || pipe2(outPipe, O_CLOEXEC) != 0 ||
        pipe2(errPipe, O_CLOEXEC) != 0 || pipe2(execPipe, O_CLOEXEC) != 0) {
        errMsg = std::string("spawn(): cannot create pipes: ") +
                 std::strerror(errno);
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0],
                       errPipe[1], execPipe[0], execPipe[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return nullptr;
    }

    pid_t pid = fork();
    if (pid < 0) {
        errMsg = std::string("spawn(): cannot fork: ") + std::strerror(errno);
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0],
                       errPipe[1], execPipe[0], execPipe[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return nullptr;
    }

    if (pid == 0) {
        // Child. Only async-signal-safe calls run before execvp.
        if (dup2(inPipe[0], STDIN_FILENO) < 0 ||
            dup2(outPipe[1], STDOUT_FILENO) < 0 ||
            dup2(errPipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        ::close(execPipe[0]);
        execvp(program.c_str(), argv.data());
        int err = errno;
        (void)!write(execPipe[1], &err, sizeof(err));
        _exit(127);
    }

    // Parent. Close the ends the child owns.
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    ::close(execPipe[1]);

    int childErrno = 0;
    ssize_t n;
    do {
        n = read(execPipe[0], &childErrno, sizeof(childErrno));
    } while (n < 0 && errno == EINTR);
    ::close(execPipe[0]);

    if (childErrno != 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        errMsg = "spawn(): cannot run '" + program +
                 "': " + std::strerror(childErrno);
        return nullptr;
    }

    FILE* in = fdopen(inPipe[1], "w");
    FILE* out = fdopen(outPipe[0], "r");
    FILE* err = fdopen(errPipe[0], "r");
    if (in == nullptr || out == nullptr || err == nullptr) {
        if (in != nullptr) {
            std::fclose(in);
        } else {
            ::close(inPipe[1]);
        }
        if (out != nullptr) {
            std::fclose(out);
        } else {
            ::close(outPipe[0]);
        }
        if (err != nullptr) {
            std::fclose(err);
        } else {
            ::close(errPipe[0]);
        }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        }
        errMsg = "spawn(): cannot open child streams.";
        return nullptr;
    }

    ObjProcess* p = mm->create<ObjProcess>(s_processClass);
    p->pid = static_cast<long>(pid);
    p->in = in;
    p->out = out;
    p->err = err;
    return p;
}

static Value spawnNative(int /*argc*/, Value* argv) {
    std::string program;
    if (!asStringArg(argv[0], program)) {
        return from<Nil>(Nil{});
    }
    std::vector<std::string> args;
    if (!asStringList(argv[1], args)) {
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    std::string errMsg;
    ObjProcess* p = spawnProcess(program, args, errMsg, mm);
    if (p == nullptr) {
        nativeRuntimeError(errMsg.c_str());
        return from<Nil>(Nil{});
    }
    // Collection cannot run between create<ObjProcess> and here, but the
    // process is rooted before any later allocation regardless.
    mm->pushTempRoot(p);
    mm->popTempRoot();
    return Value{static_cast<Obj*>(p)};
}

static Value runNative(int /*argc*/, Value* argv) {
    std::string program;
    if (!asStringArg(argv[0], program)) {
        return from<Nil>(Nil{});
    }
    std::vector<std::string> args;
    if (!asStringList(argv[1], args)) {
        return from<Nil>(Nil{});
    }
    MemoryManager* mm = getActiveMM();
    std::string errMsg;
    ObjProcess* p = spawnProcess(program, args, errMsg, mm);
    if (p == nullptr) {
        nativeRuntimeError(errMsg.c_str());
        return from<Nil>(Nil{});
    }
    mm->pushTempRoot(p);

    // run() is non-interactive: close the child's stdin so a reader such as
    // `cat` sees EOF instead of blocking.
    std::fclose(p->in);
    p->in = nullptr;

    // Drain stderr on its own thread, so a child that fills both pipe buffers
    // cannot deadlock against a parent reading only stdout.
    std::string errBuf;
    std::thread errThread([p, &errBuf]() {
        char buf[4096];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), p->err)) > 0) {
            errBuf.append(buf, got);
        }
    });
    std::string outBuf;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), p->out)) > 0) {
        outBuf.append(chunk, got);
    }
    errThread.join();

    std::fclose(p->out);
    p->out = nullptr;
    std::fclose(p->err);
    p->err = nullptr;

    int st = 0;
    while (waitpid(static_cast<pid_t>(p->pid), &st, 0) < 0 && errno == EINTR) {
    }
    p->reaped = true;
    p->status = statusFromWait(st);

    ObjMap* map = mm->create<ObjMap>(s_mapClass, VmAllocator<MapEntry>{mm});
    mm->pushTempRoot(map);
    auto setKey = [&](const char* key, const Value& value) {
        ObjString* ks = mm->makeString(key);
        mm->pushTempRoot(ks);
        map->mapSet(Value{static_cast<Obj*>(ks)}, value);
        mm->popTempRoot();
    };
    setKey("status", from<Number>(static_cast<Number>(p->status)));
    ObjString* outStr = mm->makeString(outBuf);
    mm->pushTempRoot(outStr);
    setKey("stdout", Value{static_cast<Obj*>(outStr)});
    mm->popTempRoot();
    ObjString* errStr = mm->makeString(errBuf);
    mm->pushTempRoot(errStr);
    setKey("stderr", Value{static_cast<Obj*>(errStr)});
    mm->popTempRoot();

    mm->popTempRoot(); // map
    mm->popTempRoot(); // p
    return Value{static_cast<Obj*>(map)};
}

static ObjProcess* checkProcess(Value* args, const char* method, bool writing) {
    ObjProcess* p = asObjProcess(as<Obj*>(args[-1]));
    if (p->reaped && writing) {
        std::string msg = "Cannot call '";
        msg += method;
        msg += "' after wait().";
        nativeRuntimeError(msg.c_str());
        return nullptr;
    }
    if (writing && p->in == nullptr) {
        std::string msg = "Cannot call '";
        msg += method;
        msg += "' on a closed process stdin.";
        nativeRuntimeError(msg.c_str());
        return nullptr;
    }
    return p;
}

static Value processReadNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "read", false);
    if (p == nullptr || p->out == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string buf;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), p->out)) > 0) {
        buf.append(chunk, got);
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value processReadBytesNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "read_bytes", false);
    if (p == nullptr) {
        return from<Nil>(Nil{});
    }
    size_t n = 0;
    if (!asByteCount(args[0], "read_bytes", n)) {
        return from<Nil>(Nil{});
    }
    if (p->out == nullptr) {
        return Value{static_cast<Obj*>(getActiveMM()->makeString(""))};
    }
    std::string buf = readStreamBytes(p->out, n);
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value processReadlineNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "readline", false);
    if (p == nullptr || p->out == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string line;
    bool sawByte = false;
    int c;
    while ((c = std::fgetc(p->out)) != EOF) {
        sawByte = true;
        if (c == '\n') {
            break;
        }
        line.push_back(static_cast<char>(c));
    }
    if (!sawByte) {
        return from<Nil>(Nil{});
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
}

static Value processReadlinesFrom(FILE* stream, MemoryManager* mm) {
    ObjList* list = mm->create<ObjList>(VmAllocator<Value>{mm});
    mm->pushTempRoot(list);
    std::string line;
    bool sawByte = false;
    bool done = false;
    int c;
    while (!done) {
        c = std::fgetc(stream);
        if (c == EOF) {
            done = true;
        } else if (c == '\n') {
            ObjString* str = mm->makeString(line);
            mm->pushTempRoot(str);
            list->elements.emplace_back(static_cast<Obj*>(str));
            mm->popTempRoot();
            line.clear();
            sawByte = false;
        } else {
            sawByte = true;
            line.push_back(static_cast<char>(c));
        }
    }
    if (sawByte) {
        ObjString* str = mm->makeString(line);
        mm->pushTempRoot(str);
        list->elements.emplace_back(static_cast<Obj*>(str));
        mm->popTempRoot();
    }
    mm->popTempRoot();
    return Value{static_cast<Obj*>(list)};
}

static Value processReadlinesNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "readlines", false);
    if (p == nullptr || p->out == nullptr) {
        return from<Nil>(Nil{});
    }
    return processReadlinesFrom(p->out, getActiveMM());
}

static Value processErrReadlinesNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "err_readlines", false);
    if (p == nullptr || p->err == nullptr) {
        return from<Nil>(Nil{});
    }
    return processReadlinesFrom(p->err, getActiveMM());
}

static Value processReadErrNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "read_err", false);
    if (p == nullptr || p->err == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string buf;
    char chunk[4096];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), p->err)) > 0) {
        buf.append(chunk, got);
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value processErrReadBytesNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "err_read_bytes", false);
    if (p == nullptr) {
        return from<Nil>(Nil{});
    }
    size_t n = 0;
    if (!asByteCount(args[0], "err_read_bytes", n)) {
        return from<Nil>(Nil{});
    }
    if (p->err == nullptr) {
        return Value{static_cast<Obj*>(getActiveMM()->makeString(""))};
    }
    std::string buf = readStreamBytes(p->err, n);
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(buf)))};
}

static Value processErrReadlineNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "err_readline", false);
    if (p == nullptr || p->err == nullptr) {
        return from<Nil>(Nil{});
    }
    std::string line;
    bool sawByte = false;
    int c;
    while ((c = std::fgetc(p->err)) != EOF) {
        sawByte = true;
        if (c == '\n') {
            break;
        }
        line.push_back(static_cast<char>(c));
    }
    if (!sawByte) {
        return from<Nil>(Nil{});
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(std::move(line)))};
}

static Value processWriteNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "write", true);
    if (p == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'write' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* str = asObjString(as<Obj*>(args[0]));
    writeStreamOrError(p->in, "write",
                       std::string_view(str->chars.data(), str->chars.size()),
                       false);
    return from<Nil>(Nil{});
}

static Value processWritelineNative(int /*argc*/, Value* args) {
    ObjProcess* p = checkProcess(args, "writeline", true);
    if (p == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isString(args[0])) {
        nativeRuntimeError("'writeline' argument must be a string.");
        return from<Nil>(Nil{});
    }
    auto* str = asObjString(as<Obj*>(args[0]));
    writeStreamOrError(p->in, "writeline",
                       std::string_view(str->chars.data(), str->chars.size()),
                       true);
    return from<Nil>(Nil{});
}

static Value processCloseStdinNative(int /*argc*/, Value* args) {
    ObjProcess* p = asObjProcess(as<Obj*>(args[-1]));
    if (p->in != nullptr) {
        std::fclose(p->in);
        p->in = nullptr;
    }
    return from<Nil>(Nil{});
}

static Value processWaitNative(int /*argc*/, Value* args) {
    ObjProcess* p = asObjProcess(as<Obj*>(args[-1]));
    if (!p->reaped) {
        int st = 0;
        pid_t r;
        do {
            r = waitpid(static_cast<pid_t>(p->pid), &st, 0);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            std::string msg = std::string("wait(): ") + std::strerror(errno);
            nativeRuntimeError(msg.c_str());
            return from<Nil>(Nil{});
        }
        p->status = statusFromWait(st);
        p->reaped = true;
    }
    return from<Number>(static_cast<Number>(p->status));
}

static Value processKillNative(int /*argc*/, Value* args) {
    ObjProcess* p = asObjProcess(as<Obj*>(args[-1]));
    if (!p->reaped && p->pid > 0) {
        kill(static_cast<pid_t>(p->pid), SIGKILL);
    }
    return from<Nil>(Nil{});
}

static Value processPidNative(int /*argc*/, Value* args) {
    ObjProcess* p = asObjProcess(as<Obj*>(args[-1]));
    return from<Number>(static_cast<Number>(p->pid));
}

ObjClass* registerProcessAPI(StdlibRegistrar& reg, ObjClass* mapClass) {
    s_mapClass = mapClass;
    ObjClass* klass = reg.makeClass("Process");
    reg.mm().pushTempRoot(klass);
    reg.addMethod(klass, "read", processReadNative, 0);
    reg.addMethod(klass, "read_bytes", processReadBytesNative, 1);
    reg.addMethod(klass, "readline", processReadlineNative, 0);
    reg.addMethod(klass, "readlines", processReadlinesNative, 0);
    reg.addMethod(klass, "read_err", processReadErrNative, 0);
    reg.addMethod(klass, "err_read_bytes", processErrReadBytesNative, 1);
    reg.addMethod(klass, "err_readline", processErrReadlineNative, 0);
    reg.addMethod(klass, "err_readlines", processErrReadlinesNative, 0);
    reg.addMethod(klass, "write", processWriteNative, 1);
    reg.addMethod(klass, "writeline", processWritelineNative, 1);
    reg.addMethod(klass, "close_stdin", processCloseStdinNative, 0);
    reg.addMethod(klass, "wait", processWaitNative, 0);
    reg.addMethod(klass, "kill", processKillNative, 0);
    reg.addMethod(klass, "pid", processPidNative, 0);

    s_processClass = klass;
    reg.defineGlobal("spawn", spawnNative, 2);
    reg.defineGlobal("run", runNative, 2);
    reg.mm().popTempRoot(); // klass
    return klass;
}
