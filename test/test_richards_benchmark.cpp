// pins the scheduler counts of the AWFY Richards port (issue #892).
//
// benchmarks/core/richards.lox carries its own self-check: Scheduler.start()
// returns true only for queuePacketCount == 23246 and holdCount == 9297, the
// values the AWFY Java/SOM Richards reports. Nothing ran that check.
// benchmarks/run.py compares backends against each other, and every backend
// ran the same port, so a port that returned false agreed with itself three
// times and every row said "ok". The 2026-08-26 report records the same gap
// from the other side: the scheduler once did no work at all
// (queuePacketCount=0) and the suite only showed a different checksum.
//
// This test runs the scheduler in one backend, so a wrong port fails here
// instead of agreeing with a copy of itself.

#include "corpus.h"
#include "test_harness.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// The reference counts, from the AWFY port this file was ported from. They
// appear in the benchmark as its self-check; a change to either pair is a
// change to the benchmark, not to this test alone.
constexpr double kQueuePacketCount = 23246;
constexpr double kHoldCount = 9297;

// The scheduler is driven directly instead of through
// Richards.benchmark(): the counts stay visible when the self-check fails, so
// a failing run reports the numbers it produced.
const char* kDriver = R"(
var RICHARDS_START = false;
var RICHARDS_QUEUE = 0;
var RICHARDS_HOLD  = 0;
var s = Scheduler();
RICHARDS_START = s.start();
RICHARDS_QUEUE = s.queuePacketCount;
RICHARDS_HOLD  = s.holdCount;
)";

} // namespace

TEST(RichardsBenchmark, SchedulerSelfCheckMatchesReferenceCounts) {
    VMTestHarness h;
    const std::string source =
        loxpp_test::readFile(loxpp_test::projectRoot() / "benchmarks" / "core" /
                             "richards.lox") +
        kDriver;
    ASSERT_EQ(h.run(source), InterpretResult::OK);

    auto start = h.getGlobal("RICHARDS_START");
    ASSERT_TRUE(start.has_value());
    ASSERT_TRUE(is<bool>(*start));
    EXPECT_TRUE(as<bool>(*start));

    auto queue = h.getGlobal("RICHARDS_QUEUE");
    ASSERT_TRUE(queue.has_value());
    ASSERT_TRUE(is<Number>(*queue));
    EXPECT_EQ(as<Number>(*queue), kQueuePacketCount);

    auto hold = h.getGlobal("RICHARDS_HOLD");
    ASSERT_TRUE(hold.has_value());
    ASSERT_TRUE(is<Number>(*hold));
    EXPECT_EQ(as<Number>(*hold), kHoldCount);
}
