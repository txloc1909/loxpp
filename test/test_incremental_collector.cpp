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
    ObjTask* newTask() {
        return mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&mm});
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

// --- The stores of a Task, a Closure, an Upvalue snapshot cell and a
// --- Coroutine that wait or that hold state

TEST_F(IncrementalTest, TaskSetPendingValueKeepsTheOldValue) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    c->setPendingValue(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setPendingValue(nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskSetStartArgKeepsTheOldValue) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    c->setStartArg(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setStartArg(nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskSetSelectWantKeepsTheOldValue) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    c->setSelectWant(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->resetSelect();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskSetCoKeepsTheOldCoroutine) {
    ObjCoroutine* x =
        mm.create<ObjCoroutine>(nullptr, nil(), VmAllocator<Value>{&mm});
    ObjTask* c = newTask();
    c->setCo(x);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setCo(nullptr);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskSetWaitKeepsTheOldTargetAndText) {
    ObjTask* c = newTask();
    ObjList* target = newList();
    ObjList* text = newList();
    WaitingCall call;
    call.target = target;
    call.text = text;
    c->setWait(std::move(call));
    startWithRoot(c);
    mm.pushTempRoot(target);
    mm.pushTempRoot(text);
    c->clearWait();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskPopMessageKeepsTheOldMessage) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    c->pushMessage(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->popMessage();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskEraseMessageKeepsTheOldMessages) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    ObjList* y = newList();
    c->pushMessage(ref(x));
    c->pushMessage(ref(y));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->eraseMessage(0);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, TaskClearMailboxKeepsTheOldMessages) {
    ObjTask* c = newTask();
    ObjList* x = newList();
    c->pushMessage(ref(x));
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->clearMailbox();
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

// A receive moves each message that stays one place to the front. The tracer
// has traced the first part of the mailbox, and the messages that move into
// that part are not traced again unless the tracer moves its position.
TEST_F(IncrementalTest, AReceiveWhileTheMailboxIsTracedLosesNoMessage) {
    ObjTask* c = newTask();
    constexpr int kMessages = 400;
    for (int i = 0; i < kMessages; ++i) {
        c->pushMessage(
            ref(mm.makeString(("message " + std::to_string(i)).c_str())));
    }
    startWithRoot(c);
    // One step of the mark traces the first part of the mailbox and no more.
    EXPECT_FALSE(ic->markStep(1));
    for (int i = 0; i < 50; ++i) {
        c->popMessage();
    }
    EXPECT_EQ(whiteAfterTheMark(), 0u);
    // The receive moves the messages that stay, so the next ones to leave
    // are the right messages too.
    for (int i = 50; i < kMessages; ++i) {
        const Value m = c->popMessage();
        EXPECT_EQ(std::string(asObjString(as<Obj*>(m))->chars),
                  "message " + std::to_string(i));
    }
}

TEST_F(IncrementalTest, ClosureSetUpvalueKeepsTheOldUpvalue) {
    ObjFunction* fn = mm.create<ObjFunction>();
    fn->upvalueCount = 1;
    ObjClosure* c = mm.create<ObjClosure>(fn);
    Value cell = nil();
    ObjUpvalue* x = mm.create<ObjUpvalue>(&cell);
    c->setUpvalue(*ic, 0, x);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->setUpvalue(*ic, 0, nullptr);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, UpvalueSetOnASnapshotCellKeepsTheOldValue) {
    ObjCoroutine* owner =
        mm.create<ObjCoroutine>(nullptr, nil(), VmAllocator<Value>{&mm});
    ObjList* x = newList();
    Value cell = ref(x);
    ObjUpvalue* c = mm.create<ObjUpvalue>(&cell);
    c->setOwner(*ic, owner);
    startWithRoot(c);
    mm.pushTempRoot(x);
    c->set(*ic, nil());
    EXPECT_EQ(whiteAfterTheMark(), 0u);
}

TEST_F(IncrementalTest, CoroutineSetWaitingKeepsTheOldInnerAndIterator) {
    ObjCoroutine* c =
        mm.create<ObjCoroutine>(nullptr, nil(), VmAllocator<Value>{&mm});
    ObjCoroutine* inner =
        mm.create<ObjCoroutine>(nullptr, nil(), VmAllocator<Value>{&mm});
    ObjIterator* iter = mm.create<ObjIterator>(nil());
    c->setWaiting(inner, iter);
    startWithRoot(c);
    mm.pushTempRoot(inner);
    mm.pushTempRoot(iter);
    c->setWaiting(nullptr, nullptr);
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

// Strings whose hash has the low nine bits clear. In a table of 512 buckets
// they all start at bucket 0 and form one cluster. After the table doubles,
// the ones with bit 9 clear start at bucket 0 and the others at bucket 512, so
// an entry that sat far inside the cluster moves to a bucket near the start.
// An entry like that is in the part of the table that was not traced, and it
// moves into the part that was.
std::vector<std::string> collidingKeys(std::size_t count) {
    std::vector<std::string> keys;
    for (std::size_t i = 0; keys.size() < count; ++i) {
        std::string s = "key" + std::to_string(i);
        if ((hashString(s) & 511) == 0) {
            keys.push_back(s);
        }
    }
    return keys;
}

TEST_F(IncrementalTest, AMapThatRehashesWhileItIsTracedStartsAgain) {
    const std::vector<std::string> names = collidingKeys(420);
    ObjMap* map = newMap();
    for (std::size_t i = 0; i < 300; ++i) {
        map->mapSet(ref(mm.makeString(names[i])), ref(newList()));
    }
    ASSERT_EQ(map->entries().capacity(), 512);
    mm.pushTempRoot(map);
    ic->startCycle("test");
    // The first 128 buckets are traced, then the table grows.
    ic->markStep(1);
    ic->markStep(1);
    for (std::size_t i = 300; i < names.size(); ++i) {
        ObjString* k = mm.makeString(names[i]);
        mm.pushTempRoot(k);
        map->mapSet(ref(k), ref(newList()));
        mm.popTempRoot();
    }
    ASSERT_EQ(map->entries().capacity(), 1024);
    EXPECT_EQ(whiteAfterTheMark(), 0u);
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
    // Spans that are half full, so that the allocations of the sweep take
    // slots in spans that it has passed and in spans that it has not.
    ObjList* keep = newList();
    mm.pushTempRoot(keep);
    for (int i = 0; i < 12000; ++i) {
        ObjList* o = newList();
        if (i % 2 == 0) {
            keep->push(ref(o));
        }
    }
    mm.collectGarbage();
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    ic->sweepStep(1);
    std::vector<ObjList*> made;
    std::size_t unswept = 0;
    std::size_t swept = 0;
    for (int i = 0; i < 12000; ++i) {
        ObjList* o = newList();
        made.push_back(o);
        // Marked exactly when the sweep has not reached its span.
        EXPECT_EQ(mm.store().isMarked(o), mm.store().isUnswept(o));
        (mm.store().isUnswept(o) ? unswept : swept)++;
    }
    // The test reaches both kinds of span.
    EXPECT_GT(unswept, 0u);
    EXPECT_GT(swept, 0u);
    while (!ic->sweepStep(4096)) {
    }
    for (ObjList* o : made) {
        EXPECT_FALSE(mm.store().isMarked(o)); // white for the next cycle
        EXPECT_TRUE(o->elements().empty());   // and alive
    }
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

// The finish pause gives back the hold on a foreign String that nothing
// reached. The owner frees the String as soon as its count is zero, so the
// table of this heap must not hold it by then.
TEST_F(IncrementalTest, AForeignStringThatNothingReachedLeavesTheTable) {
    MemoryManager other;
    ObjString* theirs = other.makeString("a string of another heap");
    MemoryManager::holdValue(Value{static_cast<Obj*>(theirs)});
    mm.receiveMessage(Value{static_cast<Obj*>(theirs)});
    EXPECT_EQ(mm.findString("a string of another heap"), theirs);
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    while (!ic->sweepStep(1000)) {
    }
    EXPECT_EQ(mm.findString("a string of another heap"), nullptr);
}

TEST_F(IncrementalTest, AnUnreachableFileIsFreedByTheFirstSweepStep) {
    ObjClass* klass = newClass("File");
    mm.pushTempRoot(klass);
    mm.create<ObjFile>(klass); // nothing reaches it
    EXPECT_EQ(ic->handleCount(), 1u);
    ic->startCycle("test");
    finishTracing();
    const std::size_t before = mm.store().objectCount();
    ic->finishMark();
    // The finish pause leaves the handle list to the sweep steps.
    EXPECT_EQ(mm.store().objectCount(), before);
    EXPECT_EQ(ic->handleCount(), 1u);
    // A step of one unit frees the File and sweeps no span: the stages end
    // before the first span is swept.
    EXPECT_FALSE(ic->sweepStep(1));
    EXPECT_EQ(mm.store().objectCount(), before - 1);
    EXPECT_EQ(ic->handleCount(), 0u);
    while (!ic->sweepStep(4096)) {
    }
}

// A File object that owns no descriptor is a handle object for the list, so
// the test needs no raised descriptor limit.
TEST_F(IncrementalTest, TheHandleStageFreesAtMostTheUnitsOfAStep) {
    ObjClass* klass = newClass("File");
    mm.pushTempRoot(klass);
    const std::size_t n = 12000;
    for (std::size_t i = 0; i < n; ++i) {
        mm.create<ObjFile>(klass);
    }
    ASSERT_EQ(ic->handleCount(), n);
    ic->startCycle("test");
    finishTracing();
    ic->finishMark();
    EXPECT_EQ(ic->handleCount(), n);
    EXPECT_FALSE(ic->sweepStep(1000));
    EXPECT_GE(ic->handleCount(), n - 1000);
    EXPECT_LT(ic->handleCount(), n);
    while (!ic->sweepStep(1000)) {
    }
    EXPECT_EQ(ic->handleCount(), 0u);
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
