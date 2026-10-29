// test_net_process.cpp — the sockets and subprocess standard-library natives.
//
// Invariants under test:
//   1. connect/listen — a loopback client and server exchange bytes.
//   2. Socket.close_write() sends EOF while leaving the read side open.
//   3. spawn — writes reach the child and reads return its output.
//   4. run   — captures status, stdout, and stderr together.
//   5. wait()/kill() and pid() — a child is reaped with a real status.
//   6. Fatal errors — bad arguments and failed launches halt the program.
//   7. Socket and Server faults — a closed socket, a refused connection, a
//      failed write or read are catchable errors.

#include "test_harness.h"
#include <gtest/gtest.h>

#include "container_objects.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

// A readline() can read past its line. The bytes after the line belong to
// the next read, whichever call it is.
static void expectReadAfterReadline(const char* next, const char* expectation) {
    VMTestHarness h;
    std::string src = "var l = listen(\"127.0.0.1\", 0); "
                      "var c = connect(\"127.0.0.1\", l.port()); "
                      "c.write(\"one\\ntwo\\nthree\"); "
                      "c.close_write(); "
                      "var s = l.accept(); "
                      "var first = s.readline(); "
                      "var rest = " +
                      std::string(next) +
                      "; "
                      "s.close(); c.close(); l.close(); "
                      "(first == \"one\") and " +
                      expectation + ";";
    ASSERT_EQ(h.run(src.c_str()), InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, ReadlineLeavesTheRestForRead) {
    expectReadAfterReadline("s.read()", "(rest == \"two\\nthree\")");
}

TEST(NetProcessTest, ReadlineLeavesTheRestForReadBytes) {
    expectReadAfterReadline("s.read_bytes(6)", "(rest == \"two\\nth\")");
}

TEST(NetProcessTest, ReadlineLeavesTheRestForReadlines) {
    expectReadAfterReadline("s.readlines()",
                            "(len(rest) == 2) and (rest[0] == \"two\") and "
                            "(rest[1] == \"three\")");
}

TEST(NetProcessTest, ReadlineLeavesTheRestForReadline) {
    expectReadAfterReadline("s.readline()", "(rest == \"two\")");
}

// Lox++ has no string repeat operator.
#define kLoxRep                                                                \
    "fun rep(t, n) { var r = \"\"; var i = 0; "                                \
    "while (i < n) { r = r + t; i = i + 1; } return r; } "

// After a read returns, the socket keeps only the unread tail. A line much
// longer than one read chunk must not leave its capacity behind. The writes
// stay at 100000 bytes so they fit in the loopback buffers without a reader.
TEST(NetProcessTest, LongLineDoesNotLeaveCapacityInTheInputBuffer) {
    VMTestHarness h;
    ASSERT_EQ(h.run(kLoxRep "var l = listen(\"127.0.0.1\", 0); "
                            "var c = connect(\"127.0.0.1\", l.port()); "
                            "var s = l.accept(); "
                            "var big = rep(\"0123456789\", 10000); "
                            "c.write(big + \"\\nsmall\\n\"); "
                            "var first = s.readline(); "
                            "c.close(); l.close(); "
                            "s;"),
              InterpretResult::OK);
    Value v = h.lastResult();
    ASSERT_TRUE(isSocket(v));
    ObjSocket* sock = asObjSocket(as<Obj*>(v));
    EXPECT_LE(sock->inbuf.capacity(), 2u * 4096u);
    EXPECT_LE(sock->inbuf.size(), 4096u);
}

// A Socket with a writer queue is about 480 bytes larger. A Socket that never
// parks in a write must not own one, also after it reads, writes with room, and
// closes.
TEST(NetProcessTest, SocketThatNeverParksAWriteHasNoWriterQueue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "c.write(\"hi\\n\"); "
                    "var line = s.readline(); "
                    "l.close(); "
                    "[c, s];"),
              InterpretResult::OK);
    Value v = h.lastResult();
    ASSERT_TRUE(isList(v));
    auto* list = asObjList(as<Obj*>(v));
    ASSERT_EQ(list->elements.size(), 2u);
    for (const Value& item : list->elements) {
        ASSERT_TRUE(isSocket(item));
        EXPECT_FALSE(asObjSocket(as<Obj*>(item))->hasWriterQueue());
    }
}

