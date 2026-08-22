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
    // and the real stack tolerates far more than 256 nested calls before
    // it would fault. Counting here keeps every recursion depth that
    // native accepts or rejects agreeing on the CLR backend too, with the
    // same message, instead of only diverging once a real stack fault (an
    // uncatchable StackOverflowException) hits some larger, host-dependent
    // depth.
    //
    // Native actually has TWO ceilings, not one: src/vm.h's STACK_MAX
    // (2048 value-stack slots, shared by every live frame) can be reached
    // first by a frame with many locals, well below 256 frames deep -
    // src/vm.cpp guards no push against it, so that path is a native
    // buffer overflow with no defined result to match. This counter
    // reproduces only the frame ceiling; the value-stack one is a native
    // defect (tracked separately), not a gap in this class.
    private const int FramesMax = 256;

    // Starts at 1, not 0: src/vm.cpp's own interpret() pushes the
    // top-level script itself as CallFrame 0 through the very same call()
    // this class's CallAsSelf mirrors, before the script body ever runs,
    // and that frame is never popped until the whole program ends. A
    // counter starting at 0 here would let one more nested Lox call
    // succeed than FRAMES_MAX allows natively.
    private static int s_frameCount = 1;

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
            throw new LoxError($"Expected {Arity} arguments but got {args.Length}.");
        }
        if (s_frameCount == FramesMax) {
            throw new LoxError("Stack overflow.");
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
