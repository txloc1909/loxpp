package lox;

import java.util.concurrent.SynchronousQueue;

/**
 * A suspendable Lox++ computation (spec/03-types.md, §Coroutine). Created by
 * {@code coroutine.create(fn)}; runs one function at a time and can pause at a
 * {@code yield}.
 *
 * <p>The JVM backend has no stack-copying primitive, so a coroutine is a
 * virtual thread: the whole Lox call chain stays live on that thread's own
 * (heap-mounted) stack while the coroutine is suspended, and no frame has
 * to be frozen or rebased. The carrier pool of {@link LoxWorker} is raised
 * from the processor count, so a CPU-bound coroutine does not stop a ready
 * task on another worker (spec/04-semantics.md, Suspend Points). Only the
 * handoff between the resumer and the coroutine crosses threads.
 *
 * <p><b>Handoff.</b> Two zero-capacity queues rendezvous each turn:
 * {@code toCoroutine} carries the resumer's next value into the coroutine, and
 * {@code fromCoroutine} carries the coroutine's outcome out to the resumer. A
 * single {@link java.util.concurrent.Exchanger} cannot express this: a yield
 * must both deliver the yielded value to the <em>current</em> resumer and then
 * block for a value that the <em>next</em> resume call has not produced yet —
 * two rendezvous. Splitting the directions makes the "who blocks first" order
 * unambiguous and deadlock-free.
 */
public final class LoxCoroutine {
    public enum State {
        SUSPENDED, // not started, or yielded; resumable
        RUNNING,   // the coroutine currently executing
        NORMAL,    // resumed another coroutine and awaits its yield/return
        DEAD,      // the function returned, or a throw left the coroutine
    }

    // The coroutine executing on this thread, if any. Tells `yield` whether
    // it suspends a coroutine or the task, and lets a nested resume mark its
    // own resumer NORMAL. Plain ThreadLocal (not inheritable): a coroutine's virtual thread
    // sets its own value in run(); resuming another coroutine must not leak the
    // child's identity onto the resumer.
    private static final ThreadLocal<LoxCoroutine> CURRENT = new ThreadLocal<>();

    static LoxCoroutine currentOrNull() { return CURRENT.get(); }

    private final Object callee; // LoxClosure, LoxBoundMethod, or LoxNative (coroutine.create)
    // A coroutine is not a message, so it runs only on the worker that made
    // it; its thread binds to that worker.
    private final LoxWorker worker = LoxWorker.current();
    // The callback depth of the resumer's call chain at the last resume
    // (LoxOps.callbackDepthOfChain): a native callback below the resume is
    // on this coroutine's chain too.
    private int callbackBelow;
    private volatile State state = State.SUSPENDED;
    private boolean started;
    private Object[] firstArgs;

    // The frame budget and the handler-liveness count are one count each per
    // worker, like native's m_frameCount and m_handlerStack.size(): a
    // resumed coroutine's frames and handler records sit above the resumer's.
    // These hold the resumer's value and this coroutine's own contribution, so
    // the coroutine can hide its slice while suspended and re-add it on resume.
    private int frameBase;
    private int frameOwn;
    private int handlerBase;
    private int handlerOwn;

    private final SynchronousQueue<Object> toCoroutine = new SynchronousQueue<>();
    private final SynchronousQueue<Object> fromCoroutine =
        new SynchronousQueue<>();

    // A value sent into the coroutine. Wrapped because SynchronousQueue
    // rejects null, while a resume value of nil (null) is legal.
    private static final class Resumed {
        final Object value;
        Resumed(Object value) { this.value = value; }
    }
    // Outcome messages the coroutine thread sends its resumer.
    private static final class Yielded {
        final Object value;
        Yielded(Object value) { this.value = value; }
    }
    private static final class Returned {
        final Object value;
        Returned(Object value) { this.value = value; }
    }
    private static final class Thrown {
        final Throwable error;
        Thrown(Throwable error) { this.error = error; }
    }

    public LoxCoroutine(Object callee) { this.callee = callee; }

    public State state() { return state; }

    public String statusName() {
        switch (state) {
        case RUNNING:
            return "running";
        case NORMAL:
            return "normal";
        case DEAD:
            return "dead";
        case SUSPENDED:
        default:
            return "suspended";
        }
    }

    /** Bound method value for GET_PROPERTY, mirroring LoxMap/LoxFile. */
    LoxCallable getMethod(String name) {
        switch (name) {
        case "resume":
            return new LoxNative("resume", -1, args -> resume(args), this);
        case "status":
            return new LoxNative("status", 0, args -> statusName(), this);
        default:
            return null;
        }
    }

