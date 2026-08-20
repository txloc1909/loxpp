// test_os_api.cpp — the OS / world access standard-library natives.
//
// Invariants under test:
//   1. args()  — reflects the program arguments passed to the VM.
//   2. env()   — reads the host environment; nil for undefined variables.
//   3. exit()  — terminates the process with the requested code.
//   4. time()  — reports wall-clock seconds since the Unix epoch.
//   5. sleep() — suspends for roughly the requested duration.
//   6. exists()/is_dir()/is_file()/stat() — file-system metadata.

#include "test_harness.h"
#include "container_objects.h"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// Directives so a temp directory + file are created before each test and
// removed after, giving the file-system assertions a known fixture.
class OsApiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec);
        do {
            ++m_counter;
            m_dir = base / ("loxpp_os_api_" + std::to_string(::getpid()) + "_" +
                            std::to_string(m_counter));
        } while (std::filesystem::exists(m_dir, ec));
        std::filesystem::create_directories(m_dir, ec);
        ASSERT_FALSE(ec);
        m_file = m_dir / "note.txt";
        std::ofstream out(m_file);
        out << "hello loxpp";
        ASSERT_TRUE(out.good());
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    std::filesystem::path m_dir;
    std::filesystem::path m_file;
    static int m_counter;
};

int OsApiTest::m_counter = 0;

// ---------------------------------------------------------------------------
// args()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Args_EmptyByDefault) {
    VMTestHarness h;
    ASSERT_EQ(h.run("args();"), InterpretResult::OK);
    ASSERT_TRUE(isList(h.lastResult()));
    auto* list = asObjList(as<Obj*>(h.lastResult()));
    EXPECT_EQ(list->elements.size(), 0);
}

TEST_F(OsApiTest, Args_ReturnsProgramArgs) {
    VMTestHarness h;
    h.setArgs({"alpha", "beta", "3"});
    ASSERT_EQ(h.run("args();"), InterpretResult::OK);
    ASSERT_TRUE(isList(h.lastResult()));
    auto* list = asObjList(as<Obj*>(h.lastResult()));
    ASSERT_EQ(list->elements.size(), 3);
    EXPECT_EQ(stringify(list->elements[0]), "alpha");
    EXPECT_EQ(stringify(list->elements[1]), "beta");
    EXPECT_EQ(stringify(list->elements[2]), "3");
}

// ---------------------------------------------------------------------------
// env()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Env_ReturnsValue) {
    ::setenv("LOXPP_OS_API_TEST_VAR", "hello-os", 1);
    VMTestHarness h;
    ASSERT_EQ(h.run("env(\"LOXPP_OS_API_TEST_VAR\");"), InterpretResult::OK);
    ASSERT_TRUE(isString(h.lastResult()));
    EXPECT_EQ(stringify(h.lastResult()), "hello-os");
}

TEST_F(OsApiTest, Env_Unknown_ReturnsNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("env(\"LOXPP_OS_API_NO_SUCH_VAR_XYZ\");"),
              InterpretResult::OK);
    EXPECT_TRUE(is<Nil>(h.lastResult()));
}

TEST_F(OsApiTest, Env_NonString_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("env(42);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// exit()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Exit_NonNumber_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("exit(\"no\");"), InterpretResult::RUNTIME_ERROR);
}

// exit() must not trigger the undefined integral conversion for values a
// 32-bit int cannot represent — these reject in-process, never reaching
// std::exit. The scanner has no exponent literals, so out-of-range finite
// values are written longhand and non-finite ones come from the math module.
TEST_F(OsApiTest, Exit_OutOfRange_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("exit(2147483648);"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("exit(-2147483649);"), InterpretResult::RUNTIME_ERROR);
}

TEST_F(OsApiTest, Exit_NonFinite_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("exit(math.inf);"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("exit(math.nan);"), InterpretResult::RUNTIME_ERROR);
}

TEST_F(OsApiTest, Exit_TruncatesFraction) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        VMTestHarness h;
        static_cast<void>(h.run("exit(3.7);"));
        std::_Exit(0); // never reached: exit() must terminate the process
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 3);
}

