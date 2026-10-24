package lox;

import java.util.ArrayDeque;
import java.util.concurrent.Semaphore;

/**
 * A task (spec/04-semantics.md, Workers and Tasks) and the Task value that
 * names it (spec/03-types.md, Task). A task never moves to another worker.
 *
 * <p>Every field that the scheduler changes is guarded by
 * {@link LoxWorker#LOCK}: a send from another worker reads {@link #ended}
 * and appends to {@link #mailbox} under that lock.
 */
public final class LoxTask {
    enum State {
        READY,   // in the run queue of its worker, or not started yet
        RUNNING, // the current task of its worker
        WAITING, // in task.receive() on an empty mailbox
        DEAD,
    }

    // ArrayDeque rejects null, and nil is a message.
    private static final Object NIL = new Object();

    final LoxWorker worker;

    State state = State.READY;
    boolean ended;
    // True once a thread runs this task. The main task and the first task
    // of a worker start on the thread that makes them.
    boolean started;
    // What the thread of a task.spawn task runs; null for a task that
    // starts on the thread that makes it.
    Runnable body;
    private final ArrayDeque<Object> mailbox = new ArrayDeque<>();

    // The frame count and the handler depth of this task while another
    // task of the worker runs (see LoxWorker.switchTo). A task starts below
    // its first call, as native's scheduler resumes each task from an empty
    // frame stack.
    int savedFrameCount;
    int savedHandlerDepth;

    // A thread of this task waits here while the task is suspended. Only
    // one thread of a task runs at a time, so one permit wakes the right
    // one: the task's own thread, or the thread of a coroutine it resumed.
    final Semaphore wake = new Semaphore(0);

    LoxTask(LoxWorker worker) { this.worker = worker; }

    /**
     * The message rule of spec/03-types.md, Messages. An enum value carries
     * the result in its own bit, fixed when it was made, so the check never
     * walks the fields.
     */
    static boolean isMessage(Object v) {
        if (v == null || v instanceof Boolean || v instanceof Double ||
            v instanceof Long || v instanceof String ||
            v instanceof LoxTask) {
            return true;
        }
        return v instanceof LoxEnum && ((LoxEnum)v).message;
    }

    void deliver(Object msg) { mailbox.addLast(msg == null ? NIL : msg); }

    boolean hasMessage() { return !mailbox.isEmpty(); }

    int pending() { return mailbox.size(); }

    Object takeMessage() {
        Object m = mailbox.pollFirst();
        return m == NIL ? null : m;
    }

    void discardMessages() { mailbox.clear(); }
}
