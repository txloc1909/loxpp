package lox;

import static lox.TestSupport.checkEquals;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.PrintStream;

/**
 * PR #97 review finding R1: every string that crosses stdout or stdin must
 * be treated as raw bytes (LoxRuntime.CHARSET), not decoded/encoded as UTF-8
 * text. These checks drive the same logic LoxRuntime.out and input() use,
 * through a ByteArrayOutputStream/ByteArrayInputStream instead of the real
 * file descriptors, so the suite stays hermetic.
 */
public final class RuntimeIOTest {
    public static void main(String[] args) throws Exception {
        // Read the production object itself, not a copy built to the same
        // recipe (PR #97 review finding R10: a check that rebuilds its own
        // PrintStream cannot catch a bug in LoxRuntime.out's own charset).
        checkEquals(LoxRuntime.CHARSET, LoxRuntime.out.charset(),
                "LoxRuntime.out writes raw bytes, not the JVM default charset");

        // Output: a char in 0x80-0xFF must become exactly one byte, not the
        // two-byte UTF-8 sequence the JVM default charset would produce.
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        PrintStream ps = new PrintStream(baos, false, LoxRuntime.CHARSET);
        ps.print(String.valueOf((char) 0xE9));
        ps.flush();
        byte[] written = baos.toByteArray();
        checkEquals(1, written.length, "one Lox++ char of value 0xE9 prints exactly one byte");
        checkEquals((byte) 0xE9, written[0], "the printed byte matches the char's value exactly");

        // Input: a lone high byte must decode to that same byte, not U+FFFD
        // (the replacement character an InputStreamReader would produce when
        // it cannot parse 0xE9 as a UTF-8 continuation byte).
        String line = LoxRuntime.readByteLine(new ByteArrayInputStream(new byte[] {(byte) 0xE9, '\n'}));
        checkEquals(1, line.length(), "readByteLine keeps a lone high byte as one char");
        checkEquals((int) 0xE9, (int) line.charAt(0), "readByteLine does not replace the byte with U+FFFD");

        checkEquals(null, LoxRuntime.readByteLine(new ByteArrayInputStream(new byte[0])),
                "readByteLine returns nil at immediate EOF, matching std::getline");
        checkEquals("abc", LoxRuntime.readByteLine(new ByteArrayInputStream("abc".getBytes(LoxRuntime.CHARSET))),
                "a partial trailing line with no newline still counts as a line");
        checkEquals("", LoxRuntime.readByteLine(new ByteArrayInputStream(new byte[] {'\n', 'x'})),
                "an immediate newline is an empty line, not EOF");

        System.exit(TestSupport.finish("RuntimeIOTest"));
    }
}