TEST_F(OsApiTest, Exit_TerminatesWithCode) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        VMTestHarness h;
        static_cast<void>(h.run("exit(7);"));
        std::_Exit(0); // never reached: exit() must terminate the process
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 7);
}

// ---------------------------------------------------------------------------
// time()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Time_ReturnsWallClockSeconds) {
    std::time_t before = std::time(nullptr);
    VMTestHarness h;
    ASSERT_EQ(h.run("time();"), InterpretResult::OK);
    std::time_t after = std::time(nullptr);
    ASSERT_TRUE(is<Number>(h.lastResult()));
    double t = as<Number>(h.lastResult());
    EXPECT_GE(t, static_cast<double>(before) - 1.0);
    EXPECT_LE(t, static_cast<double>(after) + 1.0);
}

// ---------------------------------------------------------------------------
// sleep()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Sleep_Elapses) {
    VMTestHarness h;
    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(h.run("sleep(0.05);"), InterpretResult::OK);
    auto end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end - start).count();
    EXPECT_GE(elapsed, 0.04);
    EXPECT_TRUE(is<Nil>(h.lastResult()));
}

TEST_F(OsApiTest, Sleep_NonNumber_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("sleep(\"long\");"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// exists() / is_dir() / is_file()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Exists_TrueForExistingFile) {
    VMTestHarness h;
    ASSERT_EQ(h.run("exists(\"" + m_file.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST_F(OsApiTest, Exists_TrueForExistingDir) {
    VMTestHarness h;
    ASSERT_EQ(h.run("exists(\"" + m_dir.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST_F(OsApiTest, Exists_FalseForMissing) {
    VMTestHarness h;
    std::filesystem::path missing = m_dir / "nope.lox";
    ASSERT_EQ(h.run("exists(\"" + missing.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);
}

TEST_F(OsApiTest, IsDir_TrueForDirFalseForFile) {
    VMTestHarness h;
    ASSERT_EQ(h.run("is_dir(\"" + m_dir.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
    ASSERT_EQ(h.run("is_dir(\"" + m_file.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);
}

TEST_F(OsApiTest, IsFile_TrueForFileFalseForDir) {
    VMTestHarness h;
    ASSERT_EQ(h.run("is_file(\"" + m_file.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
    ASSERT_EQ(h.run("is_file(\"" + m_dir.string() + "\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);
}

TEST_F(OsApiTest, FsPredicates_NonString_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("exists(1);"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("is_dir(1);"), InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("is_file(1);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// stat()
// ---------------------------------------------------------------------------

TEST_F(OsApiTest, Stat_MissingPath_ReturnsNil) {
    VMTestHarness h;
    std::filesystem::path missing = m_dir / "nope.lox";
    ASSERT_EQ(h.run("stat(\"" + missing.string() + "\");"),
              InterpretResult::OK);
    EXPECT_TRUE(is<Nil>(h.lastResult()));
}

TEST_F(OsApiTest, Stat_RegularFile_Metadata) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = stat(\"" + m_file.string() +
                    "\"); "
                    "m[\"exists\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);

    ASSERT_EQ(h.run("var m = stat(\"" + m_file.string() +
                    "\"); "
                    "m[\"is_file\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);

    ASSERT_EQ(h.run("var m = stat(\"" + m_file.string() +
                    "\"); "
                    "m[\"is_dir\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);

    ASSERT_EQ(h.run("var m = stat(\"" + m_file.string() +
                    "\"); "
                    "m[\"size\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<Number>(h.lastResult()), 11.0);

    ASSERT_EQ(h.run("var m = stat(\"" + m_file.string() +
                    "\"); "
                    "m[\"mtime\"];"),
              InterpretResult::OK);
    EXPECT_GT(as<Number>(h.lastResult()), 0.0);
}

TEST_F(OsApiTest, Stat_Directory_Metadata) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = stat(\"" + m_dir.string() +
                    "\"); "
                    "m[\"is_dir\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);

    ASSERT_EQ(h.run("var m = stat(\"" + m_dir.string() +
                    "\"); "
                    "m[\"is_file\"];"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);
}

TEST_F(OsApiTest, Stat_NonString_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("stat(1);"), InterpretResult::RUNTIME_ERROR);
}