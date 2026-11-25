#include "class_objects.h"
#include "collector.h"
#include "container_objects.h"
#include "exec_objects.h"
#include "incremental_collector.h"
#include "memory_manager.h"
#include "value.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

// The incremental collector (docs/decisions/garbage-collectors.md, section 4).
// The tests drive the steps by hand, so the program and the collector mix at
// the points that each case names. The heap has no automatic collection here:
// every cycle is one that the test starts.

namespace {

Value ref(Obj* o) { return Value{o}; }
Value nil() { return Value{Nil{}}; }

class IncrementalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mm.pollState().stress = false;
        old = mm.swapCollector(
            std::make_unique<IncrementalCollector>(mm, mm.pollState(), 0));
        ic = static_cast<IncrementalCollector*>(&mm.collector());
        // No allocation starts a cycle: the test does.
        mm.pollState().pollAt = ~std::size_t{0};
    }
    void TearDown() override {
        // A collection from the middle of a cycle ends it and leaves no mark.
        mm.collectGarbage();
        mm.swapCollector(std::move(old));
    }

    ObjList* newList() { return mm.create<ObjList>(VmAllocator<Value>{&mm}); }
    ObjMap* newMap() {
        return mm.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&mm});
    }
    ObjClass* newClass(const char* name) {
        return mm.create<ObjClass>(mm.makeString(name),
                                   VmAllocator<Entry>{&mm});
    }
    ObjInstance* newInstance(ObjClass* k) {
        return mm.create<ObjInstance>(k, VmAllocator<Entry>{&mm});
    }

    // Runs the mark until the gray stack is empty.
    void finishTracing() {
        while (!ic->markStep(1000)) {
        }
    }
    // The cycle in progress has `root` as its only root, and has not traced
    // it. Then `x` becomes reachable through a root that the cycle never
    // reads (a root that appears after the start is not scanned: the snapshot
    // is the barrier's job). The caller overwrites the reference to `x` in
    // `root` next. It returns the number of reachable white objects after the
    // mark: it is 0 only if the barrier made `x` gray.
    std::size_t whiteAfterTheMark() {
        finishTracing();
        return ic->verify();
    }
    void startWithRoot(Obj* root) {
        mm.pushTempRoot(root);
        ic->startCycle("test");
    }

    MemoryManager mm;
    std::unique_ptr<Collector> old;
    IncrementalCollector* ic{nullptr};
};

// --- One case for each store that the barrier covers ---------------------

