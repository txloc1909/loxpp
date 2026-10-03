#include "value.h"

#include <gtest/gtest.h>

// MemoryManager::create rejects a pointer that does not fit in the NaN-boxed
// payload (src/value.h). These tests pin the boundary of that predicate: if
// kPointerBits or pointerFitsInValue drift, an object pointer would be silently
// truncated by Value's constructor instead of reported.
TEST(NanBoxPointerGuard, AcceptsAddressesBelowTheTag) {
    EXPECT_TRUE(pointerFitsInValue(0));
    EXPECT_TRUE(pointerFitsInValue((uintptr_t{1} << kPointerBits) - 1));
    EXPECT_TRUE(pointerFitsInValue(0x0000'7FFF'FFFF'FFFFULL));
}

TEST(NanBoxPointerGuard, RejectsAddressesAtOrBeyondTheTag) {
    EXPECT_FALSE(pointerFitsInValue(uintptr_t{1} << kPointerBits));
    EXPECT_FALSE(pointerFitsInValue(~uintptr_t{0}));
}

TEST(NanBoxPointerGuard, LeavesAtLeast48Bits) {
    EXPECT_GE(kPointerBits, 48u);
    EXPECT_LE(kPointerBits, 50u);
}
