package lox;

import java.io.IOException;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.SelectableChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.List;
import java.util.TreeSet;

/**
 * The wait set of one worker: the channels its parked tasks wait on and the
 * timers they wait for (spec/04-semantics.md, Scheduling). The worker creates
 * it on its first park, so a program that never parks pays nothing.
 *
 * <p>Every method except {@link #select} and {@link #wake} runs with
 * {@link LoxWorker#LOCK} held, on the thread that schedules the worker.
 * {@link #select} runs without the lock and touches only the selector, which
 * no other thread uses while the worker has no current task.
 */
final class LoxPoller {
    static final class Wait {
        final long seq;
        final LoxTask task;
        final SelectableChannel channel; // null for a timer
        final int interest;
        final long deadline;

        Wait(long seq, LoxTask task, SelectableChannel channel, int interest,
             long deadline) {
            this.seq = seq;
            this.task = task;
            this.channel = channel;
            this.interest = interest;
            this.deadline = deadline;
        }
    }

    private final Selector selector;
    // Written by the worker's own thread and read by tests on any thread.
    private volatile long selectReturns;
    private long nextSeq = 1;
    // In registration order.
    private final ArrayList<Wait> fdWaits = new ArrayList<>();
    // The deadline orders the timers; the registration number breaks a tie.
    private final TreeSet<Wait> timers = new TreeSet<>((a, b) -> {
        long d = a.deadline - b.deadline;
        return d != 0 ? (d < 0 ? -1 : 1) : Long.compare(a.seq, b.seq);
    });

    LoxPoller() {
        try {
            selector = Selector.open();
        } catch (IOException e) {
            throw new IllegalStateException("cannot open a selector", e);
        }
    }

    boolean hasWaits() { return !fdWaits.isEmpty() || !timers.isEmpty(); }

    /**
     * Registers a wait and returns its number. A call that waits again passes
     * the number of its first wait as {@code seq}, so its wait keeps its place
     * among the waits that began after it; 0 starts a new call.
     */
    long addFd(LoxTask task, SelectableChannel channel, int interest, long seq)
        throws ClosedChannelException {
        SelectionKey key = channel.keyFor(selector);
        if (key != null && !key.isValid()) {
            // A cancelled key stays registered until a select; registering
            // the channel again before then throws.
            selectNow();
            key = channel.keyFor(selector);
        }
        if (key == null) {
            channel.register(selector, interest);
        } else {
            key.interestOps(key.interestOps() | interest);
        }
        if (seq == 0) {
            seq = nextSeq++;
        }
        int at = fdWaits.size();
        while (at > 0 && fdWaits.get(at - 1).seq > seq) {
            at--;
        }
        fdWaits.add(at, new Wait(seq, task, channel, interest, 0));
        return seq;
    }

    void addTimer(LoxTask task, long deadlineNanos) {
        timers.add(new Wait(nextSeq++, task, null, 0, deadlineNanos));
    }

    /** Removes the timer of {@code task}, if it has one. */
    void cancelTimer(LoxTask task) {
        timers.removeIf(w -> w.task == task);
    }

    /**
     * Removes every wait on {@code channel} and appends the tasks, in
     * registration order. A channel must leave the poller before it closes.
     */
    void cancelChannel(SelectableChannel channel, List<LoxTask> out) {
        for (Iterator<Wait> it = fdWaits.iterator(); it.hasNext();) {
            Wait w = it.next();
            if (w.channel == channel) {
                out.add(w.task);
                it.remove();
            }
        }
        SelectionKey key = channel.keyFor(selector);
        if (key != null) {
            key.cancel();
        }
    }

    /** Looks once, never sleeps. */
    void selectNow() {
        try {
            selector.selectNow();
        } catch (IOException e) {
            throw new IllegalStateException("select failed", e);
        }
    }

    /**
     * Sleeps until a channel is ready, a timer is due, or {@link #wake} was
     * called. A wake before the call makes it return at once.
     */
    void select() {
        long timeout = 0; // 0 sleeps without a limit
        if (!timers.isEmpty()) {
            long left = timers.first().deadline - System.nanoTime();
            if (left <= 0) {
                return;
            }
            timeout = Math.max(1, (left + 999_999) / 1_000_000);
        }
        try {
            selector.select(timeout);
        } catch (IOException e) {
            throw new IllegalStateException("select failed", e);
        }
        selectReturns++;
    }

    /**
     * How many times {@link #select} slept and returned. A test reads it to
     * tell a worker that sleeps from one that spins on a ready channel.
     */
    long selectReturns() { return selectReturns; }

    void wake() { selector.wakeup(); }

    /** The channels the selector still holds, cancelled ones included. */
    int keyCount() { return selector.keys().size(); }

    /**
     * Appends the tasks whose wait is over and removes their waits: the
     * channel waits in registration order, then the timers in deadline order.
     */
    void drainReady(List<LoxTask> out) {
        if (!selector.selectedKeys().isEmpty()) {
            // A key keeps the ready set of its last selection, so only the
            // keys of this round count.
            IdentityHashMap<SelectableChannel, Integer> ready =
                new IdentityHashMap<>();
            for (SelectionKey key : selector.selectedKeys()) {
                if (key.isValid()) {
                    ready.put(key.channel(), key.readyOps());
                }
            }
            selector.selectedKeys().clear();
            ArrayList<Wait> kept = new ArrayList<>(fdWaits.size());
            for (Wait w : fdWaits) {
                Integer ops = ready.get(w.channel);
                if (ops != null && (ops & w.interest) != 0) {
                    out.add(w.task);
                } else {
                    kept.add(w);
                }
            }
            fdWaits.clear();
            fdWaits.addAll(kept);
            boolean cancelled = false;
            for (SelectableChannel channel : ready.keySet()) {
                int interest = 0;
                for (Wait w : fdWaits) {
                    if (w.channel == channel) {
                        interest |= w.interest;
                    }
                }
                SelectionKey key = channel.keyFor(selector);
                if (interest == 0) {
                    // No call waits on the channel, and the program may never
                    // close it. The selector must not keep it.
                    key.cancel();
                    cancelled = true;
                } else {
                    key.interestOps(interest);
                }
            }
            if (cancelled && fdWaits.isEmpty()) {
                // No select follows while nothing waits, and the selector
                // holds a cancelled key until one does.
                selectNow();
            }
        }
        long now = System.nanoTime();
        while (!timers.isEmpty() && timers.first().deadline - now <= 0) {
            out.add(timers.pollFirst().task);
        }
    }
}
