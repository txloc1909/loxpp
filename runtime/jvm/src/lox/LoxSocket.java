package lox;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;

/**
 * Mirrors src/stdlib/net_api.cpp's ObjSocket over a {@link Socket}. Text
 * crosses this boundary as {@link LoxRuntime#CHARSET}, the same byte-boundary
 * rule {@link LoxFile} follows.
 */
public final class LoxSocket {
    private Socket socket; // null once closed
    private final InputStream in;
    private final OutputStream out;
    private boolean writeClosed;

    public LoxSocket(Socket socket) throws IOException {
        this.socket = socket;
        this.in = new BufferedInputStream(socket.getInputStream());
        this.out = socket.getOutputStream();
    }

    private void checkOpen(String method) {
        if (socket == null) {
            throw new LoxError("Cannot call '" + method + "' on a closed socket.");
        }
    }

    private void checkWritable(String method) {
        checkOpen(method);
        if (writeClosed) {
            throw new LoxError("Cannot write to a socket after close_write().");
        }
    }

    public Object read() {
        checkOpen("read");
        try {
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            byte[] chunk = new byte[4096];
            int n;
            while ((n = in.read(chunk)) != -1) {
                buf.write(chunk, 0, n);
            }
            return new String(buf.toByteArray(), LoxRuntime.CHARSET);
        } catch (IOException e) {
            throw new LoxError("read(): " + e.getMessage());
        }
    }

    /**
     * Up to {@code n} bytes. Blocks until {@code n} bytes have arrived or the
     * peer closes, then returns what was read; "" at EOF, matching native's
     * readStreamBytes.
     */
    public Object readBytes(int n) {
        checkOpen("read_bytes");
        try {
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
        } catch (IOException e) {
            throw new LoxError("read_bytes(): " + e.getMessage());
        }
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
        try {
            StringBuilder line = new StringBuilder();
            boolean sawByte = false;
            int b;
            while ((b = in.read()) != -1) {
                sawByte = true;
                if (b == '\n') {
                    break;
                }
                line.append((char)(b & 0xFF));
            }
            return sawByte ? line.toString() : null;
        } catch (IOException e) {
            throw new LoxError("readline(): " + e.getMessage());
        }
    }

    public Object readlines() {
        checkOpen("readlines");
        LoxList list = new LoxList();
        Object line;
        while ((line = readline()) != null) {
            list.elements.add(line);
        }
        return list;
    }

    public void write(String s) {
        checkWritable("write");
        try {
            out.write(s.getBytes(LoxRuntime.CHARSET));
            out.flush();
        } catch (IOException e) {
            throw new LoxError("write(): " + e.getMessage());
        }
    }

    public void writeline(String s) {
        write(s + "\n");
    }

    /**
     * The INVOKE and bound-native entry points. The stream state is checked
     * before the argument type, matching native's checkSocketWrite-then-type
     * order, so a non-String write to a closed socket reports the same fatal
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
        if (socket == null || writeClosed) {
            return;
        }
        try {
            socket.shutdownOutput();
        } catch (IOException ignored) {
            // Best effort, matching shutdown's own idempotence.
        }
        writeClosed = true;
    }

    public void close() {
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException ignored) {
                // Matches ObjSocket: close is best-effort, never raises.
            }
            socket = null;
        }
    }

    public static LoxSocket connect(String host, int port) {
        try {
            return new LoxSocket(new Socket(host, port));
        } catch (IOException e) {
            throw new LoxError("connect(): cannot connect to '" + host + ":" +
                               port + "': " + e.getMessage());
        }
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
            throw new LoxError(
                method + "() byte count must be a non-negative integer.");
        }
        return (int)raw;
    }
}
