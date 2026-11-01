#include "task_api.h"
#include "reflect_api.h"
#include "stdlib_context.h"
#include "../class_objects.h"
#include "../container_objects.h"
#include "../exec_objects.h"
#include "../runtime.h"
#include "../value.h"

#include <string>

// Every member needs the scheduler of runTasks(). A Runtime that runs a
// program without it (rt_call, backend/rt_capi.h) has no task, so there the
// members fail before they touch one. The QBE front end runs every program
// that names `task` through the scheduler (rt_run_tasks).
static Runtime* taskRuntime() {
    Runtime* rt = getActiveRuntime();
    if (rt->currentTask() == nullptr) {
        nativeRuntimeError("task: no task scheduler runs this program.");
        return nullptr;
    }
    return rt;
}

static Value taskValue(ObjTask* task) { return Value{static_cast<Obj*>(task)}; }

// task.spawn(fn): spec/04-semantics.md, task.spawn.
static Value taskSpawnNative(int /*argCount*/, Value* argv) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    Value fn = argv[0];
    if (!isCallableValue(fn)) {
        nativeRuntimeError("task.spawn expects a function.");
        return from<Nil>(Nil{});
    }
    return taskValue(rt->spawnTask(fn));
}

static void raiseSendError(Runtime* rt, const Value& msg) {
    std::string text = "Value of type ";
    text += valueTypeName(msg);
    text += " is not a message.";
    rt->raiseFromNative("SendError", text.c_str());
}

// task.spawn_worker(fn, msg): spec/04-semantics.md, task.spawn_worker, in
// its order of checks.
static Value taskSpawnWorkerNative(int /*argCount*/, Value* argv) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!rt->isTopLevelFunction(argv[0])) {
        nativeRuntimeError("task.spawn_worker expects a function that a "
                           "top-level fun declaration made.");
        return from<Nil>(Nil{});
    }
    Value msg = argv[1];
    if (!isMessage(msg)) {
        raiseSendError(rt, msg);
        return from<Nil>(Nil{});
    }
    return taskValue(rt->spawnWorker(asObjClosure(as<Obj*>(argv[0])), msg));
}

// task.send(t, msg): spec/04-semantics.md, task.send, in its order of checks.
static Value taskSendNative(int /*argCount*/, Value* argv) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    if (!isTask(argv[0])) {
        nativeRuntimeError("task.send expects a task.");
        return from<Nil>(Nil{});
    }
    ObjTask* target = asObjTask(as<Obj*>(argv[0]));
    Value msg = argv[1];
    if (!isMessage(msg)) {
        raiseSendError(rt, msg);
        return from<Nil>(Nil{});
    }
    if (rt->taskEnded(target) || !rt->sendMessage(target, msg)) {
        rt->raiseFromNative("DeadTaskError",
                            "Cannot send to a task that has ended.");
        return from<Nil>(Nil{});
    }
    return from<Nil>(Nil{});
}

// task.receive(): on an empty mailbox the Runtime either requests a task
// switch, which callNative carries out when this native returns, or raises.
static Value taskReceiveNative(int /*argCount*/, Value* /*argv*/) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    Value msg;
    if (!rt->receiveFromNative(&msg)) {
        return from<Nil>(Nil{});
    }
    return msg;
}

// The limit of a receive_* call: a Number greater than zero. A Number that is
// not greater than zero includes NaN.
static bool checkLimit(const char* name, const Value& seconds) {
    if (!is<Number>(seconds)) {
        std::string text = std::string(name) + " expects a number of seconds.";
        nativeRuntimeError(text.c_str());
        return false;
    }
    if (!(as<Number>(seconds) > 0)) {
        std::string text =
            std::string(name) + " expects a limit greater than zero.";
        nativeRuntimeError(text.c_str());
        return false;
    }
    return true;
}

static bool checkWant(const char* name, const Value& want) {
    if (!isCallableValue(want)) {
        std::string text = std::string(name) + " expects a function.";
        nativeRuntimeError(text.c_str());
        return false;
    }
    return true;
}

// task.receive_within(seconds), task.receive_where(want), and
// task.receive_where_within(want, seconds): spec/04-semantics.md,
// task.receive_within. Each returns the message, or requests a task switch
// like task.receive, or raises.
static Value taskReceiveCall(const char* name, bool hasWant, bool limited,
                             Value* argv) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    Value want = from<Nil>(Nil{});
    if (hasWant) {
        want = argv[0];
        if (!checkWant(name, want)) {
            return from<Nil>(Nil{});
        }
    }
    double seconds = 0;
    if (limited) {
        const Value& limit = argv[hasWant ? 1 : 0];
        if (!checkLimit(name, limit)) {
            return from<Nil>(Nil{});
        }
        seconds = as<Number>(limit);
    }
    Value msg;
    if (!rt->selectiveReceive(want, hasWant, limited, seconds, &msg)) {
        return from<Nil>(Nil{});
    }
    return msg;
}

static Value taskReceiveWithinNative(int /*argCount*/, Value* argv) {
    return taskReceiveCall("task.receive_within", false, true, argv);
}

static Value taskReceiveWhereNative(int /*argCount*/, Value* argv) {
    return taskReceiveCall("task.receive_where", true, false, argv);
}

static Value taskReceiveWhereWithinNative(int /*argCount*/, Value* argv) {
    return taskReceiveCall("task.receive_where_within", true, true, argv);
}

static Value taskSelfNative(int /*argCount*/, Value* /*argv*/) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    return taskValue(rt->currentTask());
}

static Value taskPendingNative(int /*argCount*/, Value* /*argv*/) {
    Runtime* rt = taskRuntime();
    if (rt == nullptr) {
        return from<Nil>(Nil{});
    }
    return from<Number>(static_cast<Number>(rt->currentTask()->mailbox.size()));
}

void registerTaskAPI(StdlibRegistrar& reg) {
    ObjClass* moduleClass = reg.makeClass("TaskModule");
    reg.mm().pushTempRoot(moduleClass);
    ObjInstance* module = reg.makeInstance(moduleClass);
    reg.mm().pushTempRoot(module);
    reg.addNativeField(module, "spawn", taskSpawnNative, 1);
    reg.addNativeField(module, "spawn_worker", taskSpawnWorkerNative, 2);
    reg.addNativeField(module, "send", taskSendNative, 2);
    reg.addNativeField(module, "receive", taskReceiveNative, 0);
    reg.addNativeField(module, "receive_within", taskReceiveWithinNative, 1);
    reg.addNativeField(module, "receive_where", taskReceiveWhereNative, 1);
    reg.addNativeField(module, "receive_where_within",
                       taskReceiveWhereWithinNative, 2);
    reg.addNativeField(module, "self", taskSelfNative, 0);
    reg.addNativeField(module, "pending", taskPendingNative, 0);
    reg.defineGlobalValue("task", Value{static_cast<Obj*>(module)});
    reg.mm().popTempRoot(); // module
    reg.mm().popTempRoot(); // moduleClass
}
