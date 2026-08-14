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

        System.exit(TestSupport.finish("FileTest"));
    }
}
