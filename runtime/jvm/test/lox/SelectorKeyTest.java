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
        checkSocketParksAgainWhileAnotherSocketWaits();
        checkAcceptLoopWhileAnotherSocketWaits();
        checkReadWakeKeepsTheWriteWait();

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

    // Parks a reader on a second Socket that gets no data until the caller
    // closes it, so the poller always holds one wait.
    private static LoxSocket[] parkBystander(LoxServer server) {
        LoxSocket[] pair = connectedPair(server);
        task("spawn", fn("bystander", () -> {
            try {
                pair[1].readline();
            } catch (LoxError e) {
                // The close at the end of the test ends the wait.
            }
        }));
        LoxOps.yield(null);
        return pair;
    }

    // The main task waits for a message, so no task runs between the wake of
    // the reader and its next park. The key of the first Socket is cancelled
    // while the bystander still waits, so no select flushes it before then.
    private static void checkSocketParksAgainWhileAnotherSocketWaits() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] bystander = parkBystander(server);
        LoxSocket[] pair = connectedPair(server);
        Object me = task("self");
        String[] got = new String[3];
        task("spawn", fn("reader", () -> {
            for (int i = 0; i < got.length; i++) {
                got[i] = (String)pair[1].readline();
                task("send", me, "read");
            }
        }));
        for (int i = 0; i < got.length; i++) {
            LoxOps.yield(null); // the reader parks
            pair[0].writeline("line" + i);
            task("receive");
            checkEquals("line" + i, got[i],
                        "park " + i + " reads its line while a bystander waits");
        }
        checkEquals(1, LoxWorker.current().selectorKeyCount(),
                    "only the bystander keeps a key");
        bystander[0].close();
        bystander[1].close();
        LoxOps.yield(null);
        pair[0].close();
        pair[1].close();
        server.close();
    }

    // The accept loop parks on the same Server again and again.
    private static void checkAcceptLoopWhileAnotherSocketWaits() {
        LoxServer bystanderServer = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] bystander = parkBystander(bystanderServer);
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        Object me = task("self");
        int[] accepted = {0};
        task("spawn", fn("acceptor", () -> {
            for (int i = 0; i < SOCKETS; i++) {
                ((LoxSocket)server.accept()).close();
                accepted[0]++;
                task("send", me, "accepted");
            }
        }));
        for (int i = 0; i < SOCKETS; i++) {
            LoxOps.yield(null); // the acceptor parks
            LoxSocket.connect("127.0.0.1", (int)server.port()).close();
            task("receive");
        }
        checkEquals(SOCKETS, accepted[0], "the accept loop takes every connection");
        checkEquals(1, LoxWorker.current().selectorKeyCount(),
                    "only the bystander keeps a key after the accept loop");
        bystander[0].close();
        bystander[1].close();
        LoxOps.yield(null);
        server.close();
        bystanderServer.close();
    }

    // A read and a write wait on one Socket. The read wakes first and must
    // leave the write wait registered.
    private static void checkReadWakeKeepsTheWriteWait() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        Object me = task("self");
        int size = 32 << 20; // more than the socket buffers hold
        String big = "x".repeat(size);
        task("spawn", fn("reader", () -> {
            pair[1].readline();
            task("send", me, "read");
        }));
        task("spawn", fn("writer", () -> {
            pair[1].write(big);
            task("send", me, "wrote");
        }));
        LoxOps.yield(null); // the reader parks
        LoxOps.yield(null); // the writer fills the send buffer and parks
        pair[0].writeline("go");
        task("receive");
        Object bytes = pair[0].readBytes(size);
        task("receive");
        checkEquals(size, ((String)bytes).length(),
                    "the parked write finishes once the peer reads");
        pair[0].close();
        pair[1].close();
        server.close();
    }
}
