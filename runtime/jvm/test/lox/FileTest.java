package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

public final class FileTest {
    public static void main(String[] args) throws IOException {
        File tmp = File.createTempFile("lox-rt-file-test", ".txt");
        tmp.deleteOnExit();
        String path = tmp.getAbsolutePath();

        LoxFile writer = LoxFile.open(path, "w");
        check(!LoxOps.equal(writer.getMethod("write"), writer.getMethod("write")),
                "two reads of the same file's 'write' method are not Lox-equal");
        File otherTmp = File.createTempFile("lox-rt-file-test-other", ".txt");
        otherTmp.deleteOnExit();
        LoxFile otherWriter = LoxFile.open(otherTmp.getAbsolutePath(), "w");
        check(!LoxOps.equal(writer.getMethod("write"), otherWriter.getMethod("write")),
                "two different files' 'write' method values are not Lox-equal");
        otherWriter.close();
        writer.writeline("first");
        writer.writeline("second");
        writer.write("third-no-newline");
        writer.close();
        checkThrows(writer::read, LoxError.class, "a closed file rejects further reads");

        LoxFile reader = LoxFile.open(path, "r");
        checkEquals("first", reader.readline(), "readline() returns the first line");
        checkEquals("second", reader.readline(), "readline() advances to the next line");
        checkEquals("third-no-newline", reader.readline(), "a trailing line with no newline still counts");
        checkEquals(null, reader.readline(), "readline() at EOF is nil");
        checkThrows(() -> reader.write("x"), LoxError.class, "a read-only file rejects writes");
        reader.close();

        LoxFile reader2 = LoxFile.open(path, "r");
        LoxList lines = (LoxList) reader2.readlines();
        checkEquals(3, lines.elements.size(), "readlines() returns every line");
        reader2.close();

        LoxFile appender = LoxFile.open(path, "a");
        appender.writeline("fourth");
        appender.close();
        LoxFile reader3 = LoxFile.open(path, "r");
        String all = (String) reader3.read();
        check(all.endsWith("fourth\n"), "append mode adds after existing content");
        reader3.close();

        File rwTmp = File.createTempFile("lox-rt-file-rplus", ".txt");
        rwTmp.deleteOnExit();
        LoxFile seed = LoxFile.open(rwTmp.getAbsolutePath(), "w");
        seed.write("seed");
        seed.close();
        LoxFile rw = LoxFile.open(rwTmp.getAbsolutePath(), "r+");
        checkEquals("seed", rw.read(), "r+ can read existing content");
        rw.close();

        checkThrows(() -> LoxFile.open(path, "bogus"), LoxError.class, "open() rejects an invalid mode");

        // PR #97: file I/O must round-trip a raw high byte 1:1, never as
        // a 2-byte UTF-8 sequence — `open()`'s ISO-8859-1 boundary already
        // guaranteed this; the fix was stdout/stdin catching up to it.
        File byteTmp = File.createTempFile("lox-rt-file-bytes", ".bin");
        byteTmp.deleteOnExit();
        LoxFile byteWriter = LoxFile.open(byteTmp.getAbsolutePath(), "w");
        byteWriter.write(String.valueOf((char) 0xE9));
        byteWriter.close();
        checkEquals(1L, byteTmp.length(), "one Lox++ char of value 0xE9 writes exactly one byte");
        LoxFile byteReader = LoxFile.open(byteTmp.getAbsolutePath(), "r");
        String byteBack = (String) byteReader.read();
        byteReader.close();
        checkEquals(1, byteBack.length(), "the byte reads back as one char, not a replacement pair");
        checkEquals((int) 0xE9, (int) byteBack.charAt(0), "the byte round-trips to its exact value");

        // Same call, dispatched through LoxOps.invoke's no-allocation path.
        LoxFile forInvoke = LoxFile.open(rwTmp.getAbsolutePath(), "r");
        checkEquals("seed", LoxOps.invoke(forInvoke, "read", new Object[0]), "invoke() dispatches file methods directly");
        forInvoke.close();

        // Directory opens: on Linux, fopen(dir, "r") succeeds but read(2)
        // fails with EISDIR. LoxFile.open must return a file and defer to
        // read()/readline()/readlines().
        if (System.getProperty("os.name", "").toLowerCase(Locale.ROOT).contains("linux")) {
            Path dirPath = Files.createTempDirectory("lox-rt-file-dir-test");
            try {
                String dir = dirPath.toString();
                LoxFile dirReader = LoxFile.open(dir, "r");
                check(dirReader != null, "open(directory, \"r\") succeeds and returns a file");
                checkEquals("", dirReader.read(), "read() on a directory gives empty string");
                checkEquals("", dirReader.read(), "read() on a directory again gives empty string");
                checkEquals(null, dirReader.readline(), "readline() on a directory gives nil");
                LoxList emptyList = (LoxList) dirReader.readlines();
                checkEquals(0, emptyList.elements.size(), "readlines() on a directory gives an empty list");
                checkThrows(() -> dirReader.write("x"), LoxError.class,
                        "write() on a read-only directory file raises an error");
                checkThrows(() -> dirReader.writeline("x"), LoxError.class,
                        "writeline() on a read-only directory file raises an error");
                dirReader.close();
                checkThrows(dirReader::read, LoxError.class,
                        "read() on a closed directory file raises an error");
                checkThrows(dirReader::readline, LoxError.class,
                        "readline() on a closed directory file raises an error");
                dirReader.close();
                checkThrows(() -> LoxFile.open(dir, "w"), LoxError.class,
                        "open(directory, \"w\") raises an error");
                checkThrows(() -> LoxFile.open(dir, "a"), LoxError.class,
                        "open(directory, \"a\") raises an error");
                checkThrows(() -> LoxFile.open(dir, "r+"), LoxError.class,
                        "open(directory, \"r+\") raises an error");
                checkThrows(() -> LoxFile.open(dir, "bogus"), LoxError.class,
                        "open(directory, \"bogus\") raises an error for invalid mode");

                Path symlinkPath = dirPath.resolveSibling(
                        dirPath.getFileName() + "-link-" + System.nanoTime());
                try {
                    Files.createSymbolicLink(symlinkPath, dirPath);
                    LoxFile symlinkReader = LoxFile.open(symlinkPath.toString(), "r");
                    check(symlinkReader != null, "open(link to directory, \"r\") succeeds");
                    checkEquals("", symlinkReader.read(),
                            "read() on a link to a directory gives empty string");
                    symlinkReader.close();
                } finally {
                    Files.deleteIfExists(symlinkPath);
                }

                String missing = dirPath.resolve("lox-rt-file-dir-missing-" + System.nanoTime() + ".txt")
                        .toString();
                checkThrows(() -> LoxFile.open(missing, "r"), LoxError.class,
                        "open(missing path, \"r\") raises an error even with directory check");
            } finally {
                Files.deleteIfExists(dirPath);
            }
        }

        // Issue #401: NUL byte handling in File.read/readline/readlines
        // Create a temp file with NUL bytes
        File nulTmp = File.createTempFile("lox-rt-file-nul", ".bin");
        nulTmp.deleteOnExit();
        String nulPath = nulTmp.getAbsolutePath();

        // Test read() with NUL bytes
        LoxFile nulWriter = LoxFile.open(nulPath, "w");
        nulWriter.write("A\0BCDEFGH");
        nulWriter.close();

        LoxFile nulReader = LoxFile.open(nulPath, "r");
        String readData = (String) nulReader.read();
        nulReader.close();
        checkEquals(9, readData.length(), "read() returns all 9 bytes including NUL");
        checkEquals('A', readData.charAt(0), "first byte is 'A'");
        checkEquals('\0', readData.charAt(1), "second byte is NUL");
        checkEquals('B', readData.charAt(2), "third byte is 'B'");
        checkEquals('H', readData.charAt(8), "last byte is 'H'");

        // Test readline() with NULs in line (no trailing newline)
        nulWriter = LoxFile.open(nulPath, "w");
        nulWriter.write("line1\0with\0nuls");
        nulWriter.close();

        nulReader = LoxFile.open(nulPath, "r");
        String line1 = (String) nulReader.readline();
        nulReader.close();
        checkEquals(15, line1.length(), "readline() preserves NULs in line without newline");
        checkEquals("line1\0with\0nuls", line1, "readline() content matches");

        // Test readline() with NULs and trailing newline
        nulWriter = LoxFile.open(nulPath, "w");
        nulWriter.write("line1\0with\0nuls\nline2\n");
        nulWriter.close();

        nulReader = LoxFile.open(nulPath, "r");
        line1 = (String) nulReader.readline();
        String line2 = (String) nulReader.readline();
        nulReader.close();
        checkEquals(15, line1.length(), "first line length with NULs");
        checkEquals("line1\0with\0nuls", line1, "first line content");
        checkEquals(5, line2.length(), "second line length");
        checkEquals("line2", line2, "second line content");

        // Test readlines() with multiple lines containing NULs
        nulWriter = LoxFile.open(nulPath, "w");
        nulWriter.write("A\0B\nC\0D\nE\n");
        nulWriter.close();

        nulReader = LoxFile.open(nulPath, "r");
        LoxList nulLines = (LoxList) nulReader.readlines();
        nulReader.close();
        checkEquals(3, nulLines.elements.size(), "readlines() returns 3 lines");
        checkEquals("A\0B", nulLines.elements.get(0), "first line: A\\0B");
        checkEquals("C\0D", nulLines.elements.get(1), "second line: C\\0D");
        checkEquals("E", nulLines.elements.get(2), "third line: E");

        System.exit(TestSupport.finish("FileTest"));
    }
}
