package lox;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.Paths;

/**
 * Mirrors src/stdlib/file_api.cpp's ObjFile over a RandomAccessFile, which is
 * the one Java I/O class that supports both "r" and "r+" read/write access
 * the way a C FILE* does. Text crosses this boundary as
 * {@link LoxRuntime#CHARSET} — see that class's byte-boundary rule.
 */
public final class LoxFile {
    private RandomAccessFile raf; // null once closed
    public final boolean readable;
    public final boolean writable;
    private boolean isDirectory; // true only for a directory opened with "r" mode

    private LoxFile(RandomAccessFile raf, boolean readable, boolean writable, boolean isDirectory) {
        this.raf = raf;
        this.readable = readable;
        this.writable = writable;
        this.isDirectory = isDirectory;
    }

    public static LoxFile open(String path, String mode) {
        boolean readable;
        boolean writable;
        boolean truncate = false;
        boolean append = false;
        switch (mode) {
        case "r":
            readable = true;
            writable = false;
            break;
        case "w":
            readable = false;
            writable = true;
            truncate = true;
            break;
        case "a":
            readable = false;
            writable = true;
            append = true;
            break;
        case "r+":
            readable = true;
            writable = true;
            break;
        default:
            throw new LoxError(
                    "open(): invalid mode. Expected \"r\", \"w\", \"a\", or \"r+\".");
        }
        // Linux fopen(path, "r") succeeds on a directory; only a later read(2)
        // fails with EISDIR. RandomAccessFile throws at construction time for
        // a directory, so detect it first and defer to the read methods.
        // Only mode "r" can open a directory; "w", "a", "r+" still raise.
        if (mode.equals("r") && isLinux()) {
            if (isDirectoryPath(path)) {
                return new LoxFile(null, true, false, true);
            }
        }
        try {
            RandomAccessFile raf = new RandomAccessFile(path, readable && !writable ? "r" : "rw");
            if (truncate) {
                raf.setLength(0);
            }
            if (append) {
                raf.seek(raf.length());
            }
            return new LoxFile(raf, readable, writable, false);
        } catch (IOException e) {
            throw new LoxError("open(): cannot open '" + path + "': " + e.getMessage());
        }
    }

    private static boolean isLinux() {
        String os = System.getProperty("os.name", "");
        return os.toLowerCase(java.util.Locale.ROOT).contains("linux");
    }

    private static boolean isDirectoryPath(String path) {
        try {
            return Files.isDirectory(Paths.get(path));
        } catch (RuntimeException e) {
            return false;
        }
    }

    private void checkOpen(String method) {
        if (raf == null && !isDirectory) {
            throw new LoxError("Cannot call '" + method + "' on a closed file.");
        }
    }

    public Object read() {
        checkOpen("read");
        if (!readable) {
            throw new LoxError("File is not open for reading.");
        }
        // A directory opened with "r": read(2) fails with EISDIR on Linux.
        // Return "" to match native fopen/read behavior; each read gives "".
        if (isDirectory) {
            return "";
        }
        try {
            byte[] buf = new byte[(int) (raf.length() - raf.getFilePointer())];
            raf.readFully(buf);
            return new String(buf, LoxRuntime.CHARSET);
        } catch (IOException e) {
            throw new LoxError("read(): " + e.getMessage());
        }
    }

    /** One line, newline stripped; nil at EOF — a partial trailing line still counts as a line. */
    public Object readline() {
        checkOpen("readline");
        if (!readable) {
            throw new LoxError("File is not open for reading.");
        }
        // A directory opened with "r": native fgets gives no line, so nil.
        if (isDirectory) {
            return null;
        }
        try {
            StringBuilder line = new StringBuilder();
            boolean sawByte = false;
            int b;
            while ((b = raf.read()) != -1) {
                sawByte = true;
                if (b == '\n') {
                    break;
                }
                line.append((char) (b & 0xFF));
            }
            return sawByte ? line.toString() : null;
        } catch (IOException e) {
            throw new LoxError("readline(): " + e.getMessage());
        }
    }

    public Object readlines() {
        checkOpen("readlines");
        if (!readable) {
            throw new LoxError("File is not open for reading.");
        }
        LoxList list = new LoxList();
        Object line;
        while ((line = readline()) != null) {
            list.elements.add(line);
        }
        return list;
    }

    public void write(String s) {
        checkOpen("write");
        if (!writable) {
            throw new LoxError("File is not open for writing.");
        }
        try {
            raf.write(s.getBytes(LoxRuntime.CHARSET));
        } catch (IOException e) {
            throw new LoxError("write(): " + e.getMessage());
        }
    }

    public void writeline(String s) {
        write(s);
        write("\n");
    }

    public void close() {
        if (raf != null) {
            try {
                raf.close();
            } catch (IOException ignored) {
                // Matches ObjFile: close is best-effort, never raises.
            }
            raf = null;
        }
        isDirectory = false;
    }

    /**
     * A fresh method value on every call, matching src/vm.cpp's
     * Op::GET_PROPERTY, which wraps a new ObjBoundNative on every read
     * (the isFile branch). No two reads give the same object, so
     * LoxOps.equal's reference-identity rule gives false for a repeat read
     * of one file and for a read of two different files.
     */
    public LoxCallable getMethod(String name) {
        LoxCallable unbound = createMethod(name);
        if (unbound == null) {
            return null;
        }
        // Every branch of createMethod returns a LoxNative; re-wrap it with
        // this file as its receiver so type() can tell a bound File method
        // apart from an ordinary, unbound native — see LoxNative.receiver.
        LoxNative n = (LoxNative) unbound;
        return new LoxNative(n.name, n.arity, n::call, this);
    }

    private LoxCallable createMethod(String name) {
        switch (name) {
        case "read":
            return new LoxNative("read", 0, a -> read());
        case "readline":
            return new LoxNative("readline", 0, a -> readline());
        case "readlines":
            return new LoxNative("readlines", 0, a -> readlines());
        case "write":
            return new LoxNative("write", 1, a -> {
                write(checkStringArg(a[0], "write"));
                return null;
            });
        case "writeline":
            return new LoxNative("writeline", 1, a -> {
                writeline(checkStringArg(a[0], "writeline"));
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

    private static String checkStringArg(Object v, String method) {
        if (!(v instanceof String)) {
            throw new LoxError("'" + method + "' argument must be a string.");
        }
        return (String) v;
    }
}
