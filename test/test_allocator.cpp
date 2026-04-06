#include "memory_manager.h"
#include "value.h"

#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// MemoryManager + VmAllocator<T>
// ---------------------------------------------------------------------------

TEST(MemoryManagerTest, MakeStringReturnsObjStringPtr) {
    MemoryManager mm;
    ObjString* str = mm.makeString("hello");
    EXPECT_NE(str, nullptr);
}

TEST(MemoryManagerTest, MakeStringCharsMatch) {
    MemoryManager mm;
    ObjString* str = mm.makeString("hello");
    EXPECT_EQ(str->chars.size(), 5u);
    EXPECT_EQ(std::string(str->chars.data(), str->chars.size()), "hello");
}

TEST(MemoryManagerTest, MakeString_EmptyString) {
    MemoryManager mm;
    ObjString* str = mm.makeString("");
    EXPECT_NE(str, nullptr);
    EXPECT_EQ(str->chars.size(), 0u);
}

TEST(MemoryManagerTest, Interning_SameContent_SamePointer) {
    MemoryManager mm;
    ObjString* a = mm.makeString("interned");
    ObjString* b = mm.makeString("interned");
    EXPECT_EQ(a, b);
}

TEST(MemoryManagerTest, Interning_DifferentContent_DifferentPointer) {
    MemoryManager mm;
    ObjString* a = mm.makeString("foo");
    ObjString* b = mm.makeString("bar");
    EXPECT_NE(a, b);
}

TEST(MemoryManagerTest, PointerStability) {
    MemoryManager mm;
    ObjString* s1 = mm.makeString("first");
    ObjString* s2 = mm.makeString("second");
    ObjString* s3 = mm.makeString("third");
    EXPECT_EQ(std::string(s1->chars.data(), s1->chars.size()), "first");
    EXPECT_EQ(std::string(s2->chars.data(), s2->chars.size()), "second");
    EXPECT_EQ(std::string(s3->chars.data(), s3->chars.size()), "third");
}

TEST(MemoryManagerTest, CollectAll_DoesNotCrash) {
    MemoryManager mm;
    mm.makeString("gc1");
    mm.makeString("gc2");
    mm.makeString("gc3");
    EXPECT_NO_FATAL_FAILURE(mm.collectAll());
}

TEST(MemoryManagerTest, ValueHoldsObjPtr) {
    MemoryManager mm;
    Value v{static_cast<Obj*>(mm.makeString("hi"))};
    EXPECT_TRUE(is<Obj*>(v));
}

TEST(MemoryManagerTest, IsString_Checks) {
    MemoryManager mm;
    Value vStr{static_cast<Obj*>(mm.makeString("hello"))};
    Value vNum{42.0};
    Value vNil{Nil{}};
    EXPECT_TRUE(isString(vStr));
    EXPECT_TRUE(isObj(vStr));
    EXPECT_FALSE(isString(vNum));
    EXPECT_FALSE(isString(vNil));
}

TEST(MemoryManagerTest, Stringify_ObjPtr) {
    MemoryManager mm;
    Value v{static_cast<Obj*>(mm.makeString("hi"))};
    EXPECT_EQ(stringify(v), "hi");
}

TEST(MemoryManagerTest, EqualPointers_SameInterned) {
    MemoryManager mm;
    Value a{static_cast<Obj*>(mm.makeString("x"))};
    Value b{static_cast<Obj*>(mm.makeString("x"))};
    EXPECT_EQ(a, b);
}

TEST(MemoryManagerTest, BytesAllocated_IncreasesOnMakeString) {
    MemoryManager mm;
    std::size_t before = mm.bytesAllocated;
    // Use a string longer than SSO threshold (>15 chars on most platforms).
    mm.makeString("this_is_a_long_string_exceeding_sso");
    EXPECT_GT(mm.bytesAllocated, before);
}

TEST(MemoryManagerTest, BytesAllocated_InternedDoesNotIncrease) {
    MemoryManager mm;
    mm.makeString("shared");
    std::size_t after_first = mm.bytesAllocated;
    mm.makeString("shared");
    EXPECT_EQ(mm.bytesAllocated, after_first);
}

TEST(MemoryManagerTest, BytesAllocated_CountsStructHeader) {
    MemoryManager mm;
    mm.makeString("any");
    EXPECT_GE(mm.bytesAllocated, sizeof(ObjString));
}