// A line longer than one chunk, a line split over chunks, and read_bytes over
// chunk boundaries return the same bytes as one big read would.
TEST(NetProcessTest, ReadsAcrossChunkBoundaries) {
    VMTestHarness h;
    ASSERT_EQ(
        h.run(kLoxRep
              "var l = listen(\"127.0.0.1\", 0); "
              "var c = connect(\"127.0.0.1\", l.port()); "
              "var s = l.accept(); "
              "var a = rep(\"a\", 10000); var b = rep(\"b\", 9000); "
              "c.write(a + \"\\n\" + b + \"\\n\" + rep(\"xyz\", 3000)); "
              "c.close_write(); "
              "var r1 = s.readline(); "
              "var r2 = s.read_bytes(4500); "
              "var r3 = s.read_bytes(4501); "
              "var r4 = s.read_bytes(5); "
              "var r5 = s.read(); "
              "s.close(); c.close(); l.close(); "
              "(r1 == a) and (r2 == rep(\"b\", 4500)) and "
              "(r3 == rep(\"b\", 4500) + \"\\n\") and (r4 == \"xyzxy\") and "
              "(len(r5) == 8995);"),
        InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// A peer that writes in small pieces: each read of the reader returns a part
// of a line. The peer sleeps between pieces so the pieces arrive in separate
// reads; the reader blocks on read(2), so the sleep only shapes the arrival
// and never decides the result.
TEST(NetProcessTest, ReadsFromAPeerThatWritesInPieces) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    int port = ntohs(addr.sin_port);

    std::thread peer([lfd] {
        int fd = accept(lfd, nullptr, nullptr);
        const char* pieces[] = {"he", "llo\nwor", "ld", "\nrest", "-of-it"};
        for (const char* piece : pieces) {
            (void)!write(fd, piece, std::strlen(piece));
            usleep(20000);
        }
        close(fd);
    });
    VMTestHarness h;
    std::string src = "var c = connect(\"127.0.0.1\", " + std::to_string(port) +
                      "); "
                      "var a = c.readline(); var b = c.readline(); "
                      "var d = c.read_bytes(4); var e = c.read(); "
                      "c.close(); "
                      "(a == \"hello\") and (b == \"world\") and "
                      "(d == \"rest\") and (e == \"-of-it\");";
    InterpretResult r = h.run(src.c_str());
    peer.join();
    close(lfd);
    ASSERT_EQ(r, InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// The last line of a stream has no newline. readline returns it, then nil.
TEST(NetProcessTest, ReadlineReturnsTheUnterminatedLastLine) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "c.write(\"first\\nlast\"); "
                    "c.close_write(); "
                    "var a = s.readline(); var b = s.readline(); "
                    "var d = s.readline(); "
                    "s.close(); c.close(); l.close(); "
                    "(a == \"first\") and (b == \"last\") and (d == nil);"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// The kernel can reset a connection after bytes arrived. The bytes that were
// read before the fault stay available to the next read.
TEST(NetProcessTest, BytesBeforeAResetStayForTheNextRead) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "s.write(\"line1\\nline2\\npart\"); "
                    "c.write(\"unread\"); "
                    "sleep(0.2); "
                    "s.close(); "
                    "var a = c.readline(); var b = c.readline(); "
                    "var k = \"\"; "
                    "try { c.readline(); } catch (e) { k = e.kind; } "
                    "var d = c.readline(); "
                    "c.close(); l.close(); "
                    "(a == \"line1\") and (b == \"line2\") and "
                    "(k == \"NetworkError\") and (d == \"part\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, ReadBytesKeepsItsBytesWhenTheReadFaults) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "s.write(\"abc\"); "
                    "c.write(\"unread\"); "
                    "sleep(0.2); "
                    "s.close(); "
                    "var k = \"\"; "
                    "try { c.read_bytes(100); } catch (e) { k = e.kind; } "
                    "var d = c.read_bytes(100); "
                    "c.close(); l.close(); "
                    "(k == \"NetworkError\") and (d == \"abc\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// A socket the runtime creates must not leak into a child process.
TEST(NetProcessTest, SocketsAreNotInheritedByChildren) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "var r = run(\"/bin/sh\", [\"-c\", "
                    "\"ls -l /proc/self/fd | grep -c socket:\"]); "
                    "s.close(); c.close(); l.close(); "
                    "r[\"stdout\"] == \"0\\n\";"),
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

// read_bytes(n) blocks until n bytes or EOF. Three exact reads consume the
// client's six bytes; the fourth sees EOF and returns "".
TEST(NetProcessTest, SocketReadBytesExactThenEmptyAtEof) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.write(\"abcdef\"); "
                    "c.close_write(); "
                    "var s = l.accept(); "
                    "var a = s.read_bytes(3); "
                    "var b = s.read_bytes(3); "
                    "var e = s.read_bytes(3); "
                    "s.close(); "
                    "c.close(); "
                    "l.close(); "
                    "(a == \"abc\") and (b == \"def\") and (e == \"\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// A count larger than the remaining bytes returns the remainder at EOF, never
// nil and never a block past the close.
TEST(NetProcessTest, SocketReadBytesShortAtEof) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.write(\"abc\"); "
                    "c.close_write(); "
                    "var s = l.accept(); "
                    "var a = s.read_bytes(10); "
                    "var e = s.read_bytes(1); "
                    "s.close(); "
                    "c.close(); "
                    "l.close(); "
                    "(a == \"abc\") and (e == \"\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

// n == 0 returns immediately with "", even with no data waiting.
TEST(NetProcessTest, SocketReadBytesZero) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.read_bytes(0) == \"\";"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, SocketReadBytesBadCountFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.read_bytes(-1);"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.read_bytes(1.5);"),
              InterpretResult::RUNTIME_ERROR);
    EXPECT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.read_bytes(\"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(NetProcessTest, SocketReadBytesOnClosedIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.close(); "
                    "var k = \"\"; "
                    "try { c.read_bytes(1); } catch (e) { k = e.kind; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "SocketClosedError");
}

TEST(NetProcessTest, ProcessReadBytes) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/sh\", [\"-c\", \"printf hello\"]); "
                    "var a = p.read_bytes(2); "
                    "var b = p.read_bytes(10); "
                    "var e = p.read_bytes(1); "
                    "p.wait(); "
                    "(a == \"he\") and (b == \"llo\") and (e == \"\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, ProcessErrReadBytes) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var p = spawn(\"/bin/sh\", "
                    "[\"-c\", \"printf boom 1>&2\"]); "
                    "var a = p.err_read_bytes(2); "
                    "var b = p.err_read_bytes(10); "
                    "var e = p.err_read_bytes(1); "
                    "p.wait(); "
                    "(a == \"bo\") and (b == \"om\") and (e == \"\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(NetProcessTest, WriteAfterCloseWriteIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "c.close_write(); "
                    "var k = \"\"; "
                    "try { c.write(\"x\"); } catch (e) { k = e.kind; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "SocketClosedError");
}

TEST(NetProcessTest, WriteToClosedPeerIsCatchableNotSignal) {
    // Writes after the peer closes: one of them must fail with a catchable
    // NetworkError. Before the SIGPIPE fix the VM died with signal 13
    // (exit 141).
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "s.close(); "
                    "var k = \"\"; "
                    "var i = 0; "
                    "while (i < 1000 and k == \"\") { "
                    "try { c.write(\"x\"); } catch (e) { k = e.kind; } "
                    "i = i + 1; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "NetworkError");
}

TEST(NetProcessTest, ReadAfterAFailedWriteReturnsThePeersBytes) {
    // The failed write must not leave a stream error that a later read
    // reports as its own fault.
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "s.write(\"bye\"); "
                    "s.close(); "
                    "var k = \"\"; "
                    "var i = 0; "
                    "while (i < 1000 and k == \"\") { "
                    "try { c.write(\"xxxxxxxx\"); } catch (e) { k = e.kind; } "
                    "i = i + 1; } "
                    "k + \" \" + c.read_bytes(3);"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "NetworkError bye");
}

TEST(NetProcessTest, ReadAfterResetIsCatchableNotEndOfStream) {
    // The peer closes with unread bytes in its receive buffer, so the kernel
    // sends a reset. The read must not look like an end of stream.
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var c = connect(\"127.0.0.1\", l.port()); "
                    "var s = l.accept(); "
                    "c.write(\"unread\"); "
                    "sleep(0.2); "
                    "s.close(); "
                    "var k = \"\"; "
                    "try { c.read(); } catch (e) { k = e.kind; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "NetworkError");
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

TEST(NetProcessTest, AcceptOnClosedServerIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "l.close(); "
                    "var k = \"\"; "
                    "try { l.accept(); } catch (e) { k = e.kind; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "SocketClosedError");
}

TEST(NetProcessTest, ConnectRefusedIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var l = listen(\"127.0.0.1\", 0); "
                    "var port = l.port(); "
                    "l.close(); "
                    "var k = \"\"; "
                    "try { connect(\"127.0.0.1\", port); } "
                    "catch (e) { k = e.kind; } "
                    "k;"),
              InterpretResult::OK);
    EXPECT_EQ(stringify(h.lastResult()), "NetworkError");
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

// ---------------------------------------------------------------------------
// Peak memory of a read that returns a very long line
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kLongLine = 64u * 1024 * 1024;

// Runs one call on a socket whose peer sends `kLongLine` bytes with no newline
// and then closes. The program runs in a forked child, so the peak resident
// size of the child belongs to this call only. Returns that peak in bytes, or
// 0 when the child failed.
size_t peakBytesOfLongLineRead(const char* call) {
    pid_t pid = fork();
    if (pid == 0) {
        VMTestHarness h;
        std::string src =
            "var srv = listen(\"127.0.0.1\", 0); "
            "var peer = connect(\"127.0.0.1\", srv.port()); "
            "var conn = srv.accept(); "
            "fun feed() { var c = \"x\"; "
            "for (var i = 0; i < 20; i = i + 1) { c = c + c; } "
            "for (var n = 0; n < " +
            std::to_string(kLongLine) +
            "; n = n + len(c)) { peer.write(c); } peer.close(); } "
            "task.spawn(feed); "
            "var r = conn." +
            call + "();";
        _exit(h.run(src) == InterpretResult::OK ? 0 : 1);
    }
    if (pid < 0) {
        return 0;
    }
    int status = 0;
    struct rusage ru = {};
    if (wait4(pid, &status, 0, &ru) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 0;
    }
    return static_cast<size_t>(ru.ru_maxrss) * 1024;
}

} // namespace

// readlines() of one long line may not hold more than readline() of it. Both
// hold the line twice at the peak: the input buffer and the String object. A
// third copy of the line adds one line length to the peak, which is more than
// the slack. The slack covers allocator rounding and the runtime's own pages.
TEST(NetProcessTest, ReadlinesOfOneLongLineHoldsNoExtraCopy) {
    const size_t one = peakBytesOfLongLineRead("readline");
    const size_t many = peakBytesOfLongLineRead("readlines");
    ASSERT_GT(one, kLongLine);
    ASSERT_GT(many, kLongLine);
    EXPECT_LT(many, one + kLongLine / 2)
        << "readline peak " << one << ", readlines peak " << many;
}
