using System.Reflection;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// LoxClosure.CallAsSelf's own call-depth ceiling, mirroring src/vm.h's
/// FRAMES_MAX (1024) and src/vm.cpp's "Stack overflow." error. The boundary
/// below (1022 succeeds, 1023 throws) matches native exactly, because
/// src/vm.cpp's own top-level script call already occupies one of the 1024
/// CallFrame slots before any user call runs - verified against the
/// release-preset native binary (build/loxpp) on the same recursive
/// program. This covers only the frame ceiling: native also has a
/// separate value-stack ceiling (src/vm.h STACK_MAX, guarded by
/// VM::push) that a frame with many locals can reach first, well below
/// depth 1024 - this class has no counterpart for it, so a program native
/// rejects that way still runs to completion here, a known, open gap not
/// exercised by this test (see test/translation-probes/clr-only/known-divergence).
/// </summary>
public static class StackDepthTest {
    public static int Run() {
        var t = new TestSupport();

        DelegateClosure down = null;
        down = new DelegateClosure("down", 1, new object[0][], (self, a) => {
            double n = (double)a[0];
            if (n == 0) {
                return 0.0;
            }
            return down.Call(new object[] { n - 1 });
        });

        t.CheckEquals(0.0, down.Call(new object[] { 1022.0 }),
            "1022 nested Lox calls succeed, the deepest native's own frame ceiling allows");
        t.CheckThrows(() => down.Call(new object[] { 1023.0 }), typeof(LoxError),
            "1023 nested Lox calls overflow, the same depth src/vm.cpp's FRAMES_MAX rejects");

        // The overflow above set LoxClosure.s_unwindingStackOverflow
        // (issue #316's reentrant-overflow guard) and left it set: this
        // test catches the LoxError directly in C#, bypassing the
        // generated-CIL catch prologue (clr_emitter.cpp) whose
        // LoxOps.NotifyErrorCaught call is the only thing that clears it
        // in a real Lox++ program. Clear it by hand here so the next
        // overflow check below is independent of this one, the same way
        // it would be for two unrelated try/catch blocks in actual Lox++
        // source.
        FieldInfo unwindingField = typeof(LoxClosure).GetField(
            "s_unwindingStackOverflow", BindingFlags.NonPublic | BindingFlags.Static);
        unwindingField.SetValue(null, false);

        // The ceiling check must use >=, not ==: s_frameCount only grows,
        // so a count already past FramesMax (a handler opened past the
        // ceiling) never hits an exact match again. No Lox program reaches
        // that state through normal calls, so drive it direct here.
        //
        // src/vm.cpp VM::call() only takes the catchable StackOverflowError
        // path when a handler is live somewhere in the program
        // (!m_handlerStack.empty()); with none live it goes straight to
        // the fatal "Stack overflow." path instead (issue #319). LoxOps.
        // EnterHandler/ExitHandler is the CLR mirror of that same dynamic,
        // call-stack-scoped check (LoxOps.HandlerLive) — a public API, so
        // no reflection is needed here the way s_frameCount/
        // s_unwindingStackOverflow need it. Push one before the overflow
        // below so this scenario matches what a live Lox try/catch would
        // give it, the same way test/translation-probes/clr-only/
        // 53_stack_overflow_catchable.lox exercises the catchable case
        // end to end through generated CIL.
        FieldInfo countField = typeof(LoxClosure).GetField(
            "s_frameCount", BindingFlags.NonPublic | BindingFlags.Static);
        int savedCount = (int)countField.GetValue(null);
        LoxOps.EnterHandler();
        try {
            countField.SetValue(null, 1025);
            DelegateClosure trivial = new DelegateClosure(
                "trivial", 0, new object[0][], (self, a) => 0.0);
            try {
                trivial.Call(new object[0]);
                t.Check(false,
                    "a call past FramesMax overflows instead of running");
            } catch (LoxError e) {
                t.Check(e.Catchable,
                    "an overflow past FramesMax with a handler live stays catchable");
                LoxInstance inst = e.Value as LoxInstance;
                object kind = null;
                bool hasKind = inst != null &&
                    inst.Fields.TryGetValue("kind", out kind);
                t.Check(hasKind && (kind as string) == "StackOverflowError",
                    "an overflow past FramesMax carries kind StackOverflowError");
            }
        } finally {
            countField.SetValue(null, savedCount);
            LoxOps.ExitHandler();
        }

        // Issue #446: a deferred call drained mid-unwind (the generated
        // catch prologue's own call to LoxOps.RunDefers, clr_emitter.cpp)
        // runs while its own frame's s_frameCount has not yet been
        // decremented -- that only happens in this class's own `finally`,
        // once Invoke returns to it -- so it needs room of its own above
        // FramesMax to make any nested call at all while
        // s_unwindingStackOverflow is still true. FramesMaxReserve is that
        // room (mirrors src/vm.h's STACK_OVERFLOW_FRAME_RESERVE). A real
        // Lox++ program sets s_unwindingStackOverflow only through the
        // ceiling check itself; drive it directly here, the same way the
        // block above drives s_frameCount directly, since no C# unit test
        // runs through the generated-CIL catch prologue that would set it
        // another way.
        // Expected reserve size (src/vm.h's own STACK_OVERFLOW_FRAME_RESERVE
        // and runtime/jvm/src/lox/LoxClosure.java's FRAMES_RESERVE, both
        // 16) is a LITERAL here, not read from LoxClosure.FramesMaxReserve
        // via reflection: reading it back would make this test check the
        // ceiling logic's own internal consistency with whatever the
        // constant happens to be, not that the constant is actually big
        // enough -- shrinking FramesMaxReserve by mistake would move both
        // boundaries below and the checks would still "pass" against each
        // other. A literal boundary is what actually catches that.
        const int expectedReserve = 16;

        unwindingField.SetValue(null, true);
        try {
            // Within the reserve: succeeds, matching a deferred call
            // drained mid-unwind that itself makes an ordinary,
            // non-recursive call.
            countField.SetValue(null, 1024 + expectedReserve - 1);
            DelegateClosure withinReserve = new DelegateClosure(
                "withinReserve", 0, new object[0][], (self, a) => 42.0);
            t.CheckEquals(42.0, withinReserve.Call(new object[0]),
                "a call within the reserve while unwinding an overflow succeeds");

            // At or past the reserve: fatal, not catchable, matching
            // issue #316's original guard -- the reserve is small enough
            // that no path through it can recurse for long, so a call
            // that outruns it too is a genuine second overflow, not a
            // shallow deferred call.
            countField.SetValue(null, 1024 + expectedReserve);
            DelegateClosure pastReserve = new DelegateClosure(
                "pastReserve", 0, new object[0][], (self, a) => 0.0);
            try {
                pastReserve.Call(new object[0]);
                t.Check(false,
                    "a call past the reserve while already unwinding an overflow still overflows");
            } catch (LoxError e) {
                t.Check(!e.Catchable,
                    "a second overflow past the reserve while the first is still unwinding is fatal, not catchable");
            }
        } finally {
            countField.SetValue(null, savedCount);
            unwindingField.SetValue(null, false);
        }

        return t.Finish("StackDepthTest");
    }
}
