#include "allocator.h"

#include <limits>

#include <gtest/gtest.h>

class AllocatorTest : public ::testing::Test {
  protected:
    void SetUp() override { AllocatorImpl::reset(); }

    void TearDown() override { AllocatorImpl::reset(); }
};

TEST_F(AllocatorTest, ByteAllocationTracking) {
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);

    int* array = AllocatorImpl::allocate<int>(10);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              10 * sizeof(int));

    array = AllocatorImpl::growArray<int>(array, 10, 20);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              20 * sizeof(int));

    array = AllocatorImpl::growArray<int>(array, 20, 5);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              5 * sizeof(int));

    AllocatorImpl::freeArray<int>(array, 5);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);
}

TEST_F(AllocatorTest, GrowCapacity) {
    EXPECT_EQ(AllocatorImpl::growCapacity(0), 8);
    EXPECT_EQ(AllocatorImpl::growCapacity(4), 8);
    EXPECT_EQ(AllocatorImpl::growCapacity(8), 16);
    EXPECT_EQ(AllocatorImpl::growCapacity(100), 200);
}

TEST_F(AllocatorTest, ReallocateFunction) {
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);

    void* ptr = AllocatorImpl::reallocate(nullptr, 0, 100);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 100);

    ptr = AllocatorImpl::reallocate(ptr, 100, 200);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 200);

    AllocatorImpl::reallocate(ptr, 200, 0);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);
}

TEST_F(AllocatorTest, AllocateDifferentTypes) {
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);

    int* intArray = AllocatorImpl::allocate<int>(5);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              5 * sizeof(int));

    double* doubleArray = AllocatorImpl::allocate<double>(10);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              5 * sizeof(int) + 10 * sizeof(double));

    char* charArray = AllocatorImpl::allocate<char>(100);
    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(),
              5 * sizeof(int) + 10 * sizeof(double) + 100 * sizeof(char));

    AllocatorImpl::freeArray<int>(intArray, 5);
    AllocatorImpl::freeArray<double>(doubleArray, 10);
    AllocatorImpl::freeArray<char>(charArray, 100);

    EXPECT_EQ(AllocatorState::getInstance().getByteAllocated(), 0);
}

TEST_F(AllocatorTest, AllocationFailure) {
    // Try to allocate an extremely large amount of memory
    // This should throw bad_alloc
    constexpr size_t TOO_LARGE = std::numeric_limits<std::size_t>::max() - 100;
    EXPECT_THROW(AllocatorImpl::reallocate(nullptr, 0, TOO_LARGE),
                 std::bad_alloc);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}