package lox;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.ServerSocket;

/** Mirrors src/stdlib/net_api.cpp's ObjServer over a {@link ServerSocket}. */
public final class LoxServer {
    private ServerSocket server; // null once closed
    private final int boundPort;

    public LoxServer(ServerSocket server) {
        this.server = server;
        this.boundPort = server.getLocalPort();
    }

    public static LoxServer listen(String host, int port) {
        try {
            ServerSocket server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(host, port));
            return new LoxServer(server);
        } catch (IOException e) {
            throw new LoxError("listen(): cannot listen on '" + host + ":" +
                               port + "': " + e.getMessage());
        }
    }

    public Object accept() {
        if (server == null) {
            throw new LoxError("Cannot call 'accept' on a closed server.");
        }
        try {
            return new LoxSocket(server.accept());
        } catch (IOException e) {
            throw new LoxError("accept(): " + e.getMessage());
        }
    }

    public double port() {
        return boundPort;
    }

    public void close() {
        if (server != null) {
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
