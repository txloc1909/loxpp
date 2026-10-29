package lox;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.channels.SelectionKey;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;

/**
 * Mirrors src/stdlib/net_api.cpp's ObjServer over a {@link
 * ServerSocketChannel}. The channel is nonblocking, so a task that finds no
 * connection parks and the other tasks of its worker run ({@link
 * LoxWorker#waitChannel}).
 */
public final class LoxServer {
    private ServerSocketChannel server; // null once closed
    private final int boundPort;

    private LoxServer(ServerSocketChannel server, int boundPort) {
        this.server = server;
        this.boundPort = boundPort;
    }

    public static LoxServer listen(String host, int port) {
        ServerSocketChannel server = null;
        try {
            server = ServerSocketChannel.open();
            server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
            server.bind(new InetSocketAddress(host, port));
            server.configureBlocking(false);
            LoxServer made = new LoxServer(
                server, ((InetSocketAddress)server.getLocalAddress()).getPort());
            server = null; // the Server owns the channel now
            return made;
        } catch (IOException e) {
            throw LoxOps.makeError("NetworkError",
                                   "listen(): cannot listen on '" + host + ":" +
                                       port + "': " + e.getMessage());
        } finally {
            if (server != null) {
                try {
                    server.close();
                } catch (IOException ignored) {
                    // The listen fault is the one to report.
                }
            }
        }
    }

    public Object accept() {
        LoxWorker.beginWaitingCall();
        for (;;) {
            if (server == null) {
                throw LoxOps.makeError("SocketClosedError",
                    "Cannot call 'accept' on a closed server.");
            }
            try {
                SocketChannel peer = server.accept();
                if (peer != null) {
                    peer.configureBlocking(false);
                    return new LoxSocket(peer);
                }
            } catch (IOException e) {
                throw LoxOps.makeError("NetworkError",
                                       "accept(): " + e.getMessage());
            }
            LoxWorker.waitChannel(server, SelectionKey.OP_ACCEPT);
        }
    }

    public double port() {
        return boundPort;
    }

    public void close() {
        if (server != null) {
            LoxWorker.wakeChannel(server);
            try {
                server.close();
            } catch (IOException ignored) {
                // Matches ObjServer: close is best-effort, never raises.
            }
            server = null;
        }
    }

    public LoxCallable getMethod(String name) {
        LoxCallable unbound = createMethod(name);
        if (unbound == null) {
            return null;
        }
        LoxNative n = (LoxNative)unbound;
        return new LoxNative(n.name, n.arity, n::call, this);
    }

    private LoxCallable createMethod(String name) {
        switch (name) {
        case "accept":
            return new LoxNative("accept", 0, a -> accept());
        case "port":
            return new LoxNative("port", 0, a -> port());
        case "close":
            return new LoxNative("close", 0, a -> {
                close();
                return null;
            });
        default:
            return null;
        }
    }
}
