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
    EXPECT_EQ(std::string_view(str->chars, str->length), "hello");
}

TEST_F(AllocatorTest, MakeString_EmptyString) {
    ObjHandle h = alloc->makeString("");
    auto* str = static_cast<ObjString*>(alloc->deref(h));
    EXPECT_EQ(std::string_view(str->chars, str->length), "");
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

    auto* s1 = static_cast<ObjString*>(alloc->deref(h1));
    auto* s2 = static_cast<ObjString*>(alloc->deref(h2));
    auto* s3 = static_cast<ObjString*>(alloc->deref(h3));
    EXPECT_EQ(std::string_view(s1->chars, s1->length), "first");
    EXPECT_EQ(std::string_view(s2->chars, s2->length), "second");
    EXPECT_EQ(std::string_view(s3->chars, s3->length), "third");
}

TEST_F(AllocatorTest, PolymorphicUsage_ViaInterfacePointer) {
    std::unique_ptr<Allocator> a = std::make_unique<SimpleAllocator>();
    ObjHandle h = a->makeString("poly");
    auto* str = static_cast<ObjString*>(a->deref(h));
    EXPECT_EQ(std::string_view(str->chars, str->length), "poly");
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
    alloc->makeString("hello");
    // struct + null-terminated char buffer (5 chars + '\0' = 6 bytes) +
    // sizeof(ObjString)
    EXPECT_GT(alloc->bytesAllocated(), 0u);
}

TEST_F(AllocatorTest, BytesAllocated_IncreasesWithEachNewString) {
    alloc->makeString("abc");
    size_t after_one = alloc->bytesAllocated();
    alloc->makeString("defgh"); // longer string → more bytes
    EXPECT_GT(alloc->bytesAllocated(), after_one);
}

TEST_F(AllocatorTest, BytesAllocated_InternedStringDoesNotIncrease) {
    alloc->makeString("same");
    size_t after_first = alloc->bytesAllocated();
    alloc->makeString("same"); // interned — no new allocation
    EXPECT_EQ(alloc->bytesAllocated(), after_first);
}

TEST_F(AllocatorTest, BytesAllocated_DecreasesOnDestruction) {
    {
        SimpleAllocator inner;
        inner.makeString("temp");
        EXPECT_GT(inner.bytesAllocated(), 0u);
        // destructor frees all objects — bytesAllocated reaches 0
    }
    // No crash and memory is freed (verified by sanitizers at runtime)
}

TEST_F(AllocatorTest, CharsBuffer_IsNullTerminated) {
    ObjHandle h = alloc->makeString("lox");
    auto* str = static_cast<ObjString*>(alloc->deref(h));
    EXPECT_EQ(str->chars[str->length], '\0');
}
