// test_dead_heap.cpp
//
// Issue #1352: the last releaseHold of a dead heap must not destroy the whole
// heap inside a step of another heap. It hands the heap to the heap that
// dropped the last count, and that heap destroys it in parts
// (MemoryManager::reapPendingPart).
//
// Every test constructs its heaps directly, so it needs no runtime. The *_gc
// twin runs under LOXPP_STRESS_GC=1: the figures of the parts change, the
// rules do not, so no test asserts a count of objects.

#include "memory_manager.h"
#include "worker.h"

#include <gtest/gtest.h>

namespace {

// A heap whose owner ended, with `objects` unreachable objects and one object
// that a caller holds. `held` receives that object.
MemoryManager* makeDeadHeap(std::size_t objects, Obj*& held) {
    auto* heap = new MemoryManager(1);
    for (std::size_t i = 0; i < objects; ++i) {
        heap->create<ObjInt>(static_cast<std::int64_t>(i));
    }
    held = heap->create<ObjInt>(0);
    heap->hold(held);
    heap->dropOwner();
    return heap;
}

} // namespace

// The state that the fix adds. Before the fix, releaseHold ran delete this,
// so the heap was gone before the caller could count a part.
TEST(DeadHeapTest, TheLastCountHandsTheHeapToTheHeapThatDroppedIt) {
    MemoryManager releaser;
    Obj* held = nullptr;
    MemoryManager* dead = makeDeadHeap(10, held);
    const std::uint16_t id = dead->heapId();
    ASSERT_NE(heapById(id), nullptr);
    EXPECT_EQ(heapById(id)->holderCount(held), 1U);

    releaser.releaseValue(Value{held});

    ASSERT_NE(heapById(id), nullptr);
    EXPECT_EQ(releaser.pendingDeadHeaps(), 1U);
    while (heapById(id) != nullptr) {
        ASSERT_GT(releaser.reapPendingPart(), 0U);
    }
    EXPECT_EQ(releaser.pendingDeadHeaps(), 0U);
}

// One part does at most the units that the budget allows, so no part follows
// the size of the dead heap.
TEST(DeadHeapTest, APartDoesAtMostItsUnits) {
    MemoryManager releaser;
    releaser.setDestroyPartUnits(1);
    Obj* held = nullptr;
    MemoryManager* dead = makeDeadHeap(50, held);
    const std::uint16_t id = dead->heapId();
    releaser.releaseValue(Value{held});

    // Under LOXPP_STRESS_GC the heap collects as it is built, so the count of
    // objects differs. The rule of the parts does not.
    std::size_t parts = 0;
    while (heapById(id) != nullptr) {
        ASSERT_LT(parts, 10000U);
        EXPECT_LE(releaser.reapPendingPart(), 1U);
        ++parts;
    }
    EXPECT_GE(parts, 1U);
    EXPECT_EQ(releaser.pendingDeadHeaps(), 0U);
}

// A part that drops the last count of a second dead heap hands that heap to
// the same heap, and does not destroy both in one part.
TEST(DeadHeapTest, APartHandsTheNextDeadHeapToTheSameHeap) {
    MemoryManager releaser;
    releaser.setDestroyPartUnits(1);
    auto* outer = new MemoryManager(1);
    auto* inner = new MemoryManager(2);
    const std::uint16_t outerId = outer->heapId();
    const std::uint16_t innerId = inner->heapId();
    // The outer heap holds one object of the inner heap, and the releaser
    // holds one object of the outer heap.
    Obj* innerObj = inner->create<ObjInt>(1);
    MemoryManager::holdValue(Value{innerObj});
    outer->receiveMessage(Value{innerObj});
    // A root of the outer heap names the foreign object, as the receiver of a
    // message does. Without it, a collection of the outer heap drops the entry
    // and the inner heap dies at its own dropOwner.
    outer->setMarkRootsCallback(
        [outer, innerObj] { outer->markObject(innerObj); });
    Obj* outerHeld = outer->create<ObjInt>(2);
    outer->hold(outerHeld);
    inner->dropOwner();
    outer->dropOwner();

    releaser.releaseValue(Value{outerHeld});
    ASSERT_EQ(releaser.pendingDeadHeaps(), 1U);

    std::size_t parts = 0;
    while (heapById(outerId) != nullptr) {
        ASSERT_LT(parts++, 10000U);
        releaser.reapPendingPart();
    }
    // The part that dropped the last count of the inner heap did not destroy
    // it: it waits for a part of its own.
    ASSERT_NE(heapById(innerId), nullptr);
    EXPECT_EQ(releaser.pendingDeadHeaps(), 1U);
    parts = 0;
    while (heapById(innerId) != nullptr) {
        ASSERT_LT(parts++, 10000U);
        EXPECT_GT(releaser.reapPendingPart(), 0U);
    }
    EXPECT_EQ(releaser.pendingDeadHeaps(), 0U);
}
