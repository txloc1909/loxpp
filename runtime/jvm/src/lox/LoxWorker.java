package lox;

import java.io.IOException;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.SelectableChannel;
import java.nio.channels.Selector;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;

/**
 * One worker (spec/04-semantics.md, Workers and Tasks): its own globals, its
 * own runtime counters, and its own run queue of tasks. Workers share no
 * mutable state; a value passes between them only as a message, by
 * reference.
 *
 * <p><b>Threads.</b> The main task runs on the thread that runs the
 * generated {@code main}. The first task of every other worker runs on a
 * platform thread of its own. A task of {@code task.spawn} runs on a
 * virtual thread, as a coroutine does. The default carrier pool has one
 * carrier for each processor, and a CPU-bound virtual task keeps its
 * carrier, so the pool is raised to the JDK maximum (see the static block
 * below): a suspended task holds no carrier, and a task that computes does
 * not stop the ready task of another worker. Only one task of a worker
 * runs at a time: a task that suspends gives control to the head of the
 * run queue directly ({@link #switchTo}), and no scheduler thread exists.
 *
 * <p><b>Parking.</b> A call that waits for the network or for time suspends
 * its task in the {@link LoxPoller} of the worker, and the other tasks run
 * (spec/04-semantics.md, Suspend Points). The thread that finds the run
 * queue empty polls for the end of a wait; no scheduler thread exists. A
 * call that has a native callback on its chain cannot suspend, so it waits
 * in place and blocks the worker.
 *
 * <p><b>Lock.</b> {@link #LOCK} guards the run queues, the task states, the
 * mailboxes, and the idle counts of every worker. One lock keeps the
 * deadlock check simple: it reads the state of every worker in one step.
 */
final class LoxWorker {
    // The JDK reads both properties once, when it starts the first virtual
    // thread, so this block must run before that point. The generated
    // LoxMain calls LoxRuntime.init(), which reads LoxWorker.MAIN, so this
    // class initializes and runs the block before the script. The first
    // virtual thread starts later, in switchTo or LoxCoroutine.resume.
    //
    // The virtual-thread scheduler has one carrier for each processor by
    // default. A CPU-bound virtual task keeps its carrier, so a program with
    // more CPU-bound tasks than processors can stop a ready task on another
    // worker (spec/04-semantics.md, Suspend Points). Raise the pool to the
    // largest value a ForkJoinPool accepts. The pool creates a carrier only
    // for a mounted task, so a suspended task still costs no host thread,
    // and the value is a maximum, not a reservation. A caller can set either
    // property to override the value.
    static {
        if (System.getProperty(
                "jdk.virtualThreadScheduler.parallelism") == null) {
            System.setProperty(
                "jdk.virtualThreadScheduler.parallelism", "32767");
        }
        if (System.getProperty(
                "jdk.virtualThreadScheduler.maxPoolSize") == null) {
            System.setProperty(
                "jdk.virtualThreadScheduler.maxPoolSize", "32767");
        }
    }

    static final ReentrantLock LOCK = new ReentrantLock();

    // Guarded by LOCK. A worker is live from its spawn until its last task
    // ends; it is idle while its run queue is empty and its current task
    // waits in receive.
    private static int liveWorkers = 1;
    private static int idleWorkers = 0;
    private static int nextWorkerNumber = 1;

    static final LoxWorker MAIN = new LoxWorker(0);

    // Set once, by the first task.spawn_worker. Not volatile: a thread that
    // still reads false is a thread of the main worker (every thread of
    // another worker starts after the write, and Thread.start orders it), and
    // for such a thread MAIN is the right answer.
    private static boolean several;

    private static final ThreadLocal<LoxWorker> BOUND =
        ThreadLocal.withInitial(() -> MAIN);

    static LoxWorker current() { return several ? BOUND.get() : MAIN; }

    /** Every thread of a worker other than the main worker calls this first. */
    static void bind(LoxWorker worker) {
        if (worker != MAIN) {
            BOUND.set(worker);
        }
    }

    final int number;
    LoxGlobals globals;

    // The runtime counters that native keeps in its Runtime, one Runtime per
    // worker. LoxClosure, LoxOps, and LoxCoroutine read and write them.
    int frameCount = 1;
    boolean unwindingStackOverflow;
    LoxError overflowInFlight;
    int handlerDepth;
    int stringifyDepth;

    // Guarded by LOCK.
    private final ArrayDeque<LoxTask> runQueue = new ArrayDeque<>();
    private final Condition work = LOCK.newCondition();
    private boolean idle;
    private int liveTasks = 1;
    // Null until the first park. `inPoll` is true while a thread of this
    // worker sleeps in poller.select() with LOCK released.
    private LoxPoller poller;
    private boolean inPoll;

