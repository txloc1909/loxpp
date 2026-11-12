// test_file_readline.cpp — a File keeps the bytes it read past the line it
// returned, so the next call returns them. On a stream that cannot seek, the
// bytes cannot be put back into the stream, so the File value holds them.
// Regression guard for issue #1239.

#include "test_harness.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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

    std::fflush(stdout);
    if (dup2(fileno(tmp), STDOUT_FILENO) < 0) {
        std::fclose(tmp);
        close(saved_fd);
        return false;
    }

    VMTestHarness h;
    InterpretResult result = h.run(source);
    std::fflush(stdout);

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

bool write_test_file(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        return false;
    }
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return true;
}

std::string read_test_file(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        return out;
    }
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

// A pipe that already holds `content`, with its write end closed, so the
// program's reads reach the content and then end of input. The program opens
// it through /dev/fd/<fd>: a File on a pipe cannot seek, which is the case
// this test guards, and no open on it blocks.
class Pipe {
  public:
    explicit Pipe(const std::string& content) {
        int fds[2];
        if (pipe(fds) != 0) {
            return;
        }
        ssize_t n = write(fds[1], content.data(), content.size());
        close(fds[1]);
        if (n != static_cast<ssize_t>(content.size())) {
            close(fds[0]);
            return;
        }
        m_read = fds[0];
    }
    ~Pipe() {
        if (m_read >= 0) {
            close(m_read);
        }
    }

    bool ok() const { return m_read >= 0; }
    std::string path() const { return "/dev/fd/" + std::to_string(m_read); }

  private:
    int m_read{-1};
};

} // namespace

TEST(FileReadline, ReadlineTakesTheBytesAnEarlierReadTookAhead) {
    // A block read takes more than one line. The bytes after the returned
    // line stay with the File, so the next call returns them.
    std::string path = "test_file_readline_read_ahead.txt";
    ASSERT_TRUE(write_test_file(path, "first\nsecond\nthird\n"));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "print f.readline();\n"
                                       "print f.readline();\n"
                                       "print f.readline();\n"
                                       "f.close();\n",
                                   out));
    EXPECT_EQ(out, "first\nsecond\nthird\n");
    unlink(path.c_str());
}

TEST(FileReadline, ReadlineOnPipeKeepsTheRestOfTheBlock) {
    // The defect this test guards: on a stream that cannot seek, the bytes a
    // block read took past the returned line were lost, so the second line
    // came back nil.
    Pipe pipe_with("alpha\nbeta\n");
    ASSERT_TRUE(pipe_with.ok());

    std::string out;
    ASSERT_TRUE(run_capture_stdout(
        "var f = open(\"" + pipe_with.path() +
            "\", \"r\");\n"
            "var a = f.readline();\n"
            "var b = f.readline();\n"
            "var c = f.readline();\n"
            "f.close();\n"
            "print str(a) + \"|\" + str(b) + \"|\" + str(c);\n",
        out));
    EXPECT_EQ(out, "alpha|beta|nil\n");
}

TEST(FileReadline, ReadlinesOnPipeReturnsEveryLine) {
    Pipe pipe_with("one\ntwo\nthree\n");
    ASSERT_TRUE(pipe_with.ok());

    std::string out;
    ASSERT_TRUE(run_capture_stdout(
        "var f = open(\"" + pipe_with.path() +
            "\", \"r\");\n"
            "var lines = f.readlines();\n"
            "f.close();\n"
            "print len(lines);\n"
            "print lines[0] + \",\" + lines[1] + \",\" + lines[2];\n",
        out));
    EXPECT_EQ(out, "3\none,two,three\n");
}

TEST(FileReadline, ReadOnPipeTakesTheBytesAnEarlierReadlineKept) {
    Pipe pipe_with("head\nrest of it");
    ASSERT_TRUE(pipe_with.ok());

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + pipe_with.path() +
                                       "\", \"r\");\n"
                                       "print f.readline();\n"
                                       "print f.read();\n"
                                       "f.close();\n",
                                   out));
    EXPECT_EQ(out, "head\nrest of it\n");
}

TEST(FileReadline, ReadAfterReadlineReturnsTheRest) {
    // read() takes the bytes the earlier readline kept before it reads on.
    std::string path = "test_file_readline_read_rest.txt";
    ASSERT_TRUE(write_test_file(path, "one\ntwo\nthree"));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "var first = f.readline();\n"
                                       "print first;\n"
                                       "print f.read();\n"
                                       "f.close();\n",
                                   out));
    EXPECT_EQ(out, "one\ntwo\nthree\n");
    unlink(path.c_str());
}

TEST(FileReadline, ReadlineKeepsATrailingLineWithNoNewline) {
    std::string path = "test_file_readline_trailing.txt";
    ASSERT_TRUE(write_test_file(path, "only\ntail"));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r\");\n"
                                       "print str(f.readline());\n"
                                       "print str(f.readline());\n"
                                       "print str(f.readline());\n"
                                       "f.close();\n",
                                   out));
    EXPECT_EQ(out, "only\ntail\nnil\n");
    unlink(path.c_str());
}

TEST(FileReadline, WriteAfterReadlineLandsWhereTheReadsStopped) {
    // A read may take more than the line it returned, so a write on an update
    // stream must first give those bytes back; otherwise the write lands past
    // them and the file gains a gap.
    std::string path = "test_file_readline_update.txt";
    ASSERT_TRUE(write_test_file(path, "one\ntwo\n"));

    std::string out;
    ASSERT_TRUE(run_capture_stdout("var f = open(\"" + path +
                                       "\", \"r+\");\n"
                                       "print f.readline();\n"
                                       "f.write(\"T\");\n"
                                       "f.close();\n"
                                       "var g = open(\"" +
                                       path +
                                       "\", \"r\");\n"
                                       "print g.read();\n"
                                       "g.close();\n",
                                   out));
    EXPECT_EQ(out, "one\none\nTwo\n\n");
    EXPECT_EQ(read_test_file(path), "one\nTwo\n");
    unlink(path.c_str());
}
