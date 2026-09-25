namespace Lox;

/// <summary>
/// Base class for every generated function/method body. Holds the upvalue
/// cells captured at CLOSURE time - one one-element <c>object[]</c> ref-cell
/// per upvalue, matching the fresh-cell-per-declaration model in
/// bytecode-translation-problems.md (P4): CLOSE_UPVALUE ends a cell's live
/// range, it does not merely no-op.
///
/// Lox slot 0 is "the callee itself - or, in a method, the receiver `this`"
/// (same doc, root-cause section). <see cref="Call"/> binds slot 0 to this
/// closure, matching a plain function reading its own identity;
/// <see cref="CallAsSelf"/> lets a bound-method/INVOKE/SUPER_INVOKE call
/// site bind slot 0 to the receiver instead. <c>Arity</c> excludes slot 0,
/// matching ObjFunction::arity.
/// </summary>
public abstract class LoxClosure : ILoxCallable {
    // vm.cpp's own CallFrame ceiling (src/vm.h FRAMES_MAX). Generated CIL
    // recurses the real CLR call stack one frame per Lox call - unlike
    // vm.cpp's fixed CallFrame array, nothing here bounds that on its own,
    // and the real stack tolerates far more than 1024 nested calls before
    // it would fault. Counting here keeps every recursion depth that
    // native accepts or rejects agreeing on the CLR backend too, with the
    // same message, instead of only diverging once a real stack fault (an
    // uncatchable StackOverflowException) hits some larger, host-dependent
    // depth.
    //
    // Native actually has TWO ceilings, not one: src/vm.h's STACK_MAX
    // (16384 value-stack slots, shared by every live frame) can be reached
    // first by a frame with many locals, well below 1024 frames deep -
    // src/vm.cpp's own VM::push guards every write against it and reports
    // the same "Stack overflow." native reports for FRAMES_MAX, so that
    // path is controlled on the native side, not a buffer overflow. This
    // counter reproduces only the frame ceiling; nothing here counts
    // value-stack slots, so a program native rejects through STACK_MAX
    // alone still runs to completion on this backend - a known, open gap
    // in this class, not a native defect.
    private const int FramesMax = 1024;

    // Extra capacity held above FramesMax, spent only while a
    // StackOverflowError's own unwind is in progress
    // (s_unwindingStackOverflow). s_frameCount is only decremented in this
    // method's own `finally`, once Invoke actually returns to it — not by
    // the generated catch handler that runs a discarded frame's pending
    // defers (LoxOps.RunDefers) while that frame's own Invoke call is still
    // on the stack, unwinding. A deferred call drained there is therefore a
    // real, if short-lived, nested call made while s_frameCount still holds
    // its pre-unwind value at (or past) FramesMax; with no reserve, that
    // call's own CallAsSelf sees the ceiling again immediately and goes
    // fatal, however shallow the deferred call actually is (issue #446).
    // Mirrors src/vm.h's own STACK_OVERFLOW_FRAME_RESERVE and
    // runtime/jvm/src/lox/LoxClosure.java's FRAMES_RESERVE — same value
    // (16), proven correct there first.
    private const int FramesMaxReserve = 16;

    // Starts at 1, not 0: src/vm.cpp's own interpret() pushes the
    // top-level script itself as CallFrame 0 through the very same call()
    // this class's CallAsSelf mirrors, before the script body ever runs,
    // and that frame is never popped until the whole program ends. A
    // counter starting at 0 here would let one more nested Lox call
    // succeed than FRAMES_MAX allows natively.
    private static int s_frameCount = 1;

    // Mirrors src/vm.cpp's VM::m_unwindingStackOverflow: set for the
    // duration of one StackOverflowError's own unwind (from the moment the
    // ceiling throws it until s_overflowInFlight names it is genuinely
    // delivered to a real catchBlock — see EndStackOverflowUnwind, called
    // from LoxOps.NotifyErrorCaught). A second overflow that hits the
    // ceiling while this is still true (e.g. from a deferred call running
    // during that unwind, once the reserve above is exhausted) goes fatal
    // below instead of catchable - native holds the same guard for the
    // same reason: the alternative is genuine re-entrant unwinding, which
    // native's own C++ call structure cannot support past one level
    // either.
    internal static bool s_unwindingStackOverflow = false;

    // The exact LoxError object whose delivery-or-replacement ends the
    // unwind above — see EndStackOverflowUnwind, IsOverflowInFlight, and
    // ReplaceOverflowInFlight. Identity, not the delivered value's own
    // fields, is what must decide the unwind is over: a plain Lox++
    // instance can carry a field named "kind" equal to "StackOverflowError"
    // with no connection to this guard at all, and a kind-string check
    // would clear the guard on that alone. Mirrors
    // runtime/jvm/src/lox/LoxClosure.java's s_overflowInFlight.
    private static LoxError? s_overflowInFlight = null;

