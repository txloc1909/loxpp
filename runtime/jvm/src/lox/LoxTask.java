package lox;

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
        PARKED,  // in a call that waits for a channel or for time
        DEAD,
    }

    // The mailbox uses null for an empty place, and nil is a message.
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
    // A ring buffer, so that receive_where can read and remove a message by
    // its place. The head and the end stay O(1), as for a deque.
    private Object[] mailbox = new Object[8];
    private int mailboxHead;
    private int mailboxSize;
    // True while the task waits in a receive_* call that has a limit: a send
    // must take its timer away (see LoxWorker.send).
    boolean limitedWait;

    // The frame count and the handler depth of this task while another
    // task of the worker runs (see LoxWorker.switchTo). A task starts below
    // its first call, as native's scheduler resumes each task from an empty
    // frame stack.
    // The number of the first wait of the call that the task is in (see
    // LoxPoller.addFd); 0 before the first wait of a call.
    long waitSeq;

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

    void deliver(Object msg) {
        if (mailboxSize == mailbox.length) {
            Object[] bigger = new Object[mailbox.length * 2];
            for (int i = 0; i < mailboxSize; i++) {
                bigger[i] = mailbox[(mailboxHead + i) % mailbox.length];
            }
            mailbox = bigger;
            mailboxHead = 0;
        }
        mailbox[(mailboxHead + mailboxSize) % mailbox.length] =
            msg == null ? NIL : msg;
        mailboxSize++;
    }

    boolean hasMessage() { return mailboxSize > 0; }

    int pending() { return mailboxSize; }

    /** The message at {@code at}, 0 being the head. */
    Object peekAt(int at) {
        Object m = mailbox[(mailboxHead + at) % mailbox.length];
        return m == NIL ? null : m;
    }

    /** Removes the message at {@code at} and keeps the order of the rest. */
    Object removeAt(int at) {
        Object m = peekAt(at);
        for (int i = at; i > 0; i--) {
            mailbox[(mailboxHead + i) % mailbox.length] =
                mailbox[(mailboxHead + i - 1) % mailbox.length];
        }
        mailbox[mailboxHead] = null;
        mailboxHead = (mailboxHead + 1) % mailbox.length;
        mailboxSize--;
        return m;
    }

    Object takeMessage() { return removeAt(0); }

    void discardMessages() {
        mailbox = new Object[8];
        mailboxHead = 0;
        mailboxSize = 0;
    }
}
