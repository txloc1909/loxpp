package lox;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.channels.SelectionKey;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;

/**
 * Mirrors src/stdlib/net_api.cpp's ObjServer over a {@link
 * ServerSocketChannel}. The channel is nonblocking, so a task that finds no
 * connection waits and the other tasks of its worker run ({@link
 * LoxWorker#waitChannel}).
 */
public final class LoxServer {
    private ServerSocketChannel server; // null once closed
    private final int boundPort;
    private final LoxSide acceptSide = new LoxSide("accept side");

    private LoxServer(ServerSocketChannel server, int boundPort) {
        this.server = server;
        this.boundPort = boundPort;
    }

    public static LoxServer listen(String host, int port) {
        String failure = "unknown host";
        try {
            InetAddress[] addresses = LoxSocket.resolveHost(host);
            if (addresses == null) {
                throw LoxOps.makeError("NetworkError",
                                       "listen(): cannot resolve '" + host +
                                           "'");
            }
            // Every resolved address is tried in order, as native does: the
            // first one can be unusable (an IPv6 address on a host with IPv6
            // off) while a later one binds.
            for (InetAddress ip : addresses) {
                ServerSocketChannel server = null;
                try {
                    server = ServerSocketChannel.open();
                    server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
                    server.bind(new InetSocketAddress(ip, port));
                    server.configureBlocking(false);
                    LoxServer made = new LoxServer(
                        server,
                        ((InetSocketAddress)server.getLocalAddress()).getPort());
                    server = null; // the Server owns the channel now
                    return made;
                } catch (IOException e) {
                    failure = e.getMessage() != null ? e.getMessage() : failure;
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
        } catch (IOException e) {
            failure = e.getMessage() != null ? e.getMessage() : failure;
        }
        throw LoxOps.makeError("NetworkError",
                               "listen(): cannot listen on '" + host + ":" +
                                   port + "': " + failure);
    }

    public Object accept() {
        checkOpen();
        acceptSide.enter("accept");
        try {
            for (;;) {
                checkOpen();
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
                acceptSide.await(server, SelectionKey.OP_ACCEPT);
            }
        } finally {
            acceptSide.leave();
        }
    }

    private void checkOpen() {
        if (server == null) {
            throw LoxOps.makeError("SocketClosedError",
                "Cannot call 'accept' on a closed server.");
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
