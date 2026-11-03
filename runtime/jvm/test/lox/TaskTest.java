package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.channels.SocketChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

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

    public static void main(String[] args) throws IOException {
        globals = LoxRuntime.init();
        checkMessageRule();
        checkSendAndReceiveInOrder();
        checkSendError();
        checkSpawnOrderAndDeadTask();
        checkReceiveSuspendsTheTask();
        checkReceiveAcrossNative();
        checkReceiveWithin();
        checkReceiveWhere();
        checkReceiveWhereRules();
        checkEachTaskHasItsOwnCounters();
        checkReceiveInCoroutineKeepsItRunning();
        checkReceiveInNativeCoroutineEntryRaises();
        checkReceiveInNativeCoroutineEntryWithMessage();
        checkReceiveUnderANativeResumeEntryRaises();
        checkSleepParksOnlyTheTask();
        checkSleepUnderACallbackBlocksTheWorker();
        checkCloseWakesAParkedAccept();
        checkSendWakesAPollingWorker();
        checkReadyChannelNobodyWaitsOnDoesNotSpin();
        checkWritableChannelNobodyWaitsOnDoesNotSpin();
        checkFinishedConnectKeepsNoKey();
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

    private static LoxClosure want(java.util.function.Function<Object, Object> f) {
        return new LoxClosure("want", 1, NO_UPVALUES) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return f.apply(a[0]);
            }
        };
    }

    private static void checkReceiveWithin() {
        Object me = task("self");
        LoxError none = raised(() -> task("receive_within", 0.05));
        check(none != null && none.isCatchable() &&
                  "TimeoutError".equals(kindOf(none)),
              "a limit that passes raises a catchable TimeoutError");
        task("send", me, "now");
        checkEquals("now", task("receive_within", 0.05),
                    "a message in the mailbox is returned at once");
        task("spawn", fn("late", () -> {
            sleepSeconds(0.05);
            task("send", me, "later");
        }));
        long start = System.nanoTime();
        checkEquals("later", task("receive_within", 120.0),
                    "a message that arrives in time ends the wait");
        // The bound only catches a wait that the message did not end: the
        // limit is far above the time of a loaded machine.
        check(System.nanoTime() - start < 60_000_000_000L,
              "a message ends the wait of the limit");
        for (Object bad : new Object[] {0.0, -1.0, Double.NaN, "1", null}) {
            LoxError err = raised(() -> task("receive_within", bad));
            check(err != null && !err.isCatchable(),
                  "a limit that is not a Number above zero is fatal: " + bad);
        }
        LoxOps.enterCallback();
        try {
            LoxError err = raised(() -> task("receive_within", 0.05));
            check(err != null && "YieldAcrossNativeError".equals(kindOf(err)),
                  "an empty mailbox under a callback raises, with a limit too");
        } finally {
            LoxOps.exitCallback();
        }
    }

    private static void checkReceiveWhere() {
        Object me = task("self");
        for (Object m : new Object[] {1.0, "a", 2.0, "b"}) {
            task("send", me, m);
        }
        LoxClosure isText = want(m -> m instanceof String);
        checkEquals("a", task("receive_where", isText),
                    "the first message that matches is taken");
        checkEquals(3.0, task("pending"), "the other messages stay");
        checkEquals("b", task("receive_where", isText), "the next match");
        checkEquals(1.0, task("receive"), "unmatched messages keep their order");
        checkEquals(2.0, task("receive"), "and the last of them");
        task("spawn", fn("sender", () -> {
            task("send", me, 7.0);
            LoxOps.yield(null);
            task("send", me, "late");
        }));
        checkEquals("late", task("receive_where", isText),
                    "a wait tests each new message");
        checkEquals(7.0, task("receive"), "the rejected message stays");
        LoxError none = raised(() -> task("receive_where_within", isText, 0.05));
        check(none != null && "TimeoutError".equals(kindOf(none)),
              "no match within the limit raises TimeoutError");
        task("send", me, "x");
        checkEquals("x", task("receive_where_within", isText, 0.05),
                    "a match in the mailbox is taken");
    }

    private static void checkReceiveWhereRules() {
        Object me = task("self");
        task("send", me, "kept");
        LoxClosure boom = want(m -> {
            throw LoxOps.makeError("TestError", "boom");
        });
        LoxError err = raised(() -> task("receive_where", boom));
        check(err != null && "TestError".equals(kindOf(err)),
              "a throw in want reaches the caller");
        checkEquals(1.0, task("pending"), "the message stays after a throw");
        LoxClosure inner = want(m -> task("receive"));
        LoxError nested = raised(() -> task("receive_where", inner));
        check(nested != null && "YieldAcrossNativeError".equals(kindOf(nested)),
              "a receive in want raises, also when the mailbox has a message");
        LoxClosure sees = want(m -> task("pending").equals(1.0));
        checkEquals("kept", task("receive_where", sees),
                    "want reads task.pending");
        LoxError notFn = raised(() -> task("receive_where", "x"));
        check(notFn != null && !notFn.isCatchable(),
              "a want that is not a function is fatal");
    }

    private static void sleepSeconds(double seconds) {
        ((LoxCallable)globals.get("sleep")).call(new Object[] {seconds});
    }

    private static void checkSleepParksOnlyTheTask() {
        boolean[] ticked = {false};
        task("spawn", fn("ticker", () -> ticked[0] = true));
        long start = System.nanoTime();
        sleepSeconds(0.1);
        check(System.nanoTime() - start >= 90_000_000L,
              "sleep waits for its duration");
        check(ticked[0], "a task of the worker runs while another sleeps");
    }

    private static void checkSleepUnderACallbackBlocksTheWorker() {
        boolean[] ticked = {false};
        task("spawn", fn("ticker", () -> ticked[0] = true));
        LoxOps.enterCallback();
        try {
            sleepSeconds(0.05);
        } finally {
            LoxOps.exitCallback();
        }
        check(!ticked[0], "sleep under a callback blocks the worker");
        sleepSeconds(0.05);
        check(ticked[0], "the task runs once the sleep may suspend");
    }

    private static void checkCloseWakesAParkedAccept() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        String[] kind = {null};
        task("spawn", fn("acceptor", () -> {
            LoxError err = raised(() -> server.accept());
            kind[0] = err == null ? "none" : kindOf(err);
        }));
        sleepSeconds(0.05);
        check(kind[0] == null, "accept parks while no connection arrives");
        server.close();
        sleepSeconds(0.05);
        checkEquals("SocketClosedError", kind[0],
                    "a close raises the closed fault in the parked accept");
    }

    // The state of the worker that a probe task watches, set before it starts.
    private static volatile LoxWorker watched;
    private static volatile boolean sawPollingWorker;
    private static volatile boolean spun;
    private static volatile boolean spinCheck;

    // Bounds that only catch a hang; far above any loaded time.
    private static final long HANG_SECONDS = 60;
    // A worker that sleeps returns from select 0 times in these cases (40
    // runs, also under CPU load). One that spins on a ready channel passes
    // this limit at once.
    private static final long SPIN_RETURNS = 50;
    // Waiting this long for a spin cannot fail a correct run: it only gives a
    // broken run time to show the spin.
    private static final long SPIN_WAIT_MILLIS = 1000;

    private static boolean awaitPolling(LoxWorker w) {
        long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(HANG_SECONDS);
        while (!w.isPolling()) {
            if (System.nanoTime() - end > 0) {
                return false;
            }
            try {
                Thread.sleep(1);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return true;
    }

    // A function of a top-level fun declaration that runs on a second worker.
    // It waits until the watched worker sleeps in its poller, then sends the
    // start message (the main task) a message. With spinCheck set it first
    // checks that the watched worker does not spin.
    public static final class Prober extends LoxClosure {
        public Prober(Object[][] upvalues) { super("prober", 1, upvalues, true); }

        @Override
        protected Object invoke(Object self, Object[] a) {
            sawPollingWorker = awaitPolling(watched);
            if (spinCheck) {
                long base = watched.selectReturns();
                long end = System.nanoTime() +
                           TimeUnit.MILLISECONDS.toNanos(SPIN_WAIT_MILLIS);
                while (watched.selectReturns() - base <= SPIN_RETURNS &&
                       System.nanoTime() - end < 0) {
                    try {
                        Thread.sleep(1);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
                spun = watched.selectReturns() - base > SPIN_RETURNS;
            }
            LoxInstance module = (LoxInstance)LoxRuntime.current().get("task");
            ((LoxCallable)module.fields.get("send"))
                .call(new Object[] {a[0], "wake"});
            return null;
        }
    }

    // Catches a hang only. If the main task still waits after HANG_SECONDS,
    // it wakes the poller so that the task sees the message already in its
    // mailbox, and disarm() then reports the failure. A scheduler that lost a
    // wake cannot run the later cases, so disarm() ends the suite then.
    private static final class Watchdog {
        private final CountDownLatch done = new CountDownLatch(1);
        private volatile boolean fired;

        Watchdog(LoxWorker worker) {
            Thread t = new Thread(() -> {
                try {
                    if (!done.await(HANG_SECONDS, TimeUnit.SECONDS)) {
                        fired = true;
                        worker.wakePoller();
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }, "task-test-watchdog");
            t.setDaemon(true);
            t.start();
        }

        void disarm(String description) {
            done.countDown();
            check(!fired, description);
            if (fired) {
                System.exit(TestSupport.finish("TaskTest"));
            }
        }
    }

    private static LoxSocket[] connectedPair(LoxServer server) {
        LoxSocket client = LoxSocket.connect("127.0.0.1", (int)server.port());
        return new LoxSocket[] {client, (LoxSocket)server.accept()};
    }

    // Parks a task of the main worker in readline on a socket that gets no
    // data, so the worker's poller has a wait and no timer.
    private static LoxSocket[] parkReader(LoxServer server) {
        LoxSocket[] pair = connectedPair(server);
        task("spawn", fn("reader", () -> {
            raised(() -> pair[1].readline());
        }));
        LoxOps.yield(null);
        return pair;
    }

    // A send from another worker to a task of a worker that sleeps in its
    // poller must end that sleep (spec/04-semantics.md, task.send).
    private static void checkSendWakesAPollingWorker() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] idle = parkReader(server);
        watched = LoxWorker.current();
        spinCheck = false;
        sawPollingWorker = false;
        Watchdog dog = new Watchdog(watched);
        LoxRuntime.declarations = () -> {};
        task("spawn_worker", new Prober(NO_UPVALUES), task("self"));
        Object got = task("receive");
        dog.disarm("a send from another worker ends the sleep of a polling worker");
        checkEquals("wake", got, "the message from the other worker arrives");
        check(sawPollingWorker, "the main worker slept in its poller");
        closeAndReap(idle);
        server.close();
    }

    // Runs the spin check on the main worker: a second worker watches the
    // select returns of the main worker while the main task waits for its
    // message. `disarm` names the case in a hang report.
    private static void watchForSpin(String disarm, String spinMessage) {
        watched = LoxWorker.current();
        spinCheck = true;
        spun = false;
        sawPollingWorker = false;
        Watchdog dog = new Watchdog(watched);
        LoxRuntime.declarations = () -> {};
        task("spawn_worker", new Prober(NO_UPVALUES), task("self"));
        Object got = task("receive");
        dog.disarm(disarm);
        spinCheck = false;
        checkEquals("wake", got, "the message from the other worker arrives");
        check(sawPollingWorker, "the main worker slept in its poller");
        check(!spun, spinMessage);
    }

    // A channel that is ready and that no task waits on must not keep the
    // poller from sleeping: the interest of a drained key is reset.
    private static void checkReadyChannelNobodyWaitsOnDoesNotSpin() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] idle = parkReader(server);
        LoxSocket[] ended = parkReader(server);
        // The peer of the second pair reads end of stream, which stays ready
        // after its reader returned.
        ended[0].closeWrite();
        watchForSpin("a read-ready channel that no task waits on lets the poller sleep",
                     "a ready channel that no task waits on does not wake the poller");
        closeAndReap(idle);
        closeAndReap(ended);
        server.close();
    }

    // A socket is writable whenever its send buffer has room, so the write
    // interest of a drained key must be reset or the poller spins.
    private static void checkWritableChannelNobodyWaitsOnDoesNotSpin() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] idle = parkReader(server);
        LoxSocket[] pair = connectedPair(server);
        // Larger than the send and receive buffers of the pair together, so
        // the writer parks in OP_WRITE until the main task reads.
        final int size = 1 << 25;
        task("spawn", fn("writer", () -> {
            pair[0].write("x".repeat(size));
        }));
        LoxOps.yield(null);
        checkEquals(size, ((String)pair[1].readBytes(size)).length(),
                    "the reader gets every byte of the parked write");
        LoxOps.yield(null); // the writer returns
        watchForSpin("a writable channel that no task waits on lets the poller sleep",
                     "a writable channel that no task waits on does not wake the poller");
        closeAndReap(idle);
        closeAndReap(pair);
        server.close();
    }

    // The connect wait of a channel ends when the connection finishes. The
    // program may never close that Socket, so the selector must not keep it.
    // A connect to loopback finishes at once unless the accept queue of the
    // server is full: the kernel then drops the first SYN and the connect
    // waits for its retry, after the server took a connection.
    private static void checkFinishedConnectKeepsNoKey() throws IOException {
        // A select drops the cancelled keys of the earlier cases.
        sleepSeconds(0.01);
        checkEquals(0, LoxWorker.current().selectorKeyCount(),
                    "the selector starts with no key");
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        InetSocketAddress address =
            new InetSocketAddress("127.0.0.1", (int)server.port());
        List<SocketChannel> fillers = new ArrayList<>();
        int queued = 0;
        for (;;) {
            SocketChannel filler = SocketChannel.open();
            filler.configureBlocking(false);
            fillers.add(filler);
            if (!filler.connect(address) && !filler.finishConnect()) {
                break; // the accept queue is full
            }
            queued++;
        }
        final int accepts = queued;
        int[] keysWhileWaiting = {-1};
        task("spawn", fn("acceptor", () -> {
            keysWhileWaiting[0] = LoxWorker.current().selectorKeyCount();
            for (int i = 0; i < accepts; i++) {
                ((LoxSocket)server.accept()).close();
            }
        }));
        LoxSocket made = LoxSocket.connect("127.0.0.1", (int)server.port());
        checkEquals(1, keysWhileWaiting[0],
                    "the connect waited for its channel in the poller");
        LoxOps.yield(null);
        sleepSeconds(0.01);
        checkEquals(0, LoxWorker.current().selectorKeyCount(),
                    "the selector keeps no key of a connect that finished");
        made.close();
        for (SocketChannel filler : fillers) {
            filler.close();
        }
        server.close();
    }

    private static void closeAndReap(LoxSocket[] pair) {
        pair[1].close();
        pair[0].close();
        // The reader task of the closed socket raises and ends.
        LoxOps.yield(null);
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
