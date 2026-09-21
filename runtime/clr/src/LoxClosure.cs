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

    // Starts at 1, not 0: src/vm.cpp's own interpret() pushes the
    // top-level script itself as CallFrame 0 through the very same call()
    // this class's CallAsSelf mirrors, before the script body ever runs,
    // and that frame is never popped until the whole program ends. A
    // counter starting at 0 here would let one more nested Lox call
    // succeed than FRAMES_MAX allows natively.
    private static int s_frameCount = 1;

    // Mirrors src/vm.cpp's VM::m_unwindingStackOverflow: set for the
    // duration of one StackOverflowError's own unwind (from the moment the
    // ceiling throws it until the specific catch that receives it runs -
    // see LoxOps.NotifyErrorCaught, called from the shared catch prologue
    // clr_emitter.cpp emits for every try/catch). A second overflow that
    // hits the ceiling while this is still true (e.g. from a deferred call
    // running during that unwind) goes fatal below instead of catchable -
    // native holds the same guard for the same reason: the alternative is
    // genuine re-entrant unwinding, which native's own C++ call structure
    // cannot support past one level either.
    internal static bool s_unwindingStackOverflow = false;

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
            throw new LoxError(LoxRuntime.MakeError(
                $"Expected {Arity} arguments but got {args.Length}.", "ArityError"));
        }
        // Use >=, not ==: s_frameCount only grows, so a call that starts
        // past FramesMax (a handler opened past the ceiling) would never
        // see an exact match again. Matches src/vm.cpp VM::call().
        if (s_frameCount >= FramesMax) {
            if (s_unwindingStackOverflow) {
                // A second overflow while the first is still unwinding -
                // matches native's RAISE_ERROR("Stack overflow.") fatal
                // path (src/vm.cpp), not tryCatchableError's catchable one.
                throw new LoxError("Stack overflow.");
            }
            // A bare-message LoxError is uncatchable (LoxError.cs's own
            // Catchable field) - native marks this fault catchable
            // (spec/04-semantics.md, StackOverflowError), so this must
            // carry a real Error value with that kind, not a message alone.
            s_unwindingStackOverflow = true;
            throw new LoxError(LoxRuntime.MakeError("Stack overflow.", "StackOverflowError"));
        }
        s_frameCount++;
        try {
            return Invoke(self, args);
        } finally {
            s_frameCount--;
        }
    }

    protected abstract object Invoke(object self, object[] args);
}
