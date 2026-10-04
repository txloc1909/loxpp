package lox;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Mirrors src/stdlib/process_api.cpp's ObjProcess over a Java
 * {@link Process}. Child output crosses this boundary as
 * {@link LoxRuntime#CHARSET}, the same byte-boundary rule {@link LoxFile}
 * follows.
 */
public final class LoxProcess {
    private final Process process;
    private final InputStream stdout;
    private final InputStream stderr;
    private OutputStream stdin; // null once closed
    private boolean reaped;
    private int status;

    public LoxProcess(Process process) {
        this.process = process;
        this.stdin = process.getOutputStream();
        this.stdout = new BufferedInputStream(process.getInputStream());
        this.stderr = new BufferedInputStream(process.getErrorStream());
    }

    private static String[] command(String program, LoxList args) {
        List<String> cmd = new ArrayList<>();
        cmd.add(program);
        for (Object a : args.elements) {
            if (!(a instanceof String)) {
                throw new LoxError("args must be a list of strings.");
            }
            cmd.add((String)a);
        }
        return cmd.toArray(new String[0]);
    }

    public static LoxProcess spawn(String program, LoxList args) {
        try {
            return new LoxProcess(
                new ProcessBuilder(command(program, args)).start());
        } catch (IOException e) {
            throw new LoxError("spawn(): cannot run '" + program + "': " +
                               e.getMessage());
        }
    }

    public static LoxMap run(String program, LoxList args) {
        LoxProcess p = spawn(program, args);
        // run() is non-interactive: close the child's stdin so a reader such
        // as `cat` sees EOF instead of blocking.
        p.closeStdin();

        // Drain stderr on its own thread, so a child that fills both pipe
        // buffers cannot deadlock against a parent reading only stdout.
        StringBuilder err = new StringBuilder();
        Thread t = new Thread(() -> {
            try {
                err.append(readAll(p.stderr));
            } catch (IOException ignored) {
                // A read failure leaves the partial text; matching native,
                // which does not turn a stderr read failure into a Lox fault.
            }
        });
        t.start();
        String out;
        try {
            out = readAll(p.stdout);
        } catch (IOException e) {
            throw new LoxError("run(): " + e.getMessage());
        }
        try {
            t.join();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }

        LoxMap map = new LoxMap();
        map.put("status", (double)p.waitStatus());
        map.put("stdout", out);
        map.put("stderr", err.toString());
        return map;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int n;
        while ((n = in.read(chunk)) != -1) {
            buf.write(chunk, 0, n);
        }
        return new String(buf.toByteArray(), LoxRuntime.CHARSET);
    }

    private static Object readLine(InputStream in) {
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

    private static LoxList readLines(InputStream in) {
        LoxList list = new LoxList();
        Object line;
        while ((line = readLine(in)) != null) {
            list.elements.add(line);
        }
        return list;
    }

    private void checkWritable(String method) {
        if (reaped) {
            throw new LoxError("Cannot call '" + method + "' after wait().");
        }
        if (stdin == null) {
            throw new LoxError(
                "Cannot call '" + method + "' on a closed process stdin.");
        }
    }

    public Object read() {
        try {
            return readAll(stdout);
        } catch (IOException e) {
            throw new LoxError("read(): " + e.getMessage());
        }
    }

    public Object readBytesArg(Object arg) {
        return LoxSocket.readBytesFrom(
            stdout, LoxSocket.checkByteCountArg(arg, "read_bytes"),
            "read_bytes");
    }

    public Object readline() {
        return readLine(stdout);
    }

    public LoxList readlines() {
        return readLines(stdout);
    }

    public Object readErr() {
        try {
            return readAll(stderr);
        } catch (IOException e) {
            throw new LoxError("read_err(): " + e.getMessage());
        }
    }

    public Object errReadBytesArg(Object arg) {
        return LoxSocket.readBytesFrom(
            stderr, LoxSocket.checkByteCountArg(arg, "err_read_bytes"),
            "err_read_bytes");
    }

    public Object errReadline() {
        return readLine(stderr);
    }

    public LoxList errReadlines() {
        return readLines(stderr);
    }

    public void write(String s) {
        checkWritable("write");
        try {
            stdin.write(s.getBytes(LoxRuntime.CHARSET));
            stdin.flush();
        } catch (IOException e) {
            throw new LoxError("write(): " + e.getMessage());
        }
    }

    public void writeline(String s) {
        write(s + "\n");
    }

    /**
     * The INVOKE and bound-native entry points. The stream state is checked
     * before the argument type, matching native's checkProcess-then-type
     * order, so a non-String write to a closed or reaped process reports the
     * same fatal error on every backend.
     */
    public void writeArg(Object arg) {
        checkWritable("write");
        write(LoxSocket.checkStringArg(arg, "write"));
    }

    public void writelineArg(Object arg) {
        checkWritable("writeline");
        writeline(LoxSocket.checkStringArg(arg, "writeline"));
    }

    public void closeStdin() {
        if (stdin != null) {
            try {
                stdin.close();
            } catch (IOException ignored) {
                // Best effort, matching the native close.
            }
            stdin = null;
        }
    }

    public double waitStatus() {
        if (!reaped) {
            try {
                status = process.waitFor();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new LoxError("wait(): interrupted.");
            }
            reaped = true;
        }
        return status;
    }

    public void kill() {
        if (!reaped) {
            process.destroyForcibly();
        }
    }

    public double pid() {
        return process.pid();
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
        case "read":
            return new LoxNative("read", 0, a -> read());
        case "read_bytes":
            return new LoxNative("read_bytes", 1, a -> readBytesArg(a[0]));
        case "readline":
            return new LoxNative("readline", 0, a -> readline());
        case "readlines":
            return new LoxNative("readlines", 0, a -> readlines());
        case "read_err":
            return new LoxNative("read_err", 0, a -> readErr());
        case "err_read_bytes":
            return new LoxNative("err_read_bytes", 1,
                                 a -> errReadBytesArg(a[0]));
        case "err_readline":
            return new LoxNative("err_readline", 0, a -> errReadline());
        case "err_readlines":
            return new LoxNative("err_readlines", 0, a -> errReadlines());
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
        case "close_stdin":
            return new LoxNative("close_stdin", 0, a -> {
                closeStdin();
                return null;
            });
        case "wait":
            return new LoxNative("wait", 0, a -> waitStatus());
        case "kill":
            return new LoxNative("kill", 0, a -> {
                kill();
                return null;
            });
        case "pid":
            return new LoxNative("pid", 0, a -> pid());
        default:
            return null;
        }
    }
}
