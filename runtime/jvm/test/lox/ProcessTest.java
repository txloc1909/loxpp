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

        // A write to a child that exited without reading its stdin is a fatal
        // error, not a JVM crash.
        LoxProcess dead = LoxProcess.spawn("/bin/sh", args("-c", "exit 0"));
        try {
            Thread.sleep(200);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        int[] deadWrite = {0};
        try {
            dead.write("x");
            dead.write("y");
        } catch (LoxError e) {
            deadWrite[0] = 1;
        }
        checkEquals(1, deadWrite[0],
                    "a write to an exited child is a fatal error");

        // The stream state is checked before the argument type.
        LoxProcess reaped = LoxProcess.spawn("/bin/true", empty);
        reaped.waitStatus();
        String stateMessage = null;
        try {
            reaped.writeArg(Integer.valueOf(1));
        } catch (LoxError e) {
            stateMessage = e.getMessage();
        }
        check(stateMessage != null && stateMessage.contains("after wait()"),
              "process write checks the stream state before the argument type");

        // read_bytes(n) / err_read_bytes(n): bounded child-pipe reads.
        LoxProcess out = LoxProcess.spawn("/bin/sh", args("-c", "printf hello"));
        checkEquals("he", out.readBytesArg(Double.valueOf(2)),
                    "read_bytes(2) returns two bytes");
        checkEquals("llo", out.readBytesArg(Double.valueOf(10)),
                    "read_bytes(10) returns the remainder at EOF");
        checkEquals("", out.readBytesArg(Double.valueOf(1)),
                    "read_bytes(1) returns \"\" at EOF");
        out.waitStatus();

        LoxProcess err = LoxProcess.spawn(
            "/bin/sh", args("-c", "printf boom 1>&2"));
        checkEquals("bo", err.errReadBytesArg(Double.valueOf(2)),
                    "err_read_bytes(2) returns two bytes");
        checkEquals("om", err.errReadBytesArg(Double.valueOf(10)),
                    "err_read_bytes(10) returns the remainder at EOF");
        checkEquals("", err.errReadBytesArg(Double.valueOf(1)),
                    "err_read_bytes(1) returns \"\" at EOF");
        err.waitStatus();
        checkThrows(() -> out.readBytesArg(Double.valueOf(-1)), LoxError.class,
                    "a negative byte count is fatal");

        System.exit(TestSupport.finish("ProcessTest"));
    }
}
