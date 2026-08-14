package lox;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * Mirrors src/stdlib/file_api.cpp's ObjFile over a RandomAccessFile, which is
 * the one Java I/O class that supports both "r" and "r+" read/write access
 * the way a C FILE* does. Text is read/written as ISO-8859-1: Lox++ strings
 * are byte sequences (spec/03-types.md), and that charset is the one Java
 * encoding that maps every byte 0-255 to one char losslessly, so a
 * round-tripped file is byte-identical regardless of content.
 */
public final class LoxFile {
    private RandomAccessFile raf; // null once closed
    public final boolean readable;
    public final boolean writable;

    private LoxFile(RandomAccessFile raf, boolean readable, boolean writable) {
        this.raf = raf;
        this.readable = readable;
        this.writable = writable;
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
        try {
            RandomAccessFile raf = new RandomAccessFile(path, readable && !writable ? "r" : "rw");
            if (truncate) {
                raf.setLength(0);
            }
            if (append) {
                raf.seek(raf.length());
            }
            return new LoxFile(raf, readable, writable);
        } catch (IOException e) {
            throw new LoxError("open(): cannot open '" + path + "': " + e.getMessage());
        }
    }

    private void checkOpen(String method) {
        if (raf == null) {
            throw new LoxError("Cannot call '" + method + "' on a closed file.");
        }
    }

    public Object read() {
        checkOpen("read");
        if (!readable) {
            throw new LoxError("File is not open for reading.");
        }
        try {
            byte[] buf = new byte[(int) (raf.length() - raf.getFilePointer())];
            raf.readFully(buf);
            return new String(buf, StandardCharsets.ISO_8859_1);
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
            raf.write(s.getBytes(StandardCharsets.ISO_8859_1));
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
    }

    public LoxCallable getMethod(String name) {
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
