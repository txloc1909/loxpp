package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.File;
import java.io.IOException;

public final class FileTest {
    public static void main(String[] args) throws IOException {
        File tmp = File.createTempFile("lox-rt-file-test", ".txt");
        tmp.deleteOnExit();
        String path = tmp.getAbsolutePath();

        LoxFile writer = LoxFile.open(path, "w");
        check(writer.getMethod("write") == writer.getMethod("write"),
                "getMethod caches: repeated access returns the same object (PR #97 R3)");
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

        // PR #97 R1: file I/O must round-trip a raw high byte 1:1, never as
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

        // Same call, dispatched through LoxOps.invoke's no-allocation path (R3).
        LoxFile forInvoke = LoxFile.open(rwTmp.getAbsolutePath(), "r");
        checkEquals("seed", LoxOps.invoke(forInvoke, "read", new Object[0]), "invoke() dispatches file methods directly");
        forInvoke.close();

        System.exit(TestSupport.finish("FileTest"));
    }
}
