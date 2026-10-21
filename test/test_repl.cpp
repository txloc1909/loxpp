// test_repl.cpp — a worker that a REPL line spawns (issues #787 and #789).
//
// The REPL compiles each line alone, and a worker recompiles the sources of
// its program. A worker of a later line must define the declarations of the
// earlier lines, and it must assign the same constructor tags, without a copy
// of the session's declaration tables for each source. These tests drive a VM
// the way main.cpp's repl() does: one run() per line.

#include "objects.h"
#include "test_harness.h"
#include "value.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

void expect_num(Value v, double n) {
    ASSERT_TRUE(is<Number>(v));
    EXPECT_DOUBLE_EQ(as<Number>(v), n);
}

// One run() per line, as the REPL does it.
class Session {
  public:
    Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    InterpretResult line(const std::string& source) { return m_h.run(source); }
    std::optional<Value> global(const std::string& name) const {
        return m_h.getGlobal(name);
    }

  private:
    VMTestHarness m_h;
};

} // namespace

class ReplSessionTest : public ::testing::Test {};

// #787: the entry of a later line names a top-level `fun` and an `enum` of
// earlier lines. The worker must define both before it calls the entry.
TEST_F(ReplSessionTest, WorkerDefinesAnEarlierLinesFunctionAndEnum) {
    Session s;
    ASSERT_EQ(s.line("enum Opt { Some(v) None }"), InterpretResult::OK);
    ASSERT_EQ(s.line("fun twice(x) { return x * 2; }"), InterpretResult::OK);
    ASSERT_EQ(s.line(R"(
        fun entry(main) { task.send(main, Opt.Some(twice(3))); }
        task.spawn_worker(entry, task.self());
        var got = task.receive();
        var r = match got { case Opt.Some(v) => v case Opt.None => 0 };
    )"),
              InterpretResult::OK);
    auto r = s.global("r");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 6.0);
}

// #787: the same for a class of an earlier line, which the entry constructs
// and the main task matches with a class pattern.
TEST_F(ReplSessionTest, WorkerDefinesAnEarlierLinesClass) {
    Session s;
    ASSERT_EQ(s.line("class P { init(n) { this.n = n; } }"),
              InterpretResult::OK);
    ASSERT_EQ(s.line(R"(
        fun entry(main) {
            var r = match P(4) { case P{n} => n case _ => 0 };
            task.send(main, r);
        }
        task.spawn_worker(entry, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    auto r = s.global("got");
    ASSERT_TRUE(r.has_value());
    expect_num(*r, 4.0);
}

// #787: the worker assigns constructor tags in the order of the session, so a
// value of `B.Z` does not match the arm of `B.Y` there either.
TEST_F(ReplSessionTest, WorkerReplaysEarlierConstructorTagsInOrder) {
    Session s;
    ASSERT_EQ(s.line("enum A { X }"), InterpretResult::OK);
    ASSERT_EQ(s.line("enum B { Y Z }"), InterpretResult::OK);
    ASSERT_EQ(s.line(R"(
        fun entry(main) {
            var r = match B.Z() { case B.Y => 1 case B.Z => 2 case _ => 3 };
            task.send(main, r);
        }
        task.spawn_worker(entry, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    auto got = s.global("got");
    ASSERT_TRUE(got.has_value());
    expect_num(*got, 2.0);
}

// #789: a long session of declarations still starts a worker correctly. The
// worker replays the earlier sources instead of holding a copy of the
// session's declaration tables for each of them.
TEST_F(ReplSessionTest, WorkerOfALongSessionUsesTheLastLine) {
    Session s;
    for (int i = 0; i < 40; i++) {
        const std::string i_str = std::to_string(i);
        ASSERT_EQ(s.line("class C" + i_str + " { init() {} }"),
                  InterpretResult::OK);
        ASSERT_EQ(s.line("fun f" + i_str + "() { return " + i_str + "; }"),
                  InterpretResult::OK);
    }
    ASSERT_EQ(s.line("enum E { A B }"), InterpretResult::OK);
    ASSERT_EQ(s.line(R"(
        fun entry(main) {
            var r = match E.B() { case E.A => 1 case E.B => 2 case _ => 3 };
            task.send(main, r + f39());
        }
        task.spawn_worker(entry, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    auto got = s.global("got");
    ASSERT_TRUE(got.has_value());
    expect_num(*got, 41.0);
}
