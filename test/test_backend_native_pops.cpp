// test_backend_native_pops.cpp — the shared native_pops table (native_pops.h).
//
// native_pops states one fact per opcode: how many operand-stack cells the
// native VM reads for it. Both the JVM emitter's folded-operand repair and the
// QBE emitter's own accounting depend on it, so a missing row is a real
// compile-time gap, not a style issue. This file guards two properties:
// Op::YIELD reads exactly one cell (the yielded value), and every enumerator
// in LOXPP_FOR_EACH_OP has a row at all — the mechanical completeness gate the
// hand-written switch cannot give itself.

#include "backend/native_pops.h"
#include "chunk.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

std::vector<Op> allOps() {
    std::vector<Op> ops;
#define LOXPP_COLLECT_OP(name) ops.push_back(Op::name);
    LOXPP_FOR_EACH_OP(LOXPP_COLLECT_OP)
#undef LOXPP_COLLECT_OP
    return ops;
}

} // namespace

TEST(NativePopsTest, YieldReadsTheYieldedValue) {
    DecodedInstruction in;
    in.op = Op::YIELD;
    in.length = 1;

    // The resumed value is pushed only on resume, so it is not a cell this
    // instruction reads — 1, not 2, and not the CUSTOM nullopt.
    ASSERT_TRUE(nativePops(Op::YIELD, in).has_value());
    EXPECT_EQ(*nativePops(Op::YIELD, in), 1);
    EXPECT_EQ(opName(Op::YIELD), "YIELD");
}

TEST(NativePopsTest, EveryOpcodeHasARow) {
    for (Op op : allOps()) {
        DecodedInstruction in;
        in.op = op;
        // A width operand of 0 keeps BUILD_LIST/BUILD_MAP/CALL/DEFER_RECORD
        // valid to query; the point here is that the switch returns a row, not
        // what that row computes.
        in.byteOperand = 0;
        EXPECT_NO_THROW((void)nativePops(op, in))
            << opName(op) << " has no nativePops row";
        EXPECT_NE(opName(op), "UNKNOWN_OP")
            << "opName is missing enumerator " << static_cast<int>(op);
    }
}
