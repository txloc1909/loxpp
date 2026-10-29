package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * A Socket that parked and that the program never closes must not keep a
 * selector key after its last parked call ends, and it must park again.
 */
public final class SelectorKeyTest {
    private static final Object[][] NO_UPVALUES = new Object[0][];
    // Only ends a hang; far above any loaded time.
    private static final long HANG_SECONDS = 60;
    private static final int SOCKETS = 100;

    private static LoxGlobals globals;

    public static void main(String[] args) {
        globals = LoxRuntime.init();
        CountDownLatch finished = new CountDownLatch(1);
        Thread dog = new Thread(() -> {
            try {
                if (!finished.await(HANG_SECONDS, TimeUnit.SECONDS)) {
                    System.err.println("FAIL: SelectorKeyTest hangs");
                    Runtime.getRuntime().halt(2);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }, "selector-key-test-watchdog");
        dog.setDaemon(true);
        dog.start();

        checkDroppedSocketsKeepNoKey();
        checkSocketParksAgainAfterItsKeyWentAway();

        finished.countDown();
        System.exit(TestSupport.finish("SelectorKeyTest"));
    }

    private static Object task(String member, Object... args) {
        LoxInstance module = (LoxInstance)globals.get("task");
        return ((LoxCallable)module.fields.get(member)).call(args);
    }

    private static LoxClosure fn(String name, Runnable body) {
        return new LoxClosure(name, 0, NO_UPVALUES) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                body.run();
                return null;
            }
        };
    }

    private static LoxSocket[] connectedPair(LoxServer server) {
        LoxSocket client = LoxSocket.connect("127.0.0.1", (int)server.port());
        return new LoxSocket[] {client, (LoxSocket)server.accept()};
    }

    private static void yieldUntil(boolean[] flag) {
        while (!flag[0]) {
            LoxOps.yield(null);
        }
    }

    // Each reader parks until its line arrives. Neither end is closed.
    private static void checkDroppedSocketsKeepNoKey() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        int maxDuring = 0;
        for (int i = 0; i < SOCKETS; i++) {
            LoxSocket[] pair = connectedPair(server);
            boolean[] done = {false};
            task("spawn", fn("reader", () -> {
                pair[1].readline();
                done[0] = true;
            }));
            LoxOps.yield(null); // the reader parks
            pair[0].writeline("x");
            yieldUntil(done);
            maxDuring = Math.max(maxDuring,
                                 LoxWorker.current().selectorKeyCount());
        }
        checkEquals(0, LoxWorker.current().selectorKeyCount(),
                    "no key stays for " + SOCKETS + " dropped sockets");
        check(maxDuring <= 1, "at most one key at a time (saw " + maxDuring + ")");
        server.close();
    }

    // The same socket parks, wakes, and parks again; each park registers anew.
    private static void checkSocketParksAgainAfterItsKeyWentAway() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        String[] got = new String[3];
        boolean[] done = {false};
        task("spawn", fn("reader", () -> {
            for (int i = 0; i < got.length; i++) {
                got[i] = (String)pair[1].readline();
            }
            done[0] = true;
        }));
        for (int i = 0; i < got.length; i++) {
            LoxOps.yield(null); // the reader parks
            pair[0].writeline("line" + i);
            while (got[i] == null) {
                LoxOps.yield(null);
            }
        }
        yieldUntil(done);
        checkEquals("line0", got[0], "the first park reads its line");
        checkEquals("line1", got[1], "the second park reads its line");
        checkEquals("line2", got[2], "the third park reads its line");
        checkEquals(0, LoxWorker.current().selectorKeyCount(),
                    "no key stays after the last park");
        pair[0].close();
        pair[1].close();
        server.close();
    }
}
