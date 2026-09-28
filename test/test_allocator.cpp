#include "memory_manager.h"
#include "value.h"

#include <gtest/gtest.h>

// The intern table is not a GC root (MemoryManager::removeWhiteStrings), so
// under LOXPP_STRESS_GC the next allocation would free a string held only by
// a C++ local. Root it for the rest of the test.
static ObjString* rootedString(MemoryManager& mm, const char* s) {
    ObjString* o = mm.makeString(s);
    mm.pushTempRoot(o);
    return o;
}

// ---------------------------------------------------------------------------
// MemoryManager + VmAllocator<T>
// ---------------------------------------------------------------------------

TEST(MemoryManagerTest, MakeStringReturnsObjStringPtr) {
    MemoryManager mm;
    ObjString* str = rootedString(mm, "hello");
    EXPECT_NE(str, nullptr);
}

TEST(MemoryManagerTest, MakeStringCharsMatch) {
    MemoryManager mm;
    ObjString* str = rootedString(mm, "hello");
    EXPECT_EQ(str->chars.size(), 5u);
    EXPECT_EQ(std::string(str->chars.data(), str->chars.size()), "hello");
}

TEST(MemoryManagerTest, MakeString_EmptyString) {
    MemoryManager mm;
    ObjString* str = rootedString(mm, "");
    EXPECT_NE(str, nullptr);
    EXPECT_EQ(str->chars.size(), 0u);
}

TEST(MemoryManagerTest, Interning_SameContent_SamePointer) {
    MemoryManager mm;
    ObjString* a = rootedString(mm, "interned");
    ObjString* b = rootedString(mm, "interned");
    EXPECT_EQ(a, b);
}

TEST(MemoryManagerTest, Interning_DifferentContent_DifferentPointer) {
    MemoryManager mm;
    ObjString* a = rootedString(mm, "foo");
    ObjString* b = rootedString(mm, "bar");
    EXPECT_NE(a, b);
}

TEST(MemoryManagerTest, HeapPointers_RemainValidAfterMoreAllocations) {
    // Ensures that adding more strings doesn't invalidate earlier ObjString*
    // pointers (heap objects are stable; only the allObjects vector reallocs).
    MemoryManager mm;
    ObjString* s1 = rootedString(mm, "first");
    ObjString* s2 = rootedString(mm, "second");
    ObjString* s3 = rootedString(mm, "third");
    EXPECT_EQ(std::string(s1->chars.data(), s1->chars.size()), "first");
    EXPECT_EQ(std::string(s2->chars.data(), s2->chars.size()), "second");
    EXPECT_EQ(std::string(s3->chars.data(), s3->chars.size()), "third");
}

TEST(MemoryManagerTest, CollectAll_DoesNotCrash) {
    MemoryManager mm;
    rootedString(mm, "gc1");
    rootedString(mm, "gc2");
    rootedString(mm, "gc3");
    EXPECT_NO_FATAL_FAILURE(mm.collectAll());
}

TEST(MemoryManagerTest, ValueHoldsObjPtr) {
    MemoryManager mm;
    Value v{static_cast<Obj*>(rootedString(mm, "hi"))};
    EXPECT_TRUE(is<Obj*>(v));
}

TEST(MemoryManagerTest, IsString_Checks) {
    MemoryManager mm;
    Value vStr{static_cast<Obj*>(rootedString(mm, "hello"))};
    Value vNum{42.0};
    Value vNil{Nil{}};
    EXPECT_TRUE(isString(vStr));
    EXPECT_TRUE(isObj(vStr));
    EXPECT_FALSE(isString(vNum));
    EXPECT_FALSE(isString(vNil));
}

TEST(MemoryManagerTest, Stringify_ObjPtr) {
    MemoryManager mm;
    Value v{static_cast<Obj*>(rootedString(mm, "hi"))};
    EXPECT_EQ(stringify(v), "hi");
}

TEST(MemoryManagerTest, EqualPointers_SameInterned) {
    MemoryManager mm;
    Value a{static_cast<Obj*>(rootedString(mm, "x"))};
    Value b{static_cast<Obj*>(rootedString(mm, "x"))};
    EXPECT_EQ(a, b);
}

TEST(MemoryManagerTest, BytesAllocated_IncreasesOnMakeString) {
    MemoryManager mm;
    std::size_t before = mm.bytesAllocated;
    // Use a string longer than SSO threshold (>15 chars on most platforms).
    rootedString(mm, "this_is_a_long_string_exceeding_sso");
    EXPECT_GT(mm.bytesAllocated, before);
}

TEST(MemoryManagerTest, BytesAllocated_InternedDoesNotIncrease) {
    MemoryManager mm;
    rootedString(mm, "shared");
    std::size_t after_first = mm.bytesAllocated;
    rootedString(mm, "shared");
    EXPECT_EQ(mm.bytesAllocated, after_first);
}

TEST(MemoryManagerTest, BytesAllocated_CountsStructHeader) {
    MemoryManager mm;
    rootedString(mm, "any");
    EXPECT_GE(mm.bytesAllocated, sizeof(ObjString));
}