    /**
     * Starts or continues the coroutine and returns the value it yields or
     * returns; a throw out of its function is rethrown here, in the resumer's
     * context, where it is catchable like any other throw.
     */
    Object resume(Object[] args) {
        if (state == State.DEAD) {
            throw LoxOps.makeError("DeadCoroutineError",
                                   "Cannot resume a dead coroutine.");
        }
        if (state == State.RUNNING || state == State.NORMAL) {
            throw LoxOps.makeError("RunningCoroutineError",
                                   "Cannot resume a running coroutine.");
        }
        // The first resume's arguments are the function's own parameters,
        // checked before anything mutates state so a bad arity leaves the
        // coroutine resumable (Runtime::resumeCoroutine resets started/state
        // the same way). A later resume carries at most the one value that
        // fills the pending yield expression.
        if (!started) {
            checkFirstArity(args);
        } else if (args.length > 1) {
            throw LoxOps.makeError("ArityError", "Expected 0 or 1 arguments.");
        }

        // Native checks the restored frame budget before it marks the parent
        // NORMAL or changes the coroutine state, so a rejected resume leaves
        // the coroutine suspended (src/runtime.cpp's resumeCoroutine).
        frameBase = LoxClosure.frameCountValue();
        if (started) {
            LoxClosure.checkResumeBudget(frameBase + frameOwn);
        }
        LoxCoroutine parent = CURRENT.get();
        if (parent != null) {
            parent.state = State.NORMAL;
        }
        // Re-add this coroutine's suspended slice above the resumer's own
        // frame and handler counts before it runs. Both counters return to the
        // resumer's value when the coroutine suspends or dies.
        handlerBase = LoxOps.handlerDepthValue();
        callbackBelow = LoxOps.callbackDepthOfChain();
        LoxClosure.setFrameCountValue(frameBase + frameOwn);
        LoxOps.setHandlerDepthValue(handlerBase + handlerOwn);
        state = State.RUNNING;
        try {
            if (!started) {
                started = true;
                firstArgs = args.clone();
                Thread.ofVirtual().name("lox-coroutine").start(this::run);
            } else {
                toCoroutine.put(
                    new Resumed(args.length == 1 ? args[0] : null));
            }
            return deliver();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new LoxError("Coroutine handoff interrupted.");
        } finally {
            CURRENT.set(parent);
            if (parent != null) {
                parent.state = State.RUNNING;
            }
        }
    }

    private Object deliver() throws InterruptedException {
        Object msg = fromCoroutine.take();
        if (msg instanceof Yielded) {
            return ((Yielded)msg).value;
        }
        state = State.DEAD;
        if (msg instanceof Returned) {
            return ((Returned)msg).value;
        }
        Throwable t = ((Thrown)msg).error;
        if (t instanceof LoxError) {
            throw (LoxError)t;
        }
        if (t instanceof RuntimeException) {
            throw (RuntimeException)t;
        }
        if (t instanceof Error) {
            throw (Error)t;
        }
        throw new LoxError(t.toString());
    }

    // The coroutine's whole computation, on its own virtual thread. Every
    // yield inside the call chain blocks on the queues instead of returning
    // here, so this method sees exactly one outcome: the function's result, or
    // whatever escaped it.
    int callbackBelow() { return callbackBelow; }

    private void run() {
        LoxWorker.bind(worker);
        CURRENT.set(this);
        Object msg;
        try {
            Object result = LoxOps.call(callee, firstArgs);
            state = State.DEAD;
            msg = new Returned(result);
        } catch (Throwable t) {
            state = State.DEAD;
            msg = new Thrown(t);
        } finally {
            CURRENT.remove();
            // Drop this coroutine's slice; the resumer sees its own counts.
            LoxClosure.setFrameCountValue(frameBase);
            LoxOps.setHandlerDepthValue(handlerBase);
        }
        try {
            fromCoroutine.put(msg);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /**
     * Suspends the current coroutine until the resumer resumes it, delivering
     * {@code value} and returning the value the next resume sent. Called on the
     * coroutine's own thread by {@link LoxOps#yield(Object)}.
     */
    Object yieldValue(Object value) {
        state = State.SUSPENDED;
        try {
            // Hide this coroutine's own frames and open handlers so the
            // resumer sees its own counts again (native removes the
            // coroutine's slice on suspend).
            frameOwn = LoxClosure.frameCountValue() - frameBase;
            handlerOwn = LoxOps.handlerDepthValue() - handlerBase;
            LoxClosure.setFrameCountValue(frameBase);
            LoxOps.setHandlerDepthValue(handlerBase);
            fromCoroutine.put(new Yielded(value));
            Resumed input = (Resumed)toCoroutine.take();
            state = State.RUNNING;
            return input.value;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new LoxError("Coroutine handoff interrupted.");
        }
    }

    private void checkFirstArity(Object[] args) {
        int arity = arityOf(callee);
        // A native with arity -1 is variadic (LoxNative.arity): any count is
        // accepted, matching Runtime::callNative's `arity != -1` guard.
        if (arity == -1 || args.length == arity) {
            return;
        }
        String message = "Expected " + arity + " arguments but got " +
                         args.length + ".";
        // Mirrors LoxClosure.callAsSelf: an arity fault is catchable only while
        // a handler is live somewhere in the program (issue #319).
        if (!LoxOps.isHandlerLive()) {
            throw new LoxError(message);
        }
        throw LoxOps.makeError("ArityError", message);
    }

    private static int arityOf(Object callee) {
        if (callee instanceof LoxClosure) {
            return ((LoxClosure)callee).arity;
        }
        if (callee instanceof LoxBoundMethod) {
            return ((LoxBoundMethod)callee).method.arity;
        }
        if (callee instanceof LoxNative) {
            return ((LoxNative)callee).arity;
        }
        return 0; // coroutine.create rejects every other callee kind
    }
}
