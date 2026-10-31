package lox;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.SocketChannel;

/**
 * Mirrors src/stdlib/net_api.cpp's ObjSocket over a nonblocking {@link
 * SocketChannel}. A call that finds nothing to read, no room to write, or no
 * finished connection parks its task, and the other tasks of its worker run
 * ({@link LoxWorker#waitChannel}). Text crosses this boundary as {@link
 * LoxRuntime#CHARSET}, the same byte-boundary rule {@link LoxFile} follows.
 */
public final class LoxSocket {
    private static final int CHUNK = 8192;

    private SocketChannel channel; // null once closed
    // The bytes read from the channel and not yet returned: buf[pos, lim).
    private final byte[] buf = new byte[CHUNK];
    private final ByteBuffer window = ByteBuffer.wrap(buf);
    private int pos;
    private int lim;
    private boolean eof;
    private boolean writeClosed;

    LoxSocket(SocketChannel channel) {
        this.channel = channel;
    }

    private void checkOpen(String method) {
        if (channel == null) {
            throw LoxOps.makeError("SocketClosedError",
                                   "Cannot call '" + method + "' on a closed socket.");
        }
    }

    private void checkWritable(String method) {
        checkOpen(method);
        if (writeClosed) {
            throw LoxOps.makeError("SocketClosedError",
                                   "Cannot write to a socket after close_write().");
        }
    }

    /**
     * Refills the empty buffer. False at the end of the stream. A task that
     * finds no bytes waits here; a close during the wait raises in checkOpen.
     */
    private boolean fill(String method) {
        if (eof) {
            return false;
        }
        for (;;) {
            checkOpen(method);
            window.clear();
            int n;
            try {
                n = channel.read(window);
            } catch (IOException e) {
                throw LoxOps.makeError("NetworkError",
                                       method + "(): " + e.getMessage());
            }
            if (n > 0) {
                pos = 0;
                lim = n;
                return true;
            }
            if (n < 0) {
                eof = true;
                return false;
            }
            LoxWorker.waitChannel(channel, SelectionKey.OP_READ);
        }
    }

    public Object read() {
        checkOpen("read");
        LoxWorker.beginWaitingCall();
        ByteArrayOutputStream all = new ByteArrayOutputStream();
        do {
            all.write(buf, pos, lim - pos);
            pos = lim;
        } while (fill("read"));
        return new String(all.toByteArray(), LoxRuntime.CHARSET);
    }

    /**
     * Up to {@code n} bytes. Waits until {@code n} bytes have arrived or the
     * peer closes, then returns what was read; "" at EOF, matching native's
     * readStreamBytes.
     */
    public Object readBytes(int n) {
        checkOpen("read_bytes");
        LoxWorker.beginWaitingCall();
        ByteArrayOutputStream got = new ByteArrayOutputStream();
        while (got.size() < n) {
            if (pos == lim && !fill("read_bytes")) {
                break;
            }
            int take = Math.min(lim - pos, n - got.size());
            got.write(buf, pos, take);
            pos += take;
        }
        return new String(got.toByteArray(), LoxRuntime.CHARSET);
    }

    /**
     * Up to {@code n} bytes from any stream. Blocks until {@code n} bytes have
     * arrived or the stream reaches EOF, then returns what was read; "" at EOF.
     * Shared with LoxProcess so the socket and process paths cannot drift.
     */
    static Object readBytesFrom(InputStream in, int n, String method) {
        try {
            return readBytesRaw(in, n);
        } catch (IOException e) {
            throw new LoxError(method + "(): " + e.getMessage());
        }
    }

