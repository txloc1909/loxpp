using System.IO;
using Lox;

namespace LoxRuntimeTests;

public static class FileTest {
    public static int Run() {
        var t = new TestSupport();

        string path = Path.Combine(Path.GetTempPath(), $"lox-rt-file-test-{System.Guid.NewGuid():N}.txt");
        try {
            LoxFile writer = LoxFile.Open(path, "w");
            t.Check(ReferenceEquals(writer.GetMethod("write"), writer.GetMethod("write")),
                "getMethod caches: repeated access returns the same object");
            writer.Writeline("first");
            writer.Writeline("second");
            writer.Write("third-no-newline");
            writer.Close();
            t.CheckThrows(() => writer.Read(), typeof(LoxError), "a closed file rejects further reads");

            LoxFile reader = LoxFile.Open(path, "r");
            t.CheckEquals("first", reader.Readline(), "readline() returns the first line");
            t.CheckEquals("second", reader.Readline(), "readline() advances to the next line");
            t.CheckEquals("third-no-newline", reader.Readline(), "a trailing line with no newline still counts");
            t.CheckEquals(null, reader.Readline(), "readline() at EOF is nil");
            t.CheckThrows(() => reader.Write("x"), typeof(LoxError), "a read-only file rejects writes");
            reader.Close();

            LoxFile reader2 = LoxFile.Open(path, "r");
            var lines = (LoxList)reader2.Readlines();
            t.CheckEquals(3, lines.Elements.Count, "readlines() returns every line");
            reader2.Close();

            LoxFile appender = LoxFile.Open(path, "a");
            appender.Writeline("fourth");
            appender.Close();
            LoxFile reader3 = LoxFile.Open(path, "r");
            var all = (string)reader3.Read();
            t.Check(all.EndsWith("fourth\n"), "append mode adds after existing content");
            reader3.Close();

            string rwPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-rplus-{System.Guid.NewGuid():N}.txt");
            try {
                LoxFile seed = LoxFile.Open(rwPath, "w");
                seed.Write("seed");
                seed.Close();
                LoxFile rw = LoxFile.Open(rwPath, "r+");
                t.CheckEquals("seed", rw.Read(), "r+ can read existing content");
                rw.Close();

                // Same call, dispatched through LoxOps.Invoke's no-allocation path.
                LoxFile forInvoke = LoxFile.Open(rwPath, "r");
                t.CheckEquals("seed", LoxOps.Invoke(forInvoke, "read", System.Array.Empty<object>()),
                    "invoke() dispatches file methods directly");
                forInvoke.Close();
            } finally {
                File.Delete(rwPath);
            }

            t.CheckThrows(() => LoxFile.Open(path, "bogus"), typeof(LoxError), "open() rejects an invalid mode");

            // r+ must behave like C fopen(path, "r+"): fail when the path
            // does not exist, and - just as importantly - not create it.
            string missingPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-rplus-missing-{System.Guid.NewGuid():N}.txt");
            t.CheckThrows(() => LoxFile.Open(missingPath, "r+"), typeof(LoxError),
                "open(missing path, \"r+\") fails instead of creating the file");
            t.Check(!File.Exists(missingPath), "open(missing path, \"r+\") leaves no file behind even after it fails");

            // The native VM keeps one ObjNative per method name in a
            // class-wide table shared by every ObjFile, so
            // `f1.write == f2.write` is true there even though f1 and f2
            // are different files - through LoxOps.Equal's Lox-level
            // notion of equality, not C# reference equality.
            string otherPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-other-{System.Guid.NewGuid():N}.txt");
            try {
                LoxFile other = LoxFile.Open(otherPath, "w");
                t.Check(LoxOps.Equal(writer.GetMethod("write"), other.GetMethod("write")),
                    "two different files' 'write' method values are Lox-equal");
                t.Check(!LoxOps.Equal(writer.GetMethod("write"), other.GetMethod("close")),
                    "two different method names on files are not Lox-equal");
                other.Close();
            } finally {
                File.Delete(otherPath);
            }

            // File I/O must round-trip a raw high byte 1:1, never as a
            // 2-byte UTF-8 sequence - open()'s Latin1 boundary guarantees
            // this.
            string bytePath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-bytes-{System.Guid.NewGuid():N}.bin");
            try {
                LoxFile byteWriter = LoxFile.Open(bytePath, "w");
                byteWriter.Write(((char)0xE9).ToString());
                byteWriter.Close();
                t.CheckEquals(1L, new FileInfo(bytePath).Length, "one Lox++ char of value 0xE9 writes exactly one byte");
                LoxFile byteReader = LoxFile.Open(bytePath, "r");
                var byteBack = (string)byteReader.Read();
                byteReader.Close();
                t.CheckEquals(1, byteBack.Length, "the byte reads back as one char, not a replacement pair");
                t.CheckEquals((int)0xE9, (int)byteBack[0], "the byte round-trips to its exact value");
            } finally {
                File.Delete(bytePath);
            }
        } finally {
            File.Delete(path);
        }

        // Directory-opening tests: on Linux, fopen(dir, "r") succeeds but
        // read(2) fails with EISDIR. Here, LoxFile.Open must return a LoxFile
        // and defer the error to read()/readline()/readlines() methods.
        // These tests guard against future changes to Directory.Exists.
        if (OperatingSystem.IsLinux()) {
            string dirPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-dir-test-{System.Guid.NewGuid():N}");
            Directory.CreateDirectory(dirPath);
            try {
                // Test 1: open(dir, "r") succeeds and gives a LoxFile.
                LoxFile dirReader = LoxFile.Open(dirPath, "r");
                t.Check(dirReader != null, "open(directory, \"r\") succeeds and returns a LoxFile");

                // Test 2 & 3: read() returns "" each time.
                t.CheckEquals("", dirReader.Read(), "read() on a directory gives empty string");
                t.CheckEquals("", dirReader.Read(), "read() on a directory again gives empty string");

                // Test 4: readline() gives null.
                t.CheckEquals(null, dirReader.Readline(), "readline() on a directory gives nil");

                // Test 5: readlines() gives an empty list.
                var emptyList = (LoxList)dirReader.Readlines();
                t.CheckEquals(0, emptyList.Elements.Count, "readlines() on a directory gives an empty list");

                // Test 6: write() fails (file is not open for writing).
                t.CheckThrows(() => dirReader.Write("x"), typeof(LoxError),
                    "write() on a read-only directory file raises LoxError");

                // Test 7: after close(), read() fails (file is closed).
                dirReader.Close();
                t.CheckThrows(() => dirReader.Read(), typeof(LoxError),
                    "read() on a closed directory file raises LoxError");

                // Test 8: modes "w", "a", "r+" still raise LoxError on a directory.
                t.CheckThrows(() => LoxFile.Open(dirPath, "w"), typeof(LoxError),
                    "open(directory, \"w\") raises LoxError");
                t.CheckThrows(() => LoxFile.Open(dirPath, "a"), typeof(LoxError),
                    "open(directory, \"a\") raises LoxError");
                t.CheckThrows(() => LoxFile.Open(dirPath, "r+"), typeof(LoxError),
                    "open(directory, \"r+\") raises LoxError");

                // Test 9: invalid mode still fails.
                t.CheckThrows(() => LoxFile.Open(dirPath, "bogus"), typeof(LoxError),
                    "open(directory, \"bogus\") raises LoxError for invalid mode");

                // Test 10: symbolic link to directory behaves the same.
                string symlinkPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-symlink-dir-{System.Guid.NewGuid():N}");
                try {
                    File.CreateSymbolicLink(symlinkPath, dirPath);
                    LoxFile symlinkReader = LoxFile.Open(symlinkPath, "r");
                    t.Check(symlinkReader != null, "open(symlink to directory, \"r\") succeeds");
                    t.CheckEquals("", symlinkReader.Read(), "read() on a symlink to directory gives empty string");
                    symlinkReader.Close();
                } finally {
                    if (File.Exists(symlinkPath)) {
                        File.Delete(symlinkPath);
                    }
                }

                // Test 11: missing path still fails with mode "r".
                string missingPath = Path.Combine(Path.GetTempPath(), $"lox-rt-file-dir-missing-{System.Guid.NewGuid():N}");
                t.CheckThrows(() => LoxFile.Open(missingPath, "r"), typeof(LoxError),
                    "open(missing path, \"r\") raises LoxError even with directory check in place");
            } finally {
                Directory.Delete(dirPath);
            }
        }

        return t.Finish("FileTest");
    }
}
