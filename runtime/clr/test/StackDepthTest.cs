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

        // The ceiling check must use >=, not ==: s_frameCount only grows,
        // so a count already past FramesMax (a handler opened past the
        // ceiling) never hits an exact match again. No Lox program reaches
        // that state through normal calls, so drive it direct here.
        FieldInfo countField = typeof(LoxClosure).GetField(
            "s_frameCount", BindingFlags.NonPublic | BindingFlags.Static);
        int savedCount = (int)countField.GetValue(null);
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
                    "an overflow past FramesMax stays catchable");
                LoxInstance inst = e.Value as LoxInstance;
                object kind = null;
                bool hasKind = inst != null &&
                    inst.Fields.TryGetValue("kind", out kind);
                t.Check(hasKind && (kind as string) == "StackOverflowError",
                    "an overflow past FramesMax carries kind StackOverflowError");
            }
        } finally {
            countField.SetValue(null, savedCount);
        }

        return t.Finish("StackDepthTest");
    }
}
