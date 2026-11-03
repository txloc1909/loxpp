package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;

import java.io.IOException;
import java.nio.channels.SelectionKey;
import java.nio.channels.SocketChannel;
import java.util.function.Supplier;

/**
 * The sides of a Socket and a Server (spec/04-semantics.md, The Task
 * Machine, Sides) on the JVM runtime: a side holds at most one call, and a
 * call on a side that a call of another task holds raises BusyError. Each
 * task is a hand-written {@link LoxClosure}, as in {@link TaskTest}.
 */
public final class SideTest {
    private static final Object[][] NO_UPVALUES = new Object[0][];
    // Bounds that only catch a hang; far above any loaded time.
    private static final long HANG_NANOS = 60_000_000_000L;

    private static LoxGlobals globals;

    public static void main(String[] args) throws IOException {
        globals = LoxRuntime.init();
        checkEveryReadCallIsBusyOnAHeldReadSide();
        checkWriteSideIsBusyWhileAWriterWaits();
        checkAcceptSideIsBusy();
        checkSideIsHeldWhileTheHolderIsReady();
        checkClosedSideWinsOverHeldSide();
        checkBusyWhereTheTaskCannotSuspend();
        checkCloseWriteEndsOnlyTheWriterWaitAtTheClose();
        checkPollerRefusesASecondWaitForOneInterest();
        System.exit(TestSupport.finish("SideTest"));
    }

