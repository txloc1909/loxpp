// test_file_nul.cpp — native File.read/readline/readlines must handle NUL
// bytes. Regression guard for issue #401.

#include "test_harness.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

// Runs `source` with stdout redirected into a temporary file, stores the bytes
// written to stdout into `out`, restores the original stream, and returns
// whether the capture succeeded.
bool run_capture_stdout(const std::string& source, std::string& out) {
    int saved_fd = dup(STDOUT_FILENO);
    if (saved_fd < 0) {
        return false;
    }

    FILE* tmp = tmpfile();
    if (tmp == nullptr) {
        close(saved_fd);
        return false;
    }

    // Point the C stdout stream at the temp file, flushing the original first.
    std::fflush(stdout);
    if (dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        std::fclose(tmp);
        close(saved_fd);
        return false;
    }

    VMTestHarness h;
    InterpretResult result = h.run(source);
    std::fflush(stdout);

    // Restore the original stdout before reading, so test logs are not lost.
    dup2(saved_fd, STDOUT_FILENO);
    close(saved_fd);

    if (result != InterpretResult::OK) {
        std::fclose(tmp);
        return false;
    }

    std::rewind(tmp);
    out.clear();
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, tmp)) > 0) {
        out.append(buf, n);
    }
    std::fclose(tmp);
    return true;
}

// Write a test file with the given content (binary-safe).
bool write_test_file(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

// Helper to create expected strings with embedded NULs.
// The length must be the exact number of content bytes (not including string
// literal's terminating NUL).
std::string expected(const char* data, size_t content_len) {
    return std::string(data, content_len);
}

} // namespace

TEST(FileNul, ReadReturnsAllBytesIncludingNul) {
    // Create file: "A\0BCDEFGH" (9 bytes)
    std::string path = "test_file_nul_read.bin";
    std::string content("A\0BCDEFGH", 9);
    ASSERT_TRUE(write_test_file(path, content));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var data = f.read();\n"
                                       "f.close();\n"
                                       "print len(data);\n"
                                       "print data[0];\n"
                                       "print data[1];\n"
                                       "print data[2];\n"
                                       "print data[8];\n",
                                   out));
    // Expected: "9\nA\n\0\nB\nH\n" = 10 bytes content
    EXPECT_EQ(out, expected("9\nA\n\0\nB\nH\n", 10));
}

TEST(FileNul, ReadlinePreservesNulInLine) {
    // Create file: "line1\0with\0nuls\nline2\n" (with NULs in first line)
    // "line1" (5) + \0 (1) + "with" (4) + \0 (1) + "nuls" (4) + \n (1) +
    // "line2" (5) + \n (1) = 22
    std::string path = "test_file_nul_readline.bin";
    std::string content("line1\0with\0nuls\nline2\n", 22);
    ASSERT_TRUE(write_test_file(path, content));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var line1 = f.readline();\n"
                                       "var line2 = f.readline();\n"
                                       "f.close();\n"
                                       "print len(line1);\n"
                                       "print line1;\n"
                                       "print len(line2);\n"
                                       "print line2;\n",
                                   out));
    // First line: "line1\0with\0nuls" (15 bytes, no trailing \n)
    // Second line: "line2" (5 bytes)
    // "15\nline1\0with\0nuls\n5\nline2\n" = 27 bytes content
    EXPECT_EQ(out, expected("15\nline1\0with\0nuls\n5\nline2\n", 27));
}

TEST(FileNul, ReadlinesPreservesNulInLines) {
    // Create file: "A\0B\nC\0D\nE\n" (multiple lines with NULs)
    // "A\0B" (3) + \n (1) + "C\0D" (3) + \n (1) + "E" (1) + \n (1) = 10 bytes
    std::string path = "test_file_nul_readlines.bin";
    std::string content("A\0B\nC\0D\nE\n", 10);
    ASSERT_TRUE(write_test_file(path, content));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var lines = f.readlines();\n"
                                       "f.close();\n"
                                       "print len(lines);\n"
                                       "print len(lines[0]);\n"
                                       "print lines[0];\n"
                                       "print len(lines[1]);\n"
                                       "print lines[1];\n"
                                       "print len(lines[2]);\n"
                                       "print lines[2];\n",
                                   out));
    // 3 lines: "A\0B", "C\0D", "E"
    // "3\n3\nA\0B\n3\nC\0D\n1\nE\n" = 18 bytes content
    EXPECT_EQ(out, expected("3\n3\nA\0B\n3\nC\0D\n1\nE\n", 18));
}

TEST(FileNul, ReadEmptyFileReturnsEmptyString) {
    std::string path = "test_file_nul_empty.bin";
    ASSERT_TRUE(write_test_file(path, ""));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var data = f.read();\n"
                                       "f.close();\n"
                                       "print len(data);\n",
                                   out));
    EXPECT_EQ(out, "0\n");
}

TEST(FileNul, ReadlineAtEofReturnsNil) {
    std::string path = "test_file_nul_eof.bin";
    std::string content("hello\n", 6);
    ASSERT_TRUE(write_test_file(path, content));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var l1 = f.readline();\n"
                                       "var l2 = f.readline();\n"
                                       "f.close();\n"
                                       "print l1 == nil;\n"
                                       "print l2 == nil;\n",
                                   out));
    EXPECT_EQ(out, "false\ntrue\n");
}