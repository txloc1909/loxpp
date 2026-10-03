package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.IOException;

public final class SocketTest {
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

        System.exit(TestSupport.finish("SocketTest"));
    }
}
