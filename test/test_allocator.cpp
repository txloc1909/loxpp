#include "allocator.h"
#include "arena_allocator.h"
#include "simple_allocator.h"

#include <gtest/gtest.h>

#include <memory>

// ---------------------------------------------------------------------------
// Typed test fixture — runs all invariants against every Allocator impl.
// ---------------------------------------------------------------------------

template <typename T>
class AllocatorTest : public ::testing::Test {
  protected:
    std::unique_ptr<Allocator> alloc;
    void SetUp() override { alloc = std::make_unique<T>(); }
};

using AllocatorTypes = ::testing::Types<SimpleAllocator, ArenaAllocator>;
TYPED_TEST_SUITE(AllocatorTest, AllocatorTypes);

// ---------------------------------------------------------------------------
// Phase 1 — Allocator interface
// ---------------------------------------------------------------------------

TYPED_TEST(AllocatorTest, MakeStringReturnsStringHandle) {
    ObjHandle h = this->alloc->makeString("hello");
    EXPECT_EQ(h.type, ObjType::STRING);
}

TYPED_TEST(AllocatorTest, DerefReturnsCorrectChars) {
    ObjHandle h = this->alloc->makeString("hello");
    auto* str = static_cast<ObjString*>(this->alloc->deref(h));
    EXPECT_EQ(str->chars, "hello");
}

TYPED_TEST(AllocatorTest, MakeString_EmptyString) {
    ObjHandle h = this->alloc->makeString("");
    auto* str = static_cast<ObjString*>(this->alloc->deref(h));
    EXPECT_EQ(str->chars, "");
}

TYPED_TEST(AllocatorTest, Interning_SameContent_SameHandle) {
    ObjHandle h1 = this->alloc->makeString("x");
    ObjHandle h2 = this->alloc->makeString("x");
    EXPECT_EQ(h1, h2);
}

TYPED_TEST(AllocatorTest, Interning_DifferentContent_DifferentHandle) {
    ObjHandle h1 = this->alloc->makeString("a");
    ObjHandle h2 = this->alloc->makeString("b");
    EXPECT_NE(h1, h2);
}

TYPED_TEST(AllocatorTest, HandleStability_AfterMoreAllocations) {
    ObjHandle h1 = this->alloc->makeString("first");
    ObjHandle h2 = this->alloc->makeString("second");
    ObjHandle h3 = this->alloc->makeString("third");

    EXPECT_EQ(static_cast<ObjString*>(this->alloc->deref(h1))->chars, "first");
    EXPECT_EQ(static_cast<ObjString*>(this->alloc->deref(h2))->chars, "second");
    EXPECT_EQ(static_cast<ObjString*>(this->alloc->deref(h3))->chars, "third");
}

TYPED_TEST(AllocatorTest, PolymorphicUsage_ViaInterfacePointer) {
    std::unique_ptr<Allocator> a = std::make_unique<TypeParam>();
    ObjHandle h = a->makeString("poly");
    auto* str = static_cast<ObjString*>(a->deref(h));
    EXPECT_EQ(str->chars, "poly");
}

TYPED_TEST(AllocatorTest, Collect_DoesNotCrash) {
    this->alloc->makeString("gc1");
    this->alloc->makeString("gc2");
    this->alloc->makeString("gc3");
    EXPECT_NO_FATAL_FAILURE(this->alloc->collect());
}

// ---------------------------------------------------------------------------
// Phase 2 — ObjHandle in Value
// ---------------------------------------------------------------------------

TYPED_TEST(AllocatorTest, ValueHoldsObjHandle) {
    Value v{this->alloc->makeString("hi")};
    EXPECT_TRUE(is<ObjHandle>(v));
}

TYPED_TEST(AllocatorTest, IsString_TrueForStringHandle) {
    Value vStr{this->alloc->makeString("hello")};
    Value vNum{42.0};
    Value vNil{Nil{}};
    EXPECT_TRUE(isString(vStr));
    EXPECT_FALSE(isString(vNum));
    EXPECT_FALSE(isString(vNil));
}

TYPED_TEST(AllocatorTest, Stringify_ViaAllocator) {
    Value v{this->alloc->makeString("hi")};
    EXPECT_EQ(stringify(v, *this->alloc), "hi");
}

TYPED_TEST(AllocatorTest, EqualHandles_SameInternedString) {
    Value a{this->alloc->makeString("x")};
    Value b{this->alloc->makeString("x")};
    EXPECT_EQ(a, b);
}

TYPED_TEST(AllocatorTest, UnequalHandles_DifferentStrings) {
    Value a{this->alloc->makeString("x")};
    Value b{this->alloc->makeString("y")};
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// ArenaAllocator-specific — exercises block overflow (> BLOCK_CAPACITY strings)
// ---------------------------------------------------------------------------

TEST(ArenaAllocatorTest, HandlesMoreThanOneBlock) {
    ArenaAllocator alloc;
    const std::size_t count = ArenaAllocator::BLOCK_CAPACITY * 3;
    std::vector<ObjHandle> handles;
    handles.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        handles.push_back(alloc.makeString(std::to_string(i)));

    // Verify all strings are intact after multi-block allocation.
    for (std::size_t i = 0; i < count; ++i) {
        auto* str = static_cast<ObjString*>(alloc.deref(handles[i]));
        EXPECT_EQ(str->chars, std::to_string(i));
    }
}
