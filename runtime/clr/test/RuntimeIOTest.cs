using System.IO;
using System.Text;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Every string that crosses stdout or stdin must be treated as raw bytes
/// (LoxRuntime.Charset), not decoded/encoded as UTF-8 text. These checks
/// drive the same logic LoxRuntime.Out and input() use, through a
/// MemoryStream instead of the real file descriptors, so the suite stays
/// hermetic.
/// </summary>
public static class RuntimeIOTest {
    public static int Run() {
        var t = new TestSupport();

        // Read the production object itself, not a copy built to the same
        // recipe: a check that rebuilds its own StreamWriter cannot catch a
        // bug in LoxRuntime.Out's own encoding.
        t.CheckEquals(LoxRuntime.Charset, LoxRuntime.Out.Encoding,
            "LoxRuntime.Out writes raw bytes, not the process default encoding");

        // Output: a char in 0x80-0xFF must become exactly one byte, not the
        // two-byte UTF-8 sequence the process default encoding would produce.
        using var baos = new MemoryStream();
        using (var writer = new StreamWriter(baos, LoxRuntime.Charset, 1024, leaveOpen: true)) {
            writer.Write(((char)0xE9).ToString());
        }
        byte[] written = baos.ToArray();
        t.CheckEquals(1, written.Length, "one Lox++ char of value 0xE9 prints exactly one byte");
        t.CheckEquals((byte)0xE9, written[0], "the printed byte matches the char's value exactly");

        // Input: a lone high byte must decode to that same byte, not
        // U+FFFD (the replacement character a UTF-8 reader would produce
        // when it cannot parse 0xE9 as a continuation byte).
        string line = LoxRuntime.ReadByteLine(new MemoryStream(new byte[] { 0xE9, (byte)'\n' }));
        t.CheckEquals(1, line.Length, "readByteLine keeps a lone high byte as one char");
        t.CheckEquals((int)0xE9, (int)line[0], "readByteLine does not replace the byte with U+FFFD");

        t.CheckEquals(null, LoxRuntime.ReadByteLine(new MemoryStream(System.Array.Empty<byte>())),
            "readByteLine returns nil at immediate EOF, matching std::getline");
        t.CheckEquals("abc", LoxRuntime.ReadByteLine(new MemoryStream(LoxRuntime.Charset.GetBytes("abc"))),
            "a partial trailing line with no newline still counts as a line");
        t.CheckEquals("", LoxRuntime.ReadByteLine(new MemoryStream(new byte[] { (byte)'\n', (byte)'x' })),
            "an immediate newline is an empty line, not EOF");

        return t.Finish("RuntimeIOTest");
    }
}
