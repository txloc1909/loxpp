using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Lox;

/// <summary>
/// Mirrors src/stdlib/file_api.cpp's ObjFile over a <see cref="FileStream"/>,
/// read and written one byte at a time so partial-line and EOF behaviour
/// matches the native C FILE* API exactly. Text crosses this boundary as
/// <see cref="LoxRuntime.Charset"/> - see that class's byte-boundary rule.
/// </summary>
public sealed class LoxFile {
    private FileStream m_stream; // null once closed
    public readonly bool Readable;
    public readonly bool Writable;

    // Per-instance cache so repeated GET_PROPERTY reads of the same method
    // name give back the identical object, matching native's ObjNative
    // identity: `f.read == f.read` must be true.
    private readonly Dictionary<string, ILoxCallable> m_methodCache = new();

    private LoxFile(FileStream stream, bool readable, bool writable) {
        m_stream = stream;
        Readable = readable;
        Writable = writable;
    }

    public static LoxFile Open(string path, string mode) {
        bool readable;
        bool writable;
        FileMode fileMode;
        FileAccess access;
        switch (mode) {
        case "r":
            readable = true;
            writable = false;
            fileMode = FileMode.Open;
            access = FileAccess.Read;
            break;
        case "w":
            readable = false;
            writable = true;
            fileMode = FileMode.Create;
            access = FileAccess.Write;
            break;
        case "a":
            readable = false;
            writable = true;
            fileMode = FileMode.Append;
            access = FileAccess.Write;
            break;
        case "r+":
            readable = true;
            writable = true;
            // FileMode.Open, not OpenOrCreate: C fopen(path, "r+") fails
            // when path does not exist, and this mode must fail the same
            // way rather than create the file.
            fileMode = FileMode.Open;
            access = FileAccess.ReadWrite;
            break;
        default:
            throw new LoxError("open(): invalid mode. Expected \"r\", \"w\", \"a\", or \"r+\".");
        }
        try {
            var stream = new FileStream(path, fileMode, access);
            return new LoxFile(stream, readable, writable);
        } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) {
            throw new LoxError($"open(): cannot open '{path}': {e.Message}");
        }
    }

    private void CheckOpen(string method) {
        if (m_stream == null) {
            throw new LoxError($"Cannot call '{method}' on a closed file.");
        }
    }

    public object Read() {
        CheckOpen("read");
        if (!Readable) {
            throw new LoxError("File is not open for reading.");
        }
        try {
            var buf = new List<byte>();
            int b;
            while ((b = m_stream.ReadByte()) != -1) {
                buf.Add((byte)b);
            }
            return LoxRuntime.Charset.GetString(buf.ToArray());
        } catch (IOException e) {
            throw new LoxError($"read(): {e.Message}");
        }
    }

    /// <summary>One line, newline stripped; nil at EOF - a partial trailing line still counts as a line.</summary>
    public object Readline() {
        CheckOpen("readline");
        if (!Readable) {
            throw new LoxError("File is not open for reading.");
        }
        try {
            var line = new StringBuilder();
            bool sawByte = false;
            int b;
            while ((b = m_stream.ReadByte()) != -1) {
                sawByte = true;
                if (b == '\n') {
                    break;
                }
                line.Append((char)(b & 0xFF));
            }
            return sawByte ? line.ToString() : null;
        } catch (IOException e) {
            throw new LoxError($"readline(): {e.Message}");
        }
    }

    public object Readlines() {
        CheckOpen("readlines");
        if (!Readable) {
            throw new LoxError("File is not open for reading.");
        }
        var list = new LoxList();
        object line;
        while ((line = Readline()) != null) {
            list.Elements.Add(line);
        }
        return list;
    }

    public void Write(string s) {
        CheckOpen("write");
        if (!Writable) {
            throw new LoxError("File is not open for writing.");
        }
        try {
            byte[] bytes = LoxRuntime.Charset.GetBytes(s);
            m_stream.Write(bytes, 0, bytes.Length);
        } catch (IOException e) {
            throw new LoxError($"write(): {e.Message}");
        }
    }

    public void Writeline(string s) {
        Write(s);
        Write("\n");
    }

    public void Close() {
        if (m_stream != null) {
            try {
                m_stream.Close();
            } catch (IOException) {
                // Matches ObjFile: close is best-effort, never raises.
            }
            m_stream = null;
        }
    }

    public ILoxCallable GetMethod(string name) {
        if (m_methodCache.TryGetValue(name, out ILoxCallable cached)) {
            return cached;
        }
        ILoxCallable created = CreateMethod(name);
        if (created != null) {
            m_methodCache[name] = created;
        }
        return created;
    }

    private ILoxCallable CreateMethod(string name) {
        switch (name) {
        case "read":
            return new LoxNative("read", 0, a => Read());
        case "readline":
            return new LoxNative("readline", 0, a => Readline());
        case "readlines":
            return new LoxNative("readlines", 0, a => Readlines());
        case "write":
            return new LoxNative("write", 1, a => {
                Write(CheckStringArg(a[0], "write"));
                return null;
            });
        case "writeline":
            return new LoxNative("writeline", 1, a => {
                Writeline(CheckStringArg(a[0], "writeline"));
                return null;
            });
        case "close":
            return new LoxNative("close", 0, a => {
                Close();
                return null;
            });
        default:
            return null;
        }
    }

    private static string CheckStringArg(object v, string method) {
        if (v is not string s) {
            throw new LoxError($"'{method}' argument must be a string.");
        }
        return s;
    }
}
