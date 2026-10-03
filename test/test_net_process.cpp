// test_net_process.cpp — the sockets and subprocess standard-library natives.
//
// Invariants under test:
//   1. connect/listen — a loopback client and server exchange bytes.
//   2. Socket.close_write() sends EOF while leaving the read side open.
//   3. spawn — writes reach the child and reads return its output.
//   4. run   — captures status, stdout, and stderr together.
//   5. wait()/kill() and pid() — a child is reaped with a real status.
//   6. Fatal errors — bad arguments and failed launches halt the program.

#include "test_harness.h"
#include <gtest/gtest.h>

#include <string>

// ---------------------------------------------------------------------------
// type() of the new values
// ---------------------------------------------------------------------------

TEST(NetProcessTest, TypeNames) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "type(l) + \" \" + type(c) + \" \" + "
                    "type(spawn(\"/bin/echo\", [])) + \" \" + "
                    "type(run(\"/bin/echo\", []));"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "Server Socket Process Map");
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

TEST(NetProcessTest, SocketEchoRoundTrip) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.writeline(\"ping\"); "
                    "c.close_write(); "
                    "var s = l.accept(); "
                    "var got = s.read(); "
                    "s.write(\"pong\"); "
                    "s.close(); "
                    "var back = c.read(); "
                    "c.close(); "
                    "l.close(); "
                    "(got == \"ping\\n\") and (back == \"pong\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// readline() stops at '\n' without reaching EOF, so this pins the case the
// echo round-trip above misses: a socket must accept a write after a
// non-EOF read. The client sends a second line up front so the read-ahead
// buffer still holds bytes when the server writes — stdio's r+ update mode
// would then seek to switch direction and fail with ESPIPE on a socket.
TEST(NetProcessTest, SocketReadlineThenWriteInterleaves) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.write(\"ping\\npong\\n\"); "
                    "var s = l.accept(); "
                    "var got = s.readline(); "
                    "s.writeline(\"ok\"); "
                    "var back = c.readline(); "
                    "s.close(); "
                    "c.close(); "
                    "l.close(); "
                    "(got == \"ping\") and (back == \"ok\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, ServerPortIsEphemeralAndStable) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "(l.port() > 0) and (l.port() == l.port());"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, SocketReadlines) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.write(\"a\\nb\\n\"); "
                    "c.close_write(); "
                    "var s = l.accept(); "
                    "var lines = s.readlines(); "
                    "str(lines);"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "[a, b]");
}

TEST(NetProcessTest, WriteAfterCloseWriteFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.close_write(); "
                    "c.write(\"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, WriteToClosedPeerFatalNotSignal) {
    // Two writes after the peer closes: the second must fail with a runtime
    // error. Before the SIGPIPE fix the VM died with signal 13 (exit 141).
    VMTestHarness h;
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "s.close(); "
                    "c.write(\"x\"); "
                    "c.write(\"y\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, CloseWriteIsIdempotentAndSafeAfterClose) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "c.close_write(); "
                    "c.close_write(); "
                    "c.close(); "
                    "c.close_write(); "
                    "s.close(); "
                    "l.close(); "
                    "\"ok\";"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "ok");
}

TEST(NetProcessTest, WriteToExitedChildFatalNotSignal) {
    // The child exits without reading stdin, so the pipe has no reader. The
    // write must be a runtime error, not a SIGPIPE death.
    VMTestHarness h;
    EXPECT_EQ(h.run("var p = spawn(\"/bin/sh\", [\"-c\", \"exit 0\"]); "
                    "sleep(0.2); "
                    "p.write(\"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, AcceptOnClosedServerFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "l.close(); "
                    "l.accept();"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, ConnectRefusedFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var port = l.port(); "
                    "l.close(); "
                    "connect(\"127.0.0.1\", port);"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, InvalidPortFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("connect(\"127.0.0.1\", 0);"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("listen(\"127.0.0.1\", 70000);"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("listen(\"127.0.0.1\", 1.5);"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

TEST(NetProcessTest, RunCapturesStatusStdoutStderr) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = run(\"/bin/sh\", "
                    "[\"-c\", \"printf out; printf err 1>&2; exit 3\"]); "
                    "(r[\"status\"] == 3) and (r[\"stdout\"] == \"out\") "
                    "and (r[\"stderr\"] == \"err\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, SpawnWriteReadWait) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/cat\", []); "
                    "p.writeline(\"hi\"); "
                    "p.close_stdin(); "
                    "var line = p.readline(); "
                    "var status = p.wait(); "
                    "(line == \"hi\") and (status == 0) and (p.pid() > 0);"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, SpawnReadErr) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/sh\", "
                    "[\"-c\", \"printf err 1>&2\"]); "
                    "p.read_err();"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "err");
}

TEST(NetProcessTest, WaitIsIdempotent) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/sh\", [\"-c\", \"exit 5\"]); "
                    "(p.wait() == 5) and (p.wait() == 5);"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, KillReapsWithSignalStatus) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/sleep\", [\"5\"]); "
                    "p.kill(); "
                    "p.wait() == 137;"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, WriteAfterWaitFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var p = spawn(\"/bin/sh\", [\"-c\", \"exit 0\"]); "
                    "p.wait(); "
                    "p.write(\"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, RunLaunchFailureFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("run(\"/no/such/program_loxpp_xyz\", []);"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("spawn(\"/no/such/program_loxpp_xyz\", []);"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, NonListArgsFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("spawn(\"/bin/echo\", \"nope\");"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("run(\"/bin/echo\", [1, 2]);"),
              InterpretResult::RUNTIME_ERROR);
}
