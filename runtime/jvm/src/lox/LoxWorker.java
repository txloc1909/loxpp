package lox;

import java.util.ArrayDeque;
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
 * platform thread of its own, and every task of {@code task.spawn} and
 * every coroutine runs on a platform thread of its own too. The
 * virtual-thread carrier pool has one carrier for each processor, so a
 * CPU-bound virtual task could stop the ready task of another worker, and
 * the spec says that only the tasks of the worker of a computing task
 * stop (spec/04-semantics.md, Suspend Points). Only one task of a worker
 * runs at a time: a task that suspends gives control to the head of the
 * run queue directly ({@link #switchTo}), and no scheduler thread exists.
 *
 * <p><b>Lock.</b> {@link #LOCK} guards the run queues, the task states, the
 * mailboxes, and the idle counts of every worker. One lock keeps the
 * deadlock check simple: it reads the state of every worker in one step.
 */
final class LoxWorker {
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
        try {
            LoxOps.call(callee, new Object[0]);
        } catch (Throwable fault) {
            LoxRuntime.endWithFault(fault);
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

    // Caller holds LOCK.
    private void makeReady(LoxTask task) {
        task.state = LoxTask.State.READY;
        runQueue.addLast(task);
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

    // Caller holds LOCK. Waits while the run queue is empty.
    private LoxTask takeNext() {
        while (runQueue.isEmpty()) {
            if (!idle) {
                idle = true;
                idleWorkers++;
            }
            checkDeadlock();
            work.awaitUninterruptibly();
        }
        return runQueue.pollFirst();
    }

    // Caller holds LOCK, and stops running Lox code after this call. Hands
    // the worker to `next`; releases LOCK.
    private void switchTo(LoxTask next) {
        makeCurrent(next);
        boolean start = !next.started;
        next.started = true;
        LOCK.unlock();
        if (start) {
            Thread.ofPlatform().daemon().name("lox-task").start(next.body);
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
