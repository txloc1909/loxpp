package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.IOException;

public final class ProcessTest {
    private static LoxList args(String... values) {
        LoxList list = new LoxList();
        for (String v : values) {
            list.elements.add(v);
        }
        return list;
    }

    public static void main(String[] args) throws IOException {
        LoxList empty = new LoxList();

        LoxProcess cat = LoxProcess.spawn("/bin/cat", empty);
        check(cat.pid() > 0, "spawn() returns a real pid");
        cat.writeline("hi");
        cat.closeStdin();
        checkEquals("hi", cat.readline(), "readline() returns the child output");
        checkEquals(0.0, cat.waitStatus(), "wait() returns the exit status");
        checkEquals(0.0, cat.waitStatus(), "wait() is idempotent");
        checkThrows(() -> cat.write("x"), LoxError.class,
                    "a write after wait() is a fatal error");

        LoxMap r = LoxProcess.run(
            "/bin/sh", args("-c", "printf out; printf err 1>&2; exit 3"));
        checkEquals(3.0, r.get("status"), "run() reports the exit status");
        checkEquals("out", r.get("stdout"), "run() captures stdout");
        checkEquals("err", r.get("stderr"), "run() captures stderr");

        LoxProcess sleeper = LoxProcess.spawn("/bin/sleep", args("5"));
        sleeper.kill();
        checkEquals(137.0, sleeper.waitStatus(), "kill() then wait() gives 128+SIGKILL");

        checkEquals("<process>", LoxOps.stringify(cat), "a Process stringifies as <process>");
        check(LoxOps.getProperty(cat, "wait") instanceof LoxCallable,
              "getProperty() on a Process returns its method");
        checkThrows(() -> LoxOps.getProperty(cat, "bogus"), LoxError.class,
                    "an undefined process property is fatal");

        checkThrows(() -> LoxProcess.spawn("/no/such/program_loxpp_xyz", empty),
                    LoxError.class, "spawn() of a missing program is fatal");
        checkThrows(() -> LoxProcess.spawn("/bin/echo", args((String)null)),
                    LoxError.class, "spawn() rejects a non-string argument");

        System.exit(TestSupport.finish("ProcessTest"));
    }
}
