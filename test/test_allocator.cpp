#include "allocator.h"
#include "simple_allocator.h"

#include <gtest/gtest.h>

#include <memory>

class AllocatorTest : public ::testing::Test {
  protected:
    std::unique_ptr<Allocator> alloc;

    void SetUp() override { alloc = std::make_unique<SimpleAllocator>(); }
};

// ---------------------------------------------------------------------------
// Phase 1 — Allocator interface + SimpleAllocator
// ---------------------------------------------------------------------------

TEST_F(AllocatorTest, MakeStringReturnsStringHandle) {
    ObjHandle h = alloc->makeString("hello");
    EXPECT_EQ(h.type, ObjType::STRING);
}

TEST_F(AllocatorTest, DerefReturnsCorrectChars) {
    ObjHandle h = alloc->makeString("hello");
    auto* str = static_cast<ObjString*>(alloc->deref(h));
    EXPECT_EQ(str->chars, "hello");
}

TEST_F(AllocatorTest, MakeString_EmptyString) {
    ObjHandle h = alloc->makeString("");
    auto* str = static_cast<ObjString*>(alloc->deref(h));
    EXPECT_EQ(str->chars, "");
}

TEST_F(AllocatorTest, Interning_SameContent_SameHandle) {
    ObjHandle h1 = alloc->makeString("x");
    ObjHandle h2 = alloc->makeString("x");
    EXPECT_EQ(h1, h2);
}

TEST_F(AllocatorTest, Interning_DifferentContent_DifferentHandle) {
    ObjHandle h1 = alloc->makeString("a");
    ObjHandle h2 = alloc->makeString("b");
    EXPECT_NE(h1, h2);
}

TEST_F(AllocatorTest, HandleStability_AfterMoreAllocations) {
    ObjHandle h1 = alloc->makeString("first");
    ObjHandle h2 = alloc->makeString("second");
    ObjHandle h3 = alloc->makeString("third");

    EXPECT_EQ(static_cast<ObjString*>(alloc->deref(h1))->chars, "first");
    EXPECT_EQ(static_cast<ObjString*>(alloc->deref(h2))->chars, "second");
    EXPECT_EQ(static_cast<ObjString*>(alloc->deref(h3))->chars, "third");
}

TEST_F(AllocatorTest, PolymorphicUsage_ViaInterfacePointer) {
    std::unique_ptr<Allocator> a = std::make_unique<SimpleAllocator>();
    ObjHandle h = a->makeString("poly");
    auto* str = static_cast<ObjString*>(a->deref(h));
    EXPECT_EQ(str->chars, "poly");
}

TEST_F(AllocatorTest, Collect_DoesNotCrash) {
    alloc->makeString("gc1");
    alloc->makeString("gc2");
    alloc->makeString("gc3");
    EXPECT_NO_FATAL_FAILURE(alloc->collect());
}

// ---------------------------------------------------------------------------
// Phase 2 — ObjHandle in Value
// ---------------------------------------------------------------------------

TEST_F(AllocatorTest, ValueHoldsObjHandle) {
    Value v{alloc->makeString("hi")};
    EXPECT_TRUE(is<ObjHandle>(v));
}

TEST_F(AllocatorTest, IsString_TrueForStringHandle) {
    Value vStr{alloc->makeString("hello")};
    Value vNum{42.0};
    Value vNil{Nil{}};
    EXPECT_TRUE(isString(vStr));
    EXPECT_FALSE(isString(vNum));
    EXPECT_FALSE(isString(vNil));
}

TEST_F(AllocatorTest, Stringify_ViaAllocator) {
    Value v{alloc->makeString("hi")};
    EXPECT_EQ(stringify(v, *alloc), "hi");
}

TEST_F(AllocatorTest, EqualHandles_SameInternedString) {
    Value a{alloc->makeString("x")};
    Value b{alloc->makeString("x")};
    EXPECT_EQ(a, b);
}

TEST_F(AllocatorTest, UnequalHandles_DifferentStrings) {
    Value a{alloc->makeString("x")};
    Value b{alloc->makeString("y")};
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// Phase 3 — Tracked allocation (reallocate + bytesAllocated)
// ---------------------------------------------------------------------------

TEST_F(AllocatorTest, BytesAllocated_StartsAtZero) {
    EXPECT_EQ(alloc->bytesAllocated(), 0u);
}

TEST_F(AllocatorTest, BytesAllocated_IncreasesOnMakeString) {
    // Use a string long enough to exceed SSO (typically 15 chars on libstdc++).
    alloc->makeString("this string is definitely long enough");
    EXPECT_GT(alloc->bytesAllocated(), 0u);
}

TEST_F(AllocatorTest, BytesAllocated_InternedStringDoesNotIncrease) {
    alloc->makeString("this string is definitely long enough");
    size_t after_first = alloc->bytesAllocated();
    alloc->makeString("this string is definitely long enough"); // interned
    EXPECT_EQ(alloc->bytesAllocated(), after_first);
}

// ---------------------------------------------------------------------------
// Phase 4 — GC hooks (markObject)
// ---------------------------------------------------------------------------

TEST_F(AllocatorTest, MarkObject_SetsMarkedFlag) {
    ObjHandle h = alloc->makeString("marked");
    Obj* obj = alloc->deref(h);
    EXPECT_FALSE(obj->marked);
    alloc->markObject(obj);
    EXPECT_TRUE(obj->marked);
}

TEST_F(AllocatorTest, Collect_DoesNotCrashAfterMark) {
    alloc->makeString("a");
    alloc->makeString("b");
    alloc->collect();
    EXPECT_NO_FATAL_FAILURE(alloc->makeString("c"));
}
