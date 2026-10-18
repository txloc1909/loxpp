package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;

import java.util.ArrayList;
import java.util.List;

/**
 * Direct tests for workers, tasks, and messages (spec/04-semantics.md,
 * Workers and Tasks). Generated code is not involved: each task's function
 * is a hand-written {@link LoxClosure}, and the members of the `task` module
 * are called through the globals {@link LoxRuntime#init} builds.
 */
public final class TaskTest {
    private static final Object[] NO_ARGS = new Object[0];
    private static final Object[][] NO_UPVALUES = new Object[0][];

    private static LoxGlobals globals;

    public static void main(String[] args) {
        globals = LoxRuntime.init();
        checkMessageRule();
        checkSendAndReceiveInOrder();
        checkSendError();
        checkSpawnOrderAndDeadTask();
        checkReceiveSuspendsTheTask();
        checkReceiveAcrossNative();
        checkEachTaskHasItsOwnCounters();
        checkReceiveInCoroutineKeepsItRunning();
        checkReceiveInNativeCoroutineEntryRaises();
        checkReceiveInNativeCoroutineEntryWithMessage();
        checkReceiveUnderANativeResumeEntryRaises();
        checkSpawnWorkerEntry();
        checkWorker();

        System.exit(TestSupport.finish("TaskTest"));
    }

    private static Object task(String member, Object... args) {
        LoxInstance module = (LoxInstance)globals.get("task");
        return ((LoxCallable)module.fields.get(member)).call(args);
    }

    private static LoxError raised(Runnable action) {
        try {
            action.run();
        } catch (LoxError e) {
            return e;
        }
        return null;
    }

    private static String kindOf(LoxError e) {
        return (String)((LoxInstance)e.getValue()).fields.get("kind");
    }

    private static String messageOf(LoxError e) {
        return (String)((LoxInstance)e.getValue()).fields.get("message");
    }