    private static LoxTask spawn(String name, Runnable body) {
        LoxInstance module = (LoxInstance)globals.get("task");
        LoxClosure fn = new LoxClosure(name, 0, NO_UPVALUES) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                body.run();
                return null;
            }
        };
        return (LoxTask)((LoxCallable)module.fields.get("spawn"))
            .call(new Object[] {fn});
    }

    private static String kindRaisedBy(Supplier<Object> action) {
        try {
            action.get();
        } catch (LoxError e) {
            return (String)((LoxInstance)e.getValue()).fields.get("kind");
        }
        return "none";
    }

    private static void yieldUntil(Supplier<Boolean> done, String what) {
        long end = System.nanoTime() + HANG_NANOS;
        while (!done.get()) {
            if (System.nanoTime() - end > 0) {
                check(false, "timed out waiting until " + what);
                System.exit(TestSupport.finish("SideTest"));
            }
            try {
                Thread.sleep(1);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            LoxOps.yield(null);
        }
    }

    private static LoxSocket[] connectedPair(LoxServer server) {
        LoxSocket client = LoxSocket.connect("127.0.0.1", (int)server.port());
        return new LoxSocket[] {client, (LoxSocket)server.accept()};
    }

    // A text larger than the buffers of a connection on the local host, so a
    // write of it waits while nothing reads the peer.
    private static String bigText() {
        StringBuilder b = new StringBuilder("x");
        while (b.length() < (32 << 20)) {
            b.append(b);
        }
        return b.toString();
    }

    // Rules 1 and 2: while a reader waits, each read call of another task
    // raises BusyError, also when bytes are in the buffer, and takes nothing.
    // Rule 3: the write side stays free.
    private static void checkEveryReadCallIsBusyOnAHeldReadSide() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        Object[] got = {null};
        LoxTask holder = spawn("holder", () -> got[0] = conn.readline());
        LoxOps.yield(null);
        checkEquals(LoxTask.State.PARKED, holder.state,
                    "the holder parks in readline");
        pair[0].write("ab");
        checkEquals("BusyError", kindRaisedBy(() -> conn.read()), "read");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readBytes(1)),
                    "read_bytes");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readBytesArg(1.0)),
                    "read_bytes through INVOKE");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readSome(1)),
                    "read_some");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readSomeArg(1.0)),
                    "read_some through INVOKE");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readline()),
                    "readline");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readlines()),
                    "readlines");
        checkEquals("none", kindRaisedBy(() -> {
            conn.writeline("to peer");
            return null;
        }), "a write while another task reads the Socket");
        checkEquals("to peer", pair[0].readline(),
                    "the write on the free side reaches the peer");
        pair[0].writeline("c");
        yieldUntil(() -> got[0] != null, "the holder gets its line");
        checkEquals("abc", got[0], "a busy call took no byte of the holder");
        pair[0].writeline("next");
        checkEquals("next", conn.readline(),
                    "the side is free once the call of the holder returned");
        pair[1].close();
        pair[0].close();
        server.close();
    }

    private static void checkWriteSideIsBusyWhileAWriterWaits() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        String big = bigText();
        String[] kind = {null};
        LoxTask writer = spawn("writer", () -> kind[0] = kindRaisedBy(() -> {
            conn.write(big);
            return null;
        }));
        LoxOps.yield(null);
        checkEquals(LoxTask.State.PARKED, writer.state,
                    "the writer waits for room");
        checkEquals("BusyError", kindRaisedBy(() -> {
            conn.write("");
            return null;
        }), "write, also of nothing");
        checkEquals("BusyError", kindRaisedBy(() -> {
            conn.writelineArg("y");
            return null;
        }), "writeline through INVOKE");
        pair[0].close();
        conn.close();
        yieldUntil(() -> kind[0] != null, "the writer ends");
        checkEquals("SocketClosedError", kind[0],
                    "a close ends the wait of the writer");
        server.close();
    }

    private static void checkAcceptSideIsBusy() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        String[] kind = {null};
        LoxTask acceptor =
            spawn("acceptor", () -> kind[0] = kindRaisedBy(server::accept));
        LoxOps.yield(null);
        checkEquals(LoxTask.State.PARKED, acceptor.state,
                    "the acceptor parks");
        checkEquals("BusyError", kindRaisedBy(server::accept),
                    "a second accept raises");
        server.close();
        checkEquals("SocketClosedError", kindRaisedBy(server::accept),
                    "a closed Server fails as closed, not busy (rule 6)");
        yieldUntil(() -> kind[0] != null, "the acceptor ends");
        checkEquals("SocketClosedError", kind[0],
                    "a close ends the wait of the acceptor");
    }

    // Rule 1: the wait of the holder ended and its task is ready, not yet
    // run. Its call still holds the side, so the bytes stay its own.
    private static void checkSideIsHeldWhileTheHolderIsReady() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        Object[] got = {null};
        LoxTask holder = spawn("holder", () -> got[0] = conn.readline());
        LoxOps.yield(null);
        pair[0].writeline("line");
        // A yield takes the main task first again: it joined the run queue
        // before the end of the wait of the holder was seen.
        yieldUntil(() -> holder.state == LoxTask.State.READY,
                   "the wait of the holder ends");
        checkEquals(null, got[0], "the holder has not run yet");
        checkEquals("BusyError", kindRaisedBy(() -> conn.readSome(4)),
                    "a call on the side of a ready holder raises");
        yieldUntil(() -> got[0] != null, "the holder gets its line");
        checkEquals("line", got[0], "the ready holder keeps its bytes");
        pair[1].close();
        pair[0].close();
        server.close();
    }

    // Rule 6: a close ends the wait of the holder, and a call on the closed
    // side fails as on a closed Socket while the woken holder still holds it.
    private static void checkClosedSideWinsOverHeldSide() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        String[] kind = {null};
        LoxTask holder =
            spawn("holder", () -> kind[0] = kindRaisedBy(conn::readline));
        LoxOps.yield(null);
        conn.close();
        checkEquals(LoxTask.State.READY, holder.state,
                    "the close ends the wait of the holder at the close");
        checkEquals("SocketClosedError", kindRaisedBy(conn::readline),
                    "the closed state wins over the held side");
        yieldUntil(() -> kind[0] != null, "the holder ends");
        checkEquals("SocketClosedError", kind[0],
                    "the woken holder fails as on a closed Socket");
        pair[0].close();
        server.close();
    }

    // Rules 2 and 5: where the task cannot suspend, a call on a held side
    // raises BusyError at the call and does not block the worker.
    private static void checkBusyWhereTheTaskCannotSuspend() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        Object[] got = {null};
        spawn("holder", () -> got[0] = conn.readline());
        LoxOps.yield(null);
        pair[0].write("ab");
        LoxOps.enterCallback();
        try {
            checkEquals("BusyError", kindRaisedBy(() -> conn.readSome(1)),
                        "a call that cannot suspend raises on a held side");
        } finally {
            LoxOps.exitCallback();
        }
        pair[0].writeline("c");
        yieldUntil(() -> got[0] != null, "the holder gets its line");
        checkEquals("abc", got[0], "the call under the callback took nothing");
        pair[1].close();
        pair[0].close();
        server.close();
    }

    // Rule 4: close_write() ends the wait of the writer at the call, and the
    // writer raises SocketClosedError; the wait of the reader stays.
    private static void checkCloseWriteEndsOnlyTheWriterWaitAtTheClose() {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        LoxSocket[] pair = connectedPair(server);
        LoxSocket conn = pair[1];
        String big = bigText();
        String[] kind = {null};
        Object[] got = {null};
        LoxTask writer = spawn("writer", () -> kind[0] = kindRaisedBy(() -> {
            conn.write(big);
            return null;
        }));
        LoxTask reader = spawn("reader", () -> got[0] = conn.readline());
        LoxOps.yield(null);
        checkEquals(LoxTask.State.PARKED, writer.state, "the writer waits");
        checkEquals(LoxTask.State.PARKED, reader.state, "the reader waits");
        conn.closeWrite();
        checkEquals(LoxTask.State.READY, writer.state,
                    "close_write() ends the wait of the writer at the call");
        checkEquals(LoxTask.State.PARKED, reader.state,
                    "close_write() keeps the wait of the reader");
        checkEquals("SocketClosedError", kindRaisedBy(() -> {
            conn.write("z");
            return null;
        }), "the closed write side wins over the woken writer");
        yieldUntil(() -> kind[0] != null, "the writer ends");
        checkEquals("SocketClosedError", kind[0],
                    "the woken writer fails as a write after close_write()");
        pair[0].writeline("from peer");
        yieldUntil(() -> got[0] != null, "the reader gets its line");
        checkEquals("from peer", got[0],
                    "the reader still gets the bytes of the peer");
        pair[1].close();
        pair[0].close();
        server.close();
    }

    // The poller keeps one wait for each interest of a channel: a second one
    // means a side let in a second call.
    private static void checkPollerRefusesASecondWaitForOneInterest()
        throws IOException {
        LoxPoller poller = new LoxPoller();
        try (SocketChannel channel = SocketChannel.open()) {
            channel.configureBlocking(false);
            poller.addFd(null, channel, SelectionKey.OP_READ, 0, false, 0);
            poller.addFd(null, channel, SelectionKey.OP_WRITE, 0, false, 0);
            boolean refused = false;
            try {
                poller.addFd(null, channel, SelectionKey.OP_READ, 0, false, 0);
            } catch (IllegalStateException e) {
                refused = true;
            }
            check(refused, "a second read wait on one channel is refused");
        }
    }
}
