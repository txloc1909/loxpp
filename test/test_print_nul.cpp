// test_print_nul.cpp — native `print` must write every byte of a string,
// including an embedded NUL byte.
//
// Regression guard for issue #129. The print path is `printValue` (fwrite
// of the full string length) plus a trailing `printf("\n")`. A string with
// an embedded NUL must reach stdout byte-for-byte; `printf("%s")`-style
// truncation stops at the first NUL and must not regress here.

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

} // namespace

TEST(PrintNul, EmitsEveryByteIncludingTheEmbeddedNul) {
    std::string out;
    ASSERT_TRUE(run_capture_stdout("print \"a\\0b\";", out));
    EXPECT_EQ(out, std::string("a\0b\n", 4));
}

TEST(PrintNul, EmitsNulOnlyString) {
    std::string out;
    ASSERT_TRUE(run_capture_stdout("print \"\\0\";", out));
    EXPECT_EQ(out, std::string("\0\n", 2));
}