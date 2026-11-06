package lox;

import java.nio.channels.SelectableChannel;

/**
 * One side of a Socket or a Server (spec/04-semantics.md, The Task Machine,
 * Sides): the read side or the write side of a {@link LoxSocket}, or the
 * accept side of a {@link LoxServer}. A side holds at most one call. A call
 * holds its side from its first wait until it returns or raises, also while
 * its task is ready after the wait ended. A call that never waits as a task never holds
 * its side.
 *
 * <p>Only the running task of the worker that owns the value reads or writes
 * {@link #holder}. A Socket or a Server is not a message, so it never leaves
 * its worker, and a task switch orders the writes of one task before the reads
 * of the next.
 */
final class LoxSide {
    private final String name;
    // The task whose call holds the side, or null. Every call clears it when
    // it returns or raises (see leave), so a task that ended holds no side.
    private LoxTask holder;

    LoxSide(String name) { this.name = name; }

    /**
     * Starts a call on the side. The caller checks first that the value is
     * open: a closed side fails as a closed value does, also while a woken
     * call still holds it (rule 6). A held side raises BusyError at the call,
     * before the call reads, writes, or accepts anything, also when it could
     * return at once and also where the task cannot suspend (rules 2 and 5).
     */
    void enter(String method) {
        if (holder != null) {
            throw LoxOps.makeError("BusyError",
                                   "Cannot call '" + method + "': a call of " +
                                       "another task waits on the " + name +
                                       ".");
        }
        LoxWorker.beginWaitingCall();
    }

    /**
     * Waits for {@code channel} as {@link LoxWorker#waitChannel} does. A call
     * that waits as a task holds the side from here. A call that cannot suspend blocks
     * its worker, so no other task can find the side held, and it holds
     * nothing (rule 5).
     */
    void await(SelectableChannel channel, int interest) {
        if (!LoxWorker.blocked()) {
            holder = LoxWorker.runningTask();
        }
        LoxWorker.waitChannel(channel, interest);
    }

    /**
     * The call that {@link #enter} started returns or raises. No other call
     * can hold the side meanwhile: {@link #enter} let this call in only while
     * the side was free, and from then on a call of another task raises.
     */
    void leave() { holder = null; }

    /**
     * Ends the wait of the holder now, if it is waiting on {@code channel} for
     * {@code interest}: its task joins the back of the run queue at this call
     * (spec/04-semantics.md, The Task Machine, Sides, rule 4). A holder that is ready
     * already sees the new state of the value when it runs.
     */
    void endWait(SelectableChannel channel, int interest) {
        if (holder != null) {
            LoxWorker.endWait(channel, interest);
        }
    }
}
