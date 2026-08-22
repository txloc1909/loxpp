using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// LoxClosure.CallAsSelf's own call-depth ceiling, mirroring src/vm.h's
/// FRAMES_MAX (256) and src/vm.cpp's "Stack overflow." error. The boundary
/// below (254 succeeds, 255 throws) matches native exactly, because
/// src/vm.cpp's own top-level script call already occupies one of the 256
/// CallFrame slots before any user call runs - verified against
/// build-release/loxpp on the same recursive program.
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

        t.CheckEquals(0.0, down.Call(new object[] { 254.0 }),
            "254 nested Lox calls succeed, the deepest native's own frame ceiling allows");
        t.CheckThrows(() => down.Call(new object[] { 255.0 }), typeof(LoxError),
            "255 nested Lox calls overflow, the same depth src/vm.cpp's FRAMES_MAX rejects");

        return t.Finish("StackDepthTest");
    }
}