    private static LoxClosure fn(String name, Runnable body) {
        return new LoxClosure(name, 0, NO_UPVALUES) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                body.run();
                return null;
            }
        };
    }

    private static void checkMessageRule() {
        LoxEnumCtor point = new LoxEnumCtor(0, 2, "Point", "Shape");
        LoxEnumCtor tagged = new LoxEnumCtor(1, 2, "Tagged", "Shape");
        Object ok = tagged.call(new Object[] {"t", point.call(new Object[] {1.0, 2.0})});
        Object listField = point.call(new Object[] {new LoxList(), 2.0});
        Object nested = tagged.call(new Object[] {"t", listField});

        check(LoxTask.isMessage(null), "nil is a message");
        check(LoxTask.isMessage(true), "a Boolean is a message");
        check(LoxTask.isMessage(1.5), "a Number is a message");
        check(LoxTask.isMessage("s"), "a String is a message");
        check(LoxTask.isMessage(task("self")), "a Task is a message");
        check(LoxTask.isMessage(ok), "an Enum of messages is a message");
        check(!LoxTask.isMessage(listField),
              "an Enum with a List field is not a message");
        check(!LoxTask.isMessage(nested),
              "an Enum with a nested List field is not a message");
        check(!LoxTask.isMessage(new LoxList()), "a List is not a message");
        check(!LoxTask.isMessage(new LoxMap()), "a Map is not a message");
        check(!LoxTask.isMessage(point), "an EnumConstructor is not a message");
        check(!LoxTask.isMessage(fn("f", () -> {})), "a Function is not a message");
    }

    private static void checkSendAndReceiveInOrder() {
        Object me = task("self");
        checkEquals(null, task("send", me, 1.0), "a send returns nil");
        task("send", me, null);
        task("send", me, "s");
        checkEquals(3.0, task("pending"), "pending is the mailbox length");
        checkEquals(1.0, task("receive"), "receive takes the head first");
        checkEquals(null, task("receive"), "nil passes as a message");
        checkEquals("s", task("receive"), "the last message comes last");
        checkEquals(0.0, task("pending"), "the mailbox is empty again");
    }

    private static void checkSendError() {
        Object me = task("self");
        LoxError err = raised(() -> task("send", me, new LoxList()));
        check(err != null && err.isCatchable() &&
                  "SendError".equals(kindOf(err)),
              "a send of a List raises a catchable SendError");
        checkEquals("Value of type List is not a message.", messageOf(err),
                    "the SendError message names the type");
        checkEquals(0.0, task("pending"), "a refused message is not appended");
        LoxError notTask = raised(() -> task("send", "x", 1.0));
        check(notTask != null && !notTask.isCatchable(),
              "a send to a value that is not a Task is fatal");
    }

    private static void checkSpawnOrderAndDeadTask() {
        List<String> ran = new ArrayList<>();
        Object a = task("spawn", fn("a", () -> ran.add("a")));
        Object b = task("spawn", fn("b", () -> ran.add("b")));
        check(ran.isEmpty(), "task.spawn does not run its function");
        check(a != b, "two spawns make two tasks");
        checkEquals(null, LoxOps.yield(1.0), "the main task yields nil");
        checkEquals(List.of("a", "b"), ran,
                    "the new tasks run at the yield, in spawn order");
        LoxError err = raised(() -> task("send", a, 1.0));
        check(err != null && "DeadTaskError".equals(kindOf(err)),
              "a send to an ended task raises DeadTaskError");
        checkEquals("Cannot send to a task that has ended.", messageOf(err),
                    "the DeadTaskError message");
        checkEquals("<task>", LoxOps.str(a), "str(task)");
        checkEquals("Task",
                    ((LoxCallable)globals.get("type")).call(new Object[] {a}),
                    "type(task)");
    }

    private static void checkReceiveSuspendsTheTask() {
        Object main = task("self");
        task("spawn", fn("sender", () -> task("send", main, "hello")));
        checkEquals("hello", task("receive"),
                    "an empty mailbox suspends the main task until a send");
    }

    private static void checkReceiveAcrossNative() {
        Object me = task("self");
        task("send", me, "ready");
        LoxOps.enterCallback();
        Object got;
        LoxError err;
        try {
            got = task("receive");
            err = raised(() -> task("receive"));
        } finally {
            LoxOps.exitCallback();
        }
        checkEquals("ready", got,
                    "a receive that finds a message works under a callback");
        check(err != null && "YieldAcrossNativeError".equals(kindOf(err)),
              "a receive that must suspend under a callback raises");
        checkEquals("Cannot receive across a native callback.", messageOf(err),
                    "the receive message differs from the yield message");
    }

    private static void checkEachTaskHasItsOwnCounters() {
        boolean[] sawHandler = new boolean[1];
        int[] sawFrames = new int[1];
        task("spawn", fn("probe", () -> {
            sawHandler[0] = LoxOps.isHandlerLive();
            sawFrames[0] = LoxClosure.frameCountValue();
        }));
        int framesBefore = LoxClosure.frameCountValue();
        LoxOps.enterHandler();
        try {
            LoxOps.yield(null);
            check(LoxOps.isHandlerLive(),
                  "the main task gets its own handler back");
            checkEquals(framesBefore, LoxClosure.frameCountValue(),
                        "the main task gets its own frame count back");
        } finally {
            LoxOps.exitHandler();
        }
        check(!sawHandler[0], "a task does not see another task's handler");
        checkEquals(1, sawFrames[0],
                    "a task counts from its own first frame");
    }

    private static void checkReceiveInCoroutineKeepsItRunning() {
        Object main = task("self");
        LoxCoroutine[] co = new LoxCoroutine[1];
        Object[] got = new Object[1];
        Object owner = task("spawn", fn("owner", () -> {
            co[0] = new LoxCoroutine(new LoxClosure("body", 0, NO_UPVALUES) {
                @Override
                protected Object invoke(Object self, Object[] a) {
                    return task("receive");
                }
            });
            got[0] = co[0].resume(NO_ARGS);
            task("send", main, "owner done");
        }));
        LoxOps.yield(null);
        checkEquals("running", co[0].statusName(),
                    "a coroutine that waits in receive keeps running");
        LoxError err = raised(() -> co[0].resume(NO_ARGS));
        check(err != null && "RunningCoroutineError".equals(kindOf(err)),
              "another task cannot resume it");
        task("send", owner, "go");
        checkEquals("owner done", task("receive"), "the owner task finishes");
        checkEquals("go", got[0], "the receive returns inside the coroutine");
        checkEquals("dead", co[0].statusName(), "and the coroutine returned");
    }

    private static LoxNative receiveNative() {
        return new LoxNative("receive", 0, args -> task("receive"));
    }

    // A coroutine whose entry is a native has no host-free frame to keep. A
    // receive below it must raise, and a throw out of the entry leaves the
    // coroutine dead (spec/04-semantics.md, task.receive step 3, co.resume
    // step 5).
    private static void checkReceiveInNativeCoroutineEntryRaises() {
        checkEquals(0.0, task("pending"), "the main mailbox starts empty");
        LoxCoroutine co = new LoxCoroutine(receiveNative());
        LoxError err = raised(() -> co.resume(NO_ARGS));
        check(err != null && "YieldAcrossNativeError".equals(kindOf(err)),
              "a receive below a native coroutine entry raises");
        checkEquals("Cannot receive across a native callback.", messageOf(err),
                    "the native-entry receive message");
        checkEquals("dead", co.statusName(),
                    "a throw out of a native entry leaves it dead");
    }

    // A receive that finds a message does not suspend, so the native entry
    // returns that message and the coroutine is dead.
    private static void checkReceiveInNativeCoroutineEntryWithMessage() {
        task("send", task("self"), "m");
        LoxCoroutine co = new LoxCoroutine(receiveNative());
        checkEquals("m", co.resume(NO_ARGS),
                    "a receive below a native entry returns a waiting message");
        checkEquals("dead", co.statusName(), "the native entry returned");
    }

    // A native entry that resumes another coroutine keeps its host frame on
    // the chain, so the inner receive raises and both coroutines are dead.
    private static void checkReceiveUnderANativeResumeEntryRaises() {
        LoxCoroutine inner = new LoxCoroutine(
            new LoxClosure("body", 0, NO_UPVALUES) {
                @Override
                protected Object invoke(Object self, Object[] a) {
                    return task("receive");
                }
            });
        LoxNative resume = new LoxNative("resume", -1, args -> inner.resume(args));
        LoxCoroutine outer = new LoxCoroutine(resume);
        LoxError err = raised(() -> outer.resume(NO_ARGS));
        check(err != null && "YieldAcrossNativeError".equals(kindOf(err)),
              "a receive below a native resume entry raises");
        checkEquals("dead", outer.statusName(),
                    "the outer native entry is dead after the throw");
        checkEquals("dead", inner.statusName(),
                    "the inner coroutine is dead after the throw");
    }

    private static void checkSpawnWorkerEntry() {
        LoxClosure nested = new LoxClosure("inner", 1, NO_UPVALUES) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return null;
            }
        };
        LoxError err = raised(() -> task("spawn_worker", nested, null));
        check(err != null && !err.isCatchable(),
              "an entry that no top-level fun declaration made is fatal");
        LoxError sendErr =
            raised(() -> task("spawn_worker", new Echo(NO_UPVALUES), new LoxMap()));
        check(sendErr != null && "SendError".equals(kindOf(sendErr)),
              "a start message that is not a message raises SendError");
    }

    // A function of a top-level fun declaration: the shape the emitter gives
    // one, with the public constructor a new worker calls.
    public static final class Echo extends LoxClosure {
        public Echo(Object[][] upvalues) { super("echo", 1, upvalues, true); }

        @Override
        protected Object invoke(Object self, Object[] a) {
            LoxEnum m = (LoxEnum)a[0];
            Object from = m.payload[0];
            LoxGlobals own = LoxRuntime.current();
            task("send", from, own != globals && own.isDefined("task"));
            task("send", from, task("self"));
            task("send", from, m);
            task("send", from, task("receive"));
            return null;
        }

        // The worker's own task module: Echo runs on the new worker.
        private static Object task(String member, Object... args) {
            LoxInstance module = (LoxInstance)LoxRuntime.current().get("task");
            return ((LoxCallable)module.fields.get(member)).call(args);
        }
    }

    private static void checkWorker() {
        boolean[] declared = new boolean[1];
        LoxRuntime.declarations = () -> declared[0] = true;
        LoxEnumCtor ask = new LoxEnumCtor(0, 2, "Ask", "Msg");
        Object start = ask.call(new Object[] {task("self"), "p"});
        Object w = task("spawn_worker", new Echo(NO_UPVALUES), start);
        checkEquals(true, task("receive"),
                    "a new worker has its own globals with the stdlib");
        check(declared[0], "a new worker runs the declarations first");
        check(task("receive") == w,
              "the handle a worker sends of itself is the spawned handle");
        check(task("receive") == start,
              "the start message comes back as the same value");
        Object sent = ask.call(new Object[] {1.0, "q"});
        task("send", w, sent);
        check(task("receive") == sent,
              "a message comes back from another worker as the same value");
        String kind = null;
        for (int i = 0; i < 10000 && kind == null; i++) {
            LoxError err = raised(() -> task("send", w, 0.0));
            if (err != null) {
                kind = kindOf(err);
            } else {
                try {
                    Thread.sleep(1);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }
        checkEquals("DeadTaskError", kind,
                    "a send to a task of an ended worker raises DeadTaskError");
    }
}