    private static Object readBytesRaw(InputStream in, int n) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        while (buf.size() < n) {
            int want = Math.min(chunk.length, n - buf.size());
            int got = in.read(chunk, 0, want);
            if (got == -1) {
                break;
            }
            buf.write(chunk, 0, got);
        }
        return new String(buf.toByteArray(), LoxRuntime.CHARSET);
    }

    /**
     * The INVOKE and bound-native entry point. The stream state is checked
     * before the byte count, matching native's checkSocketRead-then-asByteCount
     * order, so a bad count on a closed socket reports the closed socket.
     */
    public Object readBytesArg(Object arg) {
        checkOpen("read_bytes");
        return readBytes(checkByteCountArg(arg, "read_bytes"));
    }

    /** One line, newline stripped; null at EOF. A partial trailing line still counts. */
    public Object readline() {
        checkOpen("readline");
        LoxWorker.beginWaitingCall();
        return readLineOfCall();
    }

    private Object readLineOfCall() {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        boolean sawByte = false;
        while (pos < lim || fill("readline")) {
            int end = pos;
            while (end < lim && buf[end] != '\n') {
                end++;
            }
            line.write(buf, pos, end - pos);
            sawByte = true;
            if (end < lim) {
                pos = end + 1;
                break;
            }
            pos = lim;
        }
        return sawByte ? new String(line.toByteArray(), LoxRuntime.CHARSET)
                       : null;
    }

    public Object readlines() {
        checkOpen("readlines");
        LoxWorker.beginWaitingCall();
        LoxList list = new LoxList();
        Object line;
        while ((line = readLineOfCall()) != null) {
            list.elements.add(line);
        }
        return list;
    }

    public void write(String s) {
        writeBytes(s, "write");
    }

    public void writeline(String s) {
        writeBytes(s + "\n", "writeline");
    }

    private void writeBytes(String s, String method) {
        checkWritable(method);
        LoxWorker.beginWaitingCall();
        ByteBuffer data = ByteBuffer.wrap(s.getBytes(LoxRuntime.CHARSET));
        while (data.hasRemaining()) {
            try {
                if (channel.write(data) > 0) {
                    continue;
                }
            } catch (IOException e) {
                throw LoxOps.makeError("NetworkError",
                                       method + "(): " + e.getMessage());
            }
            LoxWorker.waitChannel(channel, SelectionKey.OP_WRITE);
            checkWritable(method);
        }
    }

    /**
     * The INVOKE and bound-native entry points. The stream state is checked
     * before the argument type, matching native's checkSocketWrite-then-type
     * order, so a non-String write to a closed socket reports the same
     * error on every backend.
     */
    public void writeArg(Object arg) {
        checkWritable("write");
        write(checkStringArg(arg, "write"));
    }

    public void writelineArg(Object arg) {
        checkWritable("writeline");
        writeline(checkStringArg(arg, "writeline"));
    }

    /**
     * Half-close: sends EOF to the peer, leaves the read side open. A second
     * call, and a call on an already closed socket, do nothing.
     */
    public void closeWrite() {
        if (channel == null || writeClosed) {
            return;
        }
        try {
            channel.shutdownOutput();
        } catch (IOException ignored) {
            // Best effort, matching shutdown's own idempotence.
        }
        writeClosed = true;
    }

    public void close() {
        if (channel != null) {
            LoxWorker.wakeChannel(channel);
            try {
                channel.close();
            } catch (IOException ignored) {
                // Matches ObjSocket: close is best-effort, never raises.
            }
            channel = null;
        }
    }

    /** Resolves a host name to its addresses; replaced only by tests. */
    interface Resolver {
        InetAddress[] resolve(String host) throws IOException;
    }

    static volatile Resolver resolver = InetAddress::getAllByName;

    public static LoxSocket connect(String host, int port) {
        return connect(host, port, 0);
    }

    /**
     * {@code limitNanos} above 0 is the limit of the whole call; the call
     * raises a NetworkError when it passes before the connection is made.
     */
    public static LoxSocket connect(String host, int port, long limitNanos) {
        final boolean timed = limitNanos > 0;
        final long deadline = System.nanoTime() + limitNanos;
        // Every resolved address is tried in order, as native does: the first
        // one can be unusable (an IPv6 address on a host with IPv6 off) while a
        // later one works. The last failure is the one reported.
        String failure = "unknown host";
        LoxWorker.beginWaitingCall();
        try {
            // A host name resolves here and blocks the worker; a numeric
            // address does not (spec/05-stdlib.md, connect).
            for (InetAddress ip : resolver.resolve(host)) {
                if (timed && deadline - System.nanoTime() <= 0) {
                    throw LoxOps.makeError("NetworkError",
                                           timeoutMessage(host, port));
                }
                SocketChannel channel = null;
                try {
                    channel = SocketChannel.open();
                    channel.configureBlocking(false);
                    InetSocketAddress address = new InetSocketAddress(ip, port);
                    if (!channel.connect(address)) {
                        while (!channel.finishConnect()) {
                            if (timed && deadline - System.nanoTime() <= 0) {
                                throw LoxOps.makeError("NetworkError",
                                                       timeoutMessage(host, port));
                            }
                            LoxWorker.waitChannel(channel, SelectionKey.OP_CONNECT,
                                                  timed, deadline);
                        }
                    }
                    LoxSocket made = new LoxSocket(channel);
                    channel = null; // the Socket owns the channel now
                    return made;
                } catch (IOException e) {
                    failure = e.getMessage() != null ? e.getMessage() : failure;
                } finally {
                    if (channel != null) {
                        try {
                            channel.close();
                        } catch (IOException ignored) {
                            // The connect fault is the one to report.
                        }
                    }
                }
            }
        } catch (IOException e) {
            failure = e.getMessage() != null ? e.getMessage() : failure;
        }
        throw LoxOps.makeError("NetworkError",
                               "connect(): cannot connect to '" + host + ":" +
                                   port + "': " + failure);
    }

    private static String timeoutMessage(String host, int port) {
        return "connect_within(): timed out connecting to '" + host + ":" +
               port + "'";
    }

    /**
     * A fresh method value on every call, matching native's GET_PROPERTY,
     * which wraps a new ObjBoundNative on every read.
     */
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
        case "read":
            return new LoxNative("read", 0, a -> read());
        case "read_bytes":
            return new LoxNative("read_bytes", 1, a -> readBytesArg(a[0]));
        case "readline":
            return new LoxNative("readline", 0, a -> readline());
        case "readlines":
            return new LoxNative("readlines", 0, a -> readlines());
        case "write":
            return new LoxNative("write", 1, a -> {
                writeArg(a[0]);
                return null;
            });
        case "writeline":
            return new LoxNative("writeline", 1, a -> {
                writelineArg(a[0]);
                return null;
            });
        case "close_write":
            return new LoxNative("close_write", 0, a -> {
                closeWrite();
                return null;
            });
        case "close":
            return new LoxNative("close", 0, a -> {
                close();
                return null;
            });
        default:
            return null;
        }
    }

    static String checkStringArg(Object v, String method) {
        if (!(v instanceof String)) {
            throw new LoxError("'" + method + "' argument must be a string.");
        }
        return (String)v;
    }

    /**
     * A non-negative integer byte count, mirroring native net_api.cpp's
     * asByteCount. Java has no separate integer type, so an integral Double in
     * the int range is the accepted shape.
     */
    static int checkByteCountArg(Object v, String method) {
        if (!(v instanceof Double)) {
            throw new LoxError(method + "() byte count must be a number.");
        }
        double raw = (Double)v;
        if (Double.isNaN(raw) || Double.isInfinite(raw) ||
            raw != Math.floor(raw) || raw < 0 || raw > Integer.MAX_VALUE) {
            throw new LoxError(method +
                "() byte count must be an integer in range 0 to 2147483647.");
        }
        return (int)raw;
    }
}
