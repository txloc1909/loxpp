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

        return t.Finish("FileTest");
    }
}