    // The task that runs now. Written under LOCK by the task that gives
    // control, before its thread wakes this one.
    LoxTask current;

    private LoxWorker(int number) {
        this.number = number;
        LoxTask first = new LoxTask(this);
        first.state = LoxTask.State.RUNNING;
        first.started = true;
        current = first;
    }

    // ------------------------------------------------------------------
    // The task module (spec/05-stdlib.md, task)
    // ------------------------------------------------------------------

    /** task.spawn: the new task joins the back of the run queue. */
    LoxTask spawn(Object callee) {
        LoxTask task = new LoxTask(this);
        task.body = () -> runSpawned(task, callee);
        LOCK.lock();
        try {
            liveTasks++;
            runQueue.addLast(task);
        } finally {
            LOCK.unlock();
        }
        return task;
    }

    /**
     * task.send. The caller has checked that {@code msg} is a message. The
     * end of a task and a send to it take the same lock, so a send that
     * starts after the end always raises.
     */
    static void send(LoxTask target, Object msg) {
        LOCK.lock();
        try {
            if (target.ended) {
                throw LoxOps.makeError("DeadTaskError",
                                       "Cannot send to a task that has ended.");
            }
            target.deliver(msg);
            if (target.state == LoxTask.State.WAITING) {
                target.worker.makeReady(target);
            }
        } finally {
            LOCK.unlock();
        }
    }

    /**
     * task.receive. {@code canSuspend} is false while a native callback is on
     * the call chain of the task; then only a receive that finds a message
     * succeeds.
     */
    Object receive(boolean canSuspend) {
        LoxTask self = current;
        LOCK.lock();
        try {
            while (!self.hasMessage()) {
                if (!canSuspend) {
                    throw LoxOps.makeError(
                        "YieldAcrossNativeError",
                        "Cannot receive across a native callback.");
                }
                suspend(self, LoxTask.State.WAITING);
            }
            return self.takeMessage();
        } finally {
            LOCK.unlock();
        }
    }

    int pending() {
        LOCK.lock();
        try {
            return current.pending();
        } finally {
            LOCK.unlock();
        }
    }

    /** A yield outside every coroutine of the task: back of the run queue. */
    void yieldTask() {
        LOCK.lock();
        try {
            suspend(current, LoxTask.State.READY);
        } finally {
            LOCK.unlock();
        }
    }

    /**
     * task.spawn_worker. The caller has checked that {@code entry} is a
     * function of a top-level fun declaration and that {@code msg} is a
     * message.
     */
    static LoxTask spawnWorker(LoxClosure entry, Object msg) {
        LoxWorker worker;
        LOCK.lock();
        try {
            worker = new LoxWorker(nextWorkerNumber++);
            liveWorkers++;
            several = true;
        } finally {
            LOCK.unlock();
        }
        LoxTask task = worker.current;
        Class<?> entryClass = entry.getClass();
        Thread thread = new Thread(() -> worker.runFirstTask(entryClass, msg),
                                   "lox-worker-" + worker.number);
        // The program ends when the main task ends, with every other task
        // abandoned (spec/04-semantics.md, Ending): no worker thread keeps
        // the JVM alive.
        thread.setDaemon(true);
        thread.start();
        return task;
    }

    // ------------------------------------------------------------------
    // Task bodies
    // ------------------------------------------------------------------

    private void runSpawned(LoxTask task, Object callee) {
        bind(this);
        // A native entry is a host frame. The task's own frames cannot be
        // captured through it, so a task.receive below it must raise
        // YieldAcrossNativeError (spec/04-semantics.md, task.receive step 3).
        // A coroutine this entry resumes still runs on its own thread, where
        // this count is not visible, so a legal yield there is unaffected.
        boolean nativeEntry = callee instanceof LoxNative;
        if (nativeEntry) {
            LoxOps.enterCallback();
        }
        try {
            LoxOps.call(callee, new Object[0]);
        } catch (Throwable fault) {
            LoxRuntime.endWithFault(fault);
        } finally {
            if (nativeEntry) {
                LoxOps.exitCallback();
            }
        }
        endTask(task);
    }