    public readonly string Name; // null for the top-level script, per <script>
    public readonly int Arity;
    public readonly object[][] Upvalues;

    protected LoxClosure(string name, int arity, object[][] upvalues) {
        Name = name;
        Arity = arity;
        Upvalues = upvalues;
    }

    public object Call(object[] args) => CallAsSelf(this, args);

    public object CallAsSelf(object self, object[] args) {
        if (args.Length != Arity) {
            // src/vm.cpp VM::call() makes ArityError catchable only when a
            // handler is live somewhere in the program; with none live, it
            // calls runtimeError() directly, bypassing the catchable
            // machinery entirely (issue #319). LoxOps.HandlerLive mirrors
            // that same dynamic (call-stack-dependent, not per-call-site)
            // test.
            if (!LoxOps.HandlerLive) {
                throw new LoxError(
                    $"Expected {Arity} arguments but got {args.Length}.");
            }
            throw new LoxError(LoxRuntime.MakeError(
                $"Expected {Arity} arguments but got {args.Length}.", "ArityError"));
        }
        // Use >=, not ==: s_frameCount only grows, so a call that starts
        // past FramesMax (a handler opened past the ceiling) would never
        // see an exact match again. Matches src/vm.cpp VM::call().
        if (s_unwindingStackOverflow) {
            // Already unwinding one StackOverflowError: only the reserve
            // stands between here and fatal, and no further catchable
            // attempt is made (see s_unwindingStackOverflow's own comment)
            // — matches native's own reserve-widened ceiling (VM::call(),
            // src/vm.h STACK_OVERFLOW_FRAME_RESERVE).
            if (s_frameCount >= FramesMax + FramesMaxReserve) {
                // A bare-message LoxError is uncatchable (LoxError.cs's own
                // Catchable field) — matches native's RAISE_ERROR("Stack
                // overflow.") fatal path (src/vm.cpp), not
                // tryCatchableError's catchable one.
                throw new LoxError("Stack overflow.");
            }
        } else if (s_frameCount >= FramesMax) {
            if (!LoxOps.HandlerLive) {
                // src/vm.cpp VM::call() takes the same fatal fast path
                // (no handler live anywhere in the program) for the first
                // overflow too - it never constructs the catchable
                // StackOverflowError Error value in that case (issue
                // #319). s_unwindingStackOverflow stays false: this fault
                // is never delivered, so there is no unwind for a second
                // overflow to be re-entrant with.
                throw new LoxError("Stack overflow.");
            }
            // A bare-message LoxError is uncatchable (LoxError.cs's own
            // Catchable field) - native marks this fault catchable
            // (spec/04-semantics.md, StackOverflowError), so this must
            // carry a real Error value with that kind, not a message alone.
            s_unwindingStackOverflow = true;
            LoxError overflow =
                new LoxError(LoxRuntime.MakeError("Stack overflow.", "StackOverflowError"));
            s_overflowInFlight = overflow;
            throw overflow;
        }
        s_frameCount++;
        try {
            return Invoke(self, args);
        } finally {
            s_frameCount--;
        }
    }

    // Called (only) from LoxOps.NotifyErrorCaught, exactly when the fault
    // about to reach a real Lox catchBlock is the one s_overflowInFlight
    // names — that delivery is the one point that knows the unwind is
    // over. Mirrors runtime/jvm/src/lox/LoxClosure.java's
    // endStackOverflowUnwind.
    internal static void EndStackOverflowUnwind() {
        s_unwindingStackOverflow = false;
        s_overflowInFlight = null;
    }

    // Called (only) from LoxOps.NotifyErrorCaught, to decide whether the
    // value about to be delivered to a real catchBlock is the fault whose
    // delivery ends the current unwind — by identity, not by inspecting
    // the delivered value. Mirrors
    // runtime/jvm/src/lox/LoxClosure.java's isOverflowInFlight.
    internal static bool IsOverflowInFlight(LoxError error) {
        return s_unwindingStackOverflow && ReferenceEquals(error, s_overflowInFlight);
    }

    // Called (only) from LoxOps.RunDefers when a deferred call's own throw
    // escapes it (spec/04-semantics.md defer Statement step 5), passing the
    // exact fault the enclosing frame's own defer list is being drained
    // for. The guard's identity moves only when `replaced` names the fault
    // it already watches — not merely whenever the guard happens to be
    // active — so a defer that throws on a normal return, or that replaces
    // some other, unrelated fault while an overflow unwinds elsewhere,
    // leaves this guard untouched. Mirrors
    // runtime/jvm/src/lox/LoxClosure.java's replaceOverflowInFlight.
    internal static void ReplaceOverflowInFlight(LoxError replaced, LoxError replacement) {
        if (s_unwindingStackOverflow && ReferenceEquals(replaced, s_overflowInFlight)) {
            s_overflowInFlight = replacement;
        }
    }

    protected abstract object Invoke(object self, object[] args);
}
