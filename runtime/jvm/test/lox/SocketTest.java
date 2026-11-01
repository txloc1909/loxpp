package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.IOException;

public final class SocketTest {
    private static LoxSocket connectOrNull(String host, int port) {
        try {
            return LoxSocket.connect(host, port);
        } catch (LoxError e) {
            return null;
        }
    }

    /** The text of the fault that a plain connect to {@code ip} raises. */
    private static String connectFailure(java.net.InetAddress ip, int port) {
        try (java.nio.channels.SocketChannel c = java.nio.channels.SocketChannel.open()) {
            c.configureBlocking(false);
            if (!c.connect(new java.net.InetSocketAddress(ip, port))) {
                while (!c.finishConnect()) {
                    Thread.sleep(1);
                }
            }
            return null;
        } catch (IOException | InterruptedException e) {
            return e.getMessage();
        }
    }

    public static void main(String[] args) throws IOException {
        LoxServer server = LoxServer.listen("127.0.0.1", 0);
        check(server.port() > 0, "listen() binds a real port");

        LoxSocket client = LoxSocket.connect("127.0.0.1", (int)server.port());
        client.writeline("ping");
        client.closeWrite();

        Object accepted = server.accept();
        check(accepted instanceof LoxSocket, "accept() returns a Socket");
        LoxSocket peer = (LoxSocket)accepted;
        checkEquals("ping\n", peer.read(), "the server reads what the client wrote");
        peer.write("pong");
        peer.close();

        checkEquals("pong", client.read(), "read() returns what the peer wrote");
        checkThrows(() -> client.write("x"), LoxError.class,
                    "a write after close_write() is a fatal error");
        client.close();
        server.close();
        checkThrows(() -> server.accept(), LoxError.class,
                    "accept() on a closed server is a fatal error");

        // Method values are fresh on every read, like File's.
        LoxServer s2 = LoxServer.listen("127.0.0.1", 0);
        check(!LoxOps.equal(s2.getMethod("accept"), s2.getMethod("accept")),
                "two reads of a server's method are not Lox-equal");
        s2.close();

        checkEquals("<socket>", LoxOps.stringify(peer), "a Socket stringifies as <socket>");
        checkEquals("<server>", LoxOps.stringify(s2), "a Server stringifies as <server>");

        // The property-get path names the type in its error, and a method
        // lookup on getProperty returns a callable bound to the receiver.
        checkThrows(() -> LoxOps.getProperty(peer, "bogus"), LoxError.class,
                    "an undefined socket property is fatal");
        check(LoxOps.getProperty(peer, "read") instanceof LoxCallable,
              "getProperty() on a Socket returns its method");

        int[] rangeError = {0};
        try {
            LoxSocket.connect("127.0.0.1", 0);
        } catch (LoxError e) {
            rangeError[0] = 1;
        }
        checkEquals(1, rangeError[0], "connect() rejects port 0");

        LoxServer ephemeral = LoxServer.listen("127.0.0.1", 0);
        int closedPort = (int)ephemeral.port();
        ephemeral.close();
        int[] launchError = {0};
        try {
            LoxSocket.connect("127.0.0.1", closedPort);
        } catch (LoxError e) {
            launchError[0] = 1;
        }
        checkEquals(1, launchError[0], "connect() to a refused port is fatal");

        // A name with a bad first address connects through a later one, and
        // when every address fails the call raises one fault.
        LoxServer second = LoxServer.listen("127.0.0.1", 0);
        int secondPort = (int)second.port();
        java.net.InetAddress noListener = java.net.InetAddress.getByName("127.0.0.2");
        java.net.InetAddress listening = java.net.InetAddress.getByName("127.0.0.1");
        LoxSocket.Resolver saved = LoxSocket.resolver;
        try {
            LoxSocket.resolver = h -> new java.net.InetAddress[] {noListener, listening};
            LoxSocket viaSecond = LoxSocket.connect("multi.test", secondPort);
            viaSecond.writeline("hi");
            viaSecond.closeWrite();
            LoxSocket gotten = (LoxSocket)second.accept();
            checkEquals("hi\n", gotten.read(), "connect() falls through to the second address");
            gotten.close();
            viaSecond.close();

            LoxSocket.resolver = h -> new java.net.InetAddress[] {noListener, noListener};
            checkThrows(() -> LoxSocket.connect("multi.test", secondPort), LoxError.class,
                        "connect() raises when every address fails");

            // Two bad addresses before the good one: a connect that stops
            // after the second address fails here.
            LoxSocket.resolver = h -> new java.net.InetAddress[] {noListener, noListener, listening};
            LoxSocket viaThird = connectOrNull("multi.test", secondPort);
            check(viaThird != null, "connect() falls through to the third address");
            if (viaThird != null) {
                viaThird.writeline("third");
                viaThird.closeWrite();
                LoxSocket thirdPeer = (LoxSocket)second.accept();
                checkEquals("third\n", thirdPeer.read(),
                            "the third address is the one that connected");
                thirdPeer.close();
                viaThird.close();
            }

            // A good first address wins: a connect that tries only the last
            // address fails here.
            LoxSocket.resolver = h -> new java.net.InetAddress[] {listening, noListener};
            LoxSocket viaFirst = connectOrNull("multi.test", secondPort);
            check(viaFirst != null, "connect() uses the first address when it works");
            if (viaFirst != null) {
                viaFirst.writeline("first");
                viaFirst.closeWrite();
                LoxSocket firstPeer = (LoxSocket)second.accept();
                checkEquals("first\n", firstPeer.read(),
                            "the first address is the one that connected");
                firstPeer.close();
                viaFirst.close();
            }

            // When every address fails, the fault is the one of the last
            // address. The two failures differ in their text.
            java.net.InetAddress unreachable = java.net.InetAddress.getByName("255.255.255.255");
            String refused = connectFailure(noListener, secondPort);
            String lastFailure = connectFailure(unreachable, secondPort);
            check(refused != null && lastFailure != null && !refused.equals(lastFailure),
                  "the two bad addresses fail with different texts");
            LoxSocket.resolver = h -> new java.net.InetAddress[] {noListener, unreachable};
            LoxError lastError = null;
            try {
                LoxSocket.connect("multi.test", secondPort);
            } catch (LoxError e) {
                lastError = e;
            }
            String reported = lastError == null ? "" : lastError.getMessage();
            check(reported.endsWith(": " + lastFailure),
                  "connect() reports the fault of the last address (got '" + reported + "')");
            check(refused == null || !reported.contains(refused),
                  "connect() does not report the fault of an earlier address");
        } finally {
            LoxSocket.resolver = saved;
            second.close();
        }

        // A limit on connect: the listener never accepts and its queue of
        // waiting connections is full, so only the limit ends the wait.
        try (java.nio.channels.ServerSocketChannel full =
                 java.nio.channels.ServerSocketChannel.open()) {
            full.bind(new java.net.InetSocketAddress("127.0.0.1", 0), 1);
            int fullPort = ((java.net.InetSocketAddress)full.getLocalAddress()).getPort();
            java.util.ArrayList<java.nio.channels.SocketChannel> fillers = new java.util.ArrayList<>();
            boolean stuck = false;
            try {
                for (int i = 0; i < 8 && !stuck; i++) {
                    java.nio.channels.SocketChannel f = java.nio.channels.SocketChannel.open();
                    fillers.add(f);
                    f.configureBlocking(false);
                    if (!f.connect(new java.net.InetSocketAddress("127.0.0.1", fullPort))) {
                        long end = System.nanoTime() + 100_000_000L;
                        while (!f.finishConnect() && System.nanoTime() < end) {
                            Thread.sleep(1);
                        }
                        stuck = !f.finishConnect();
                    }
                }
                if (stuck) {
                    LoxError limit = null;
                    try {
                        LoxSocket.connect("127.0.0.1", fullPort, 200_000_000L);
                    } catch (LoxError e) {
                        limit = e;
                    }
                    check(limit != null, "a limit ends a connect that is not answered");
                    check(limit != null && limit.getMessage().contains("timed out"),
                          "the fault of a limit names the timeout");
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } finally {
                for (java.nio.channels.SocketChannel f : fillers) {
                    f.close();
                }
            }
        }

        // close_write() is a no-op after close(), matching spec/05-stdlib.md.
        LoxServer cs = LoxServer.listen("127.0.0.1", 0);
        LoxSocket cc = LoxSocket.connect("127.0.0.1", (int)cs.port());
        cc.close();
        cc.closeWrite();
        cc.closeWrite();
        check(true, "close_write() after close() does nothing");
        cs.close();

        // A write to a peer that closed its socket is a fatal error, never a
        // JVM crash.
        LoxServer ps = LoxServer.listen("127.0.0.1", 0);
        LoxSocket pc = LoxSocket.connect("127.0.0.1", (int)ps.port());
        LoxSocket peerSocket = (LoxSocket)ps.accept();
        peerSocket.close();
        int[] peerWrite = {0};
        try {
            pc.write("x");
            pc.write("y");
        } catch (LoxError e) {
            peerWrite[0] = 1;
        }
        checkEquals(1, peerWrite[0], "a write to a closed peer is a fatal error");
        pc.close();
        ps.close();

        // The stream state is checked before the argument type, so the fatal
        // message names the closed stream, matching native.
        LoxServer ts = LoxServer.listen("127.0.0.1", 0);
        LoxSocket tc = LoxSocket.connect("127.0.0.1", (int)ts.port());
        tc.close();
        String stateMessage = null;
        try {
            tc.writeArg(Integer.valueOf(42));
        } catch (LoxError e) {
            stateMessage = e.getMessage();
        }
        check(stateMessage != null && stateMessage.contains("closed socket"),
              "write checks the stream state before the argument type");
        ts.close();

        // read_bytes(n): a bounded read over the loopback. Three exact reads
        // consume six bytes; the fourth sees EOF and returns "".
        LoxServer bs = LoxServer.listen("127.0.0.1", 0);
        LoxSocket bc = LoxSocket.connect("127.0.0.1", (int)bs.port());
        bc.write("abcdef");
        bc.closeWrite();
        LoxSocket bpeer = (LoxSocket)bs.accept();
        checkEquals("abc", bpeer.readBytesArg(Double.valueOf(3)),
                    "read_bytes(3) returns three bytes");
        checkEquals("def", bpeer.readBytesArg(Double.valueOf(3)),
                    "read_bytes(3) returns the next three bytes");
        checkEquals("", bpeer.readBytesArg(Double.valueOf(3)),
                    "read_bytes(3) returns \"\" at EOF");
        checkEquals("", bpeer.readBytesArg(Double.valueOf(0)),
                    "read_bytes(0) returns \"\"");
        checkEquals("", bpeer.readBytes(0),
                    "readBytes is callable directly");
        bpeer.close();
        bc.close();
        bs.close();

        // A bad count is fatal, and the closed-stream check precedes it.
        LoxServer is = LoxServer.listen("127.0.0.1", 0);
        LoxSocket ic = LoxSocket.connect("127.0.0.1", (int)is.port());
        checkThrows(() -> ic.readBytesArg(Double.valueOf(-1)), LoxError.class,
                    "read_bytes(-1) is fatal");
        checkThrows(() -> ic.readBytesArg(Double.valueOf(1.5)), LoxError.class,
                    "read_bytes(1.5) is fatal");
        checkThrows(() -> ic.readBytesArg("x"), LoxError.class,
                    "read_bytes(\"x\") is fatal");
        ic.close();
        checkThrows(() -> ic.readBytesArg(Double.valueOf(1)), LoxError.class,
                    "read_bytes on a closed socket is fatal");
        is.close();

        System.exit(TestSupport.finish("SocketTest"));
    }
}