    // spec/04-semantics.md, task.spawn_worker, steps 3 and 4: the
    // declarations of the program, then the entry, both in the new task.
    private void runFirstTask(Class<?> entryClass, Object msg) {
        bind(this);
        LoxTask task = current;
        try {
            globals = LoxRuntime.newGlobals();
            LoxRuntime.runDeclarations();
            // The declarations ran as the script frame; the entry is the
            // first frame of the task, as native's scheduler runs it.
            frameCount = 0;
            LoxClosure fn = (LoxClosure)entryClass
                                .getConstructor(Object[][].class)
                                .newInstance((Object)new Object[0][]);
            LoxOps.call(fn, new Object[] {msg});
        } catch (Throwable fault) {
            LoxRuntime.endWithFault(fault);
        }
        endTask(task);
    }

    // ------------------------------------------------------------------
    // Scheduling (spec/04-semantics.md, Scheduling)
    // ------------------------------------------------------------------

    /** True while a thread of this worker sleeps in the poller. */
    boolean isPolling() {
        LOCK.lock();
        try {
            return inPoll;
        } finally {
            LOCK.unlock();
        }
    }

    /** {@link LoxPoller#selectReturns} of this worker; 0 before its first park. */
    long selectReturns() {
        LOCK.lock();
        try {
            return poller == null ? 0 : poller.selectReturns();
        } finally {
            LOCK.unlock();
        }
    }

    /** {@link LoxPoller#keyCount} of this worker; 0 before its first park. */
    int selectorKeyCount() {
        LOCK.lock();
        try {
            return poller == null ? 0 : poller.keyCount();
        } finally {
            LOCK.unlock();
        }
    }

    /** Ends a sleep in the poller without making any task ready. */
    void wakePoller() {
        LOCK.lock();
        try {
            if (poller != null) {
                poller.wake();
            }
        } finally {
            LOCK.unlock();
        }
    }

    // Caller holds LOCK.
    private void makeReady(LoxTask task) {
        task.state = LoxTask.State.READY;
        runQueue.addLast(task);
        if (inPoll) {
            poller.wake();
        }
        if (idle) {
            // In the same step as the append, so that the deadlock check
            // never counts a worker idle that has a task to run.
            idle = false;
            idleWorkers--;
            work.signal();
        }
    }

    // Caller holds LOCK, on the thread of `self`, the current task. Returns
    // with LOCK held when `self` runs again.
    private void suspend(LoxTask self, LoxTask.State state) {
        self.state = state;
        if (state == LoxTask.State.READY) {
            runQueue.addLast(self);
        }
        self.savedFrameCount = frameCount;
        self.savedHandlerDepth = handlerDepth;
        LoxTask next = takeNext();
        if (next == self) {
            makeCurrent(self);
            return;
        }
        switchTo(next);
        try {
            self.wake.acquireUninterruptibly();
        } finally {
            LOCK.lock();
        }
    }

    // On the thread of the task that ended, which then exits. Nothing below
    // throws, so LOCK needs no try block: switchTo releases it.
    private void endTask(LoxTask task) {
        LOCK.lock();
        // spec/04-semantics.md, Ending: the messages still in the mailbox
        // are discarded.
        task.state = LoxTask.State.DEAD;
        task.ended = true;
        task.discardMessages();
        liveTasks--;
        if (liveTasks == 0) {
            // Only a worker other than the main worker gets here: the
            // program ends when the main task ends.
            liveWorkers--;
            checkDeadlock();
            LOCK.unlock();
            return;
        }
        switchTo(takeNext());
    }

    // Caller holds LOCK. Waits while the run queue is empty. A task whose
    // wait is over joins the back of the run queue before the next task is
    // taken, so a ready task cannot starve it.
    private LoxTask takeNext() {
        for (;;) {
            if (poller != null && poller.hasWaits()) {
                poller.selectNow();
                ArrayList<LoxTask> woken = new ArrayList<>();
                poller.drainReady(woken);
                for (LoxTask t : woken) {
                    makeReady(t);
                }
            }
            if (!runQueue.isEmpty()) {
                return runQueue.pollFirst();
            }
            if (poller != null && poller.hasWaits()) {
                // A worker with a wait registered can still make progress,
                // so it is not idle for the deadlock count.
                inPoll = true;
                LOCK.unlock();
                try {
                    poller.select();
                } finally {
                    LOCK.lock();
                    inPoll = false;
                }
                continue;
            }
            if (!idle) {
                idle = true;
                idleWorkers++;
            }
            checkDeadlock();
            work.awaitUninterruptibly();
        }
    }

    // ------------------------------------------------------------------
    // Calls that wait for a channel or for time
    // ------------------------------------------------------------------

