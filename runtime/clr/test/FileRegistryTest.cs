using System;
using System.IO;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Proves that LoxFile's flush-on-exit registry does not grow without bound:
/// Open() adds an entry only for a writable file, and Close() removes it
/// again, rather than leaking one entry per file the program ever opened.
/// </summary>
public static class FileRegistryTest {
    public static int Run() {
        var t = new TestSupport();

        // A read-only file must never enter the registry: FlushAllOpen()
        // only needs to protect buffered writes, and a leaked read handle
        // here would grow forever in a long-running program that only reads.
        string readPath = Path.Combine(Path.GetTempPath(), $"lox-rt-registry-read-{Guid.NewGuid():N}.txt");
        File.WriteAllText(readPath, "seed");
        try {
            int before = LoxFile.OpenRegistryCountForTests;
            LoxFile reader = LoxFile.Open(readPath, "r");
            t.CheckEquals(before, LoxFile.OpenRegistryCountForTests,
                "open(path, \"r\") does not add an entry to the flush-on-exit registry");
            reader.Close();
        } finally {
            File.Delete(readPath);
        }

        // A writable file is added on Open() and removed again on Close() -
        // not just added, since a registry that only grows would hold one
        // FileStream per file a long-running program ever wrote, forever.
        string writePath = Path.Combine(Path.GetTempPath(), $"lox-rt-registry-write-{Guid.NewGuid():N}.txt");
        try {
            int before = LoxFile.OpenRegistryCountForTests;
            LoxFile writer = LoxFile.Open(writePath, "w");
            t.CheckEquals(before + 1, LoxFile.OpenRegistryCountForTests,
                "open(path, \"w\") adds exactly one entry to the flush-on-exit registry");
            writer.Close();
            t.CheckEquals(before, LoxFile.OpenRegistryCountForTests,
                "close() removes the file's entry from the flush-on-exit registry");
        } finally {
            File.Delete(writePath);
        }

        // Closing the same LoxFile twice must not double-remove or throw -
        // Close() is already called this way from user code that closes in
        // a defer after an earlier explicit close.
        string doubleClosePath = Path.Combine(Path.GetTempPath(), $"lox-rt-registry-doubleclose-{Guid.NewGuid():N}.txt");
        try {
            int before = LoxFile.OpenRegistryCountForTests;
            LoxFile writer = LoxFile.Open(doubleClosePath, "w");
            writer.Close();
            writer.Close();
            t.CheckEquals(before, LoxFile.OpenRegistryCountForTests,
                "closing an already-closed file leaves the registry unchanged");
        } finally {
            File.Delete(doubleClosePath);
        }

        return t.Finish("FileRegistryTest");
    }
}