TEST_F(IncrementalTest, ListSetKeepsTheOldElement) {
    ObjList* c = newList();
    ObjList* x = newList();
    c->push(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->set(0, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ListPopKeepsTheOldElement) {
    ObjList* c = newList();
    ObjList* x = newList();
    c->push(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->pop();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ListResizeKeepsTheOldElement) {
    ObjList* c = newList();
    ObjList* x = newList();
    c->push(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->resize(0);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ListEraseAtKeepsTheOldElement) {
    ObjList* c = newList();
    ObjList* x = newList();
    c->push(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->eraseAt(0);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, InstanceSetFieldKeepsTheOldValue) {
    ObjInstance* c = newInstance(newClass("K"));
    ObjString* key = mm.makeString("f");
    ObjList* x = newList();
    c->setField(key, ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setField(key, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, MapSetKeepsTheOldValue) {
    ObjMap* c = newMap();
    ObjString* key = mm.makeString("k");
    ObjList* x = newList();
    c->mapSet(ref(key), ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->mapSet(ref(key), nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, MapDelKeepsTheOldValue) {
    ObjMap* c = newMap();
    ObjString* key = mm.makeString("k");
    ObjList* x = newList();
    c->mapSet(ref(key), ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->mapDel(ref(key));
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ClassSetMethodKeepsTheOldMethod) {
    ObjClass* c = newClass("K");
    ObjString* key = mm.makeString("m");
    ObjList* x = newList();
    c->setMethod(key, ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setMethod(key, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ClassSetSuperclassKeepsTheOldSuperclass) {
    ObjClass* c = newClass("K");
    ObjClass* x = newClass("S");
    c->setSuperclass(x);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setSuperclass(nullptr);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, IteratorSetCurrentKeepsTheOldValue) {
    ObjIterator* c = mm.create<ObjIterator>(nil());
    ObjList* x = newList();
    c->setCurrent(*ic, ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setCurrent(*ic, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, ClosedUpvalueSetKeepsTheOldValue) {
    ObjList* x = newList();
    Value cell = ref(x);
    ObjUpvalue* c = mm.create<ObjUpvalue>(&cell);
    c->close(*ic);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->set(*ic, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, UpvalueSetOwnerKeepsTheOldOwner) {
    ObjList* x = newList();
    Value cell = nil();
    ObjUpvalue* c = mm.create<ObjUpvalue>(&cell);
    c->setOwner(*ic, x);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setOwner(*ic, nullptr);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, CoroutineClearSnapshotKeepsTheOldValues) {
    ObjCoroutine* c =
        mm.create<ObjCoroutine>(nullptr, nil(), VmAllocator<Value>{&mm});
    ObjList* x = newList();
    Value slot = ref(x);
    c->captureStack(&slot, &slot + 1);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->clearSnapshot();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

// The same case, with the container already traced when the store happens.
// The old value is the only thing that makes the object reachable for the
// mark, so this is the bug class of the decision, 6.1, item 1: objects move
// between old containers in both directions at the same time.
TEST_F(IncrementalTest, ObjectsMovedBetweenTwoLargeListsInBothDirectionsLive) {
    ObjList* a = newList();
    ObjList* b = newList();
    std::vector<ObjList*> items;
    for (int i = 0; i < 2000; ++i) {
        ObjList* x = newList();
        items.push_back(x);
        (i % 2 == 0 ? a : b)->push(ref(x));
    }
    mm.pushTempRoot(a);
    mm.pushTempRoot(b);
    ic->startCycle("test");
    // The mark has traced a part of both lists when the moves start.
    ic->markStep(300);
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < 1000; ++i) {
            ObjList* from = i % 2 == 0 ? a : b;
            ObjList* to = i % 2 == 0 ? b : a;
            // Move the last element of one list to the other, and let a new
            // one take its place.
            Value moved = from->elements().back();
            from->pop();
            to->push(moved);
            ic->markStep(16);
        }
    }
    finishTracing();
    EXPECT_EQ(ic->verify(), 0u);
}

// --- The tracer in parts --------------------------------------------------

TEST_F(IncrementalTest, ALargeListIsTracedInPartsAndNoStepGrowsWithIt) {
    ObjList* big = newList();
    for (int i = 0; i < 50000; ++i) {
        big->push(ref(newList()));
    }
    mm.pushTempRoot(big);
    ic->startCycle("test");
    std::size_t steps = 0;
    while (!ic->markStep(512)) {
        ++steps;
    }
    EXPECT_GT(steps, 50000u / (512 + IncrementalCollector::kChunkUnits) / 2);
    // A step stops after the chunk that crosses its limit.
    EXPECT_LE(ic->largestStepUnits(),
              512 + 2 * IncrementalCollector::kChunkUnits);
    ic->finishMark();
    while (!ic->sweepStep(4096)) {
    }
    EXPECT_LE(ic->largestStepUnits(), 4096u);
}

TEST_F(IncrementalTest, AMapThatRehashesWhileItIsTracedStartsAgain) {
    ObjMap* map = newMap();
    std::vector<ObjString*> keys;
    for (int i = 0; i < 600; ++i) {
        keys.push_back(mm.makeString("key" + std::to_string(i)));
        map->mapSet(ref(keys.back()), ref(newList()));
    }
    mm.pushTempRoot(map);
    ic->startCycle("test");
    // A part of the buckets is traced, then the table grows.
    ic->markStep(200);
    for (int i = 600; i < 3000; ++i) {
        ObjString* k = mm.makeString("key" + std::to_string(i));
        mm.pushTempRoot(k);
        map->mapSet(ref(k), ref(newList()));
        mm.popTempRoot();
        ic->markStep(1);
    }
    finishTracing();
    EXPECT_EQ(ic->verify(), 0u);
}

// --- Colors of new objects ------------------------------------------------

TEST_F(IncrementalTest, AnObjectMadeDuringTheMarkIsBlack) {
    ic->startCycle("test");
    ObjList* made = newList();
    EXPECT_TRUE(mm.store().isMarked(made));
    mm.pushTempRoot(made); // stays alive after the cycle
    finishTracing();
    ic->finishMark();
    while (!ic->sweepStep(4096)) {
    }
    // The sweep clears the mark: the object is white at the next cycle.
    EXPECT_FALSE(mm.store().isMarked(made));
    EXPECT_EQ(ic->phase(), IncrementalCollector::Phase::Idle);
}

TEST_F(IncrementalTest, AnObjectMadeDuringTheSweepLivesThroughTheCycle) {
    std::vector<ObjList*> before;
    for (int i = 0; i < 6000; ++i) {
        before.push_back(newList()); // dead: nothing reaches them
    }
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    ic->sweepStep(1);
    std::vector<ObjList*> made;
    for (int i = 0; i < 6000; ++i) {
        ObjList* o = newList();
        made.push_back(o);
        // Marked exactly when the sweep has not reached its span.
        EXPECT_EQ(mm.store().isMarked(o), mm.store().isUnswept(o));
    }
    while (!ic->sweepStep(4096)) {
    }
    for (ObjList* o : made) {
        EXPECT_FALSE(mm.store().isMarked(o)); // white for the next cycle
        EXPECT_TRUE(o->elements().empty());   // and alive
    }
    // Every one of the made objects is still counted, and the dead ones are
    // not.
    EXPECT_GE(mm.store().objectCount(), made.size());
    EXPECT_LT(mm.store().objectCount(), before.size() + made.size());
}

// --- The read barrier of the intern table --------------------------------

TEST_F(IncrementalTest, ADeadStringFoundAgainDuringTheMarkSurvives) {
    ObjString* s = mm.makeString("dead but found again");
    ic->startCycle("test");
    EXPECT_EQ(mm.makeString("dead but found again"), s);
    finishTracing();
    ic->finishMark();
    while (!ic->sweepStep(4096)) {
    }
    EXPECT_EQ(mm.findString("dead but found again"), s);
}

TEST_F(IncrementalTest, ADeadStringFoundAgainDuringTheSweepSurvives) {
    ObjString* s = mm.makeString("dead but found again");
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    ASSERT_EQ(ic->phase(), IncrementalCollector::Phase::Sweep);
    EXPECT_EQ(mm.makeString("dead but found again"), s);
    while (!ic->sweepStep(4096)) {
    }
    EXPECT_EQ(mm.findString("dead but found again"), s);
}

TEST_F(IncrementalTest, ADeadStringThatNobodyFindsIsFreedAndLeavesTheTable) {
    mm.makeString("really dead");
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    while (!ic->sweepStep(4096)) {
    }
    EXPECT_EQ(mm.findString("really dead"), nullptr);
}

// --- The finish pause and the handle list ---------------------------------

TEST_F(IncrementalTest, AnUnreachableFileIsFreedInTheFinishPause) {
    ObjClass* klass = newClass("File");
    mm.pushTempRoot(klass);
    mm.create<ObjFile>(klass); // nothing reaches it
    EXPECT_EQ(ic->handleCount(), 1u);
    ic->startCycle("test");
    finishTracing();
    const std::size_t before = mm.store().objectCount();
    ic->finishMark();
    // The finish pause freed it: the sweep has not started yet.
    EXPECT_EQ(mm.store().objectCount(), before - 1);
    EXPECT_EQ(ic->handleCount(), 0u);
    while (!ic->sweepStep(4096)) {
    }
}

TEST_F(IncrementalTest, AReachableFileStaysInTheHandleList) {
    ObjClass* klass = newClass("File");
    mm.pushTempRoot(klass);
    ObjFile* file = mm.create<ObjFile>(klass);
    mm.pushTempRoot(file);
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    while (!ic->sweepStep(4096)) {
    }
    EXPECT_EQ(ic->handleCount(), 1u);
}

// --- Whole cycles ----------------------------------------------------------

TEST_F(IncrementalTest, ACollectionFromTheMiddleOfACycleFreesWhatDiedSince) {
    ObjList* kept = newList();
    mm.pushTempRoot(kept);
    ObjList* dies = newList();
    kept->push(ref(dies));
    ic->startCycle("test");
    // After the start, `dies` is dropped. The cycle in progress keeps it (it
    // was reachable when the mark began), but a full collection must not.
    kept->pop();
    const std::size_t before = mm.store().objectCount();
    mm.collectGarbage();
    EXPECT_EQ(ic->phase(), IncrementalCollector::Phase::Idle);
    EXPECT_EQ(mm.store().objectCount(), before - 1);
}

TEST_F(IncrementalTest, AnAllocationLoopRunsWholeCycles) {
    // Now allocations poll: the first one is above the threshold.
    mm.pollState().pollAt = 0;
    ObjList* kept = newList();
    mm.pushTempRoot(kept);
    for (int i = 0; i < 200000; ++i) {
        mm.create<ObjList>(VmAllocator<Value>{&mm}); // dropped at once
        if (i % 4 == 0) {
            kept->push(ref(newList()));
        }
    }
    EXPECT_GT(ic->collectionCount(), 0u);
    // The garbage does not pile up: the heap stays near the live size.
    const std::size_t live = kept->elements().size() * (sizeof(ObjList) + 16);
    EXPECT_LT(mm.bytesAllocated, 6 * live + (std::size_t{8} << 20));
}

} // namespace