    /**
     * Returns when {@code channel} may be ready for {@code interest} (a
     * {@link java.nio.channels.SelectionKey} operation), or when it closed.
     * The caller tries its operation again, and checks that it is open. The
     * task parks if it can suspend; otherwise the worker waits in place.
     */
    static void waitChannel(SelectableChannel channel, int interest) {
        waitChannel(channel, interest, false, 0);
    }

    /**
     * As {@link #waitChannel(SelectableChannel, int)}, and also returns when
     * {@link System#nanoTime} reaches {@code deadlineNanos} if {@code timed}.
     * The caller checks the time after it returns.
     */
    static void waitChannel(SelectableChannel channel, int interest,
                            boolean timed, long deadlineNanos) {
        if (LoxOps.callbackDepthOfChain() == 0) {
            current().parkOnChannel(channel, interest, timed, deadlineNanos);
            return;
        }
        try (Selector own = Selector.open()) {
            channel.register(own, interest);
            if (timed) {
                long left = deadlineNanos - System.nanoTime();
                if (left > 0) {
                    own.select(Math.max(1, (left + 999_999) / 1_000_000));
                }
            } else {
                own.select();
            }
        } catch (IOException e) {
            throw new IllegalStateException("cannot wait for a channel", e);
        }
    }

    /**
     * The running task starts a call that can wait for a channel. All waits
     * of one call share one place in the order of the waits of the worker, so
     * every such call starts here.
     */
    static void beginWaitingCall() {
        LoxTask task = current().current;
        if (task != null) {
            task.waitSeq = 0;
        }
    }

    /** sleep: the task waits for {@code nanos} and others of its worker run. */
    static void sleepNanos(long nanos) {
        // Capped so that the deadline cannot overflow nanoTime's range.
        long span = Math.min(nanos, 1L << 60);
        if (LoxOps.callbackDepthOfChain() == 0) {
            current().parkUntil(System.nanoTime() + span);
            return;
        }
        long end = System.nanoTime() + span;
        for (long left = span; left > 0; left = end - System.nanoTime()) {
            try {
                TimeUnit.NANOSECONDS.sleep(left);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    /**
     * A close wakes every task parked on {@code channel}, in the order of
     * their calls. Each joins the back of the run queue now. Call it before
     * the channel closes.
     */
    static void wakeChannel(SelectableChannel channel) {
        LoxWorker worker = current();
        LOCK.lock();
        try {
            if (worker.poller == null) {
                return;
            }
            ArrayList<LoxTask> woken = new ArrayList<>();
            worker.poller.cancelChannel(channel, woken);
            for (LoxTask t : woken) {
                worker.makeReady(t);
            }
        } finally {
            LOCK.unlock();
        }
    }

    private void parkOnChannel(SelectableChannel channel, int interest,
                               boolean timed, long deadlineNanos) {
        LoxTask self = current;
        LOCK.lock();
        try {
            self.waitSeq = ensurePoller().addFd(self, channel, interest,
                                                self.waitSeq, timed,
                                                deadlineNanos);
            suspend(self, LoxTask.State.PARKED);
        } catch (ClosedChannelException e) {
            // The channel closed before the wait began; the caller sees it.
        } finally {
            LOCK.unlock();
        }
    }

    private void parkUntil(long deadline) {
        LoxTask self = current;
        LOCK.lock();
        try {
            ensurePoller().addTimer(self, deadline);
            suspend(self, LoxTask.State.PARKED);
        } finally {
            LOCK.unlock();
        }
    }

    // Caller holds LOCK.
    private LoxPoller ensurePoller() {
        if (poller == null) {
            poller = new LoxPoller();
        }
        return poller;
    }

    // Caller holds LOCK, and stops running Lox code after this call. Hands
    // the worker to `next`; releases LOCK.
    private void switchTo(LoxTask next) {
        makeCurrent(next);
        boolean start = !next.started;
        next.started = true;
        LOCK.unlock();
        if (start) {
            Thread.ofVirtual().name("lox-task").start(next.body);
        } else {
            next.wake.release();
        }
    }

    // Caller holds LOCK. Each task has its own frame count and handler
    // depth, as each native task resumes from an empty frame stack.
    private void makeCurrent(LoxTask task) {
        current = task;
        task.state = LoxTask.State.RUNNING;
        frameCount = task.savedFrameCount;
        handlerDepth = task.savedHandlerDepth;
    }

    // Caller holds LOCK. spec/04-semantics.md, Deadlock: no task on any
    // worker can run again. A worker in a blocking call is not idle, and a
    // send delivers at once, so no message is on its way.
    private static void checkDeadlock() {
        if (idleWorkers < liveWorkers) {
            return;
        }
        LoxRuntime.endWithDeadlock();
    }
}
