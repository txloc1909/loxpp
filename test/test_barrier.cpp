#include "collector.h"
#include "container_objects.h"
#include "memory_manager.h"
#include "objects.h"
#include "value.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>

// The mutators of the heap object types run the write barrier hook with the
// old and the new value (docs/decisions/garbage-collectors.md, section 6).
// tools/check_gc_barrier_table.py checks that each mutator reaches its hook.
// These tests check what the hook gets.

namespace {

class RecordingCollector final : public Collector {
  public:
    struct StoreCall {
        Store kind;
        Obj* container;
        Value oldv;
        Value newv;
    };
    explicit RecordingCollector(PollState& poll) : Collector(poll) {}
    void poll() override {}
    void onCreate(Obj*) override {}
    void markObject(Obj*) override {}
    void collectNow(CollectCause) override {}
    std::size_t collectionCount() const override { return 0; }
    void releaseAll() override {}
    std::vector<StoreCall> stores;

  protected:
    void onStore(Store kind, Obj* c, const Value& o, const Value& n) override {
        stores.push_back({kind, c, o, n});
    }
};

// A heap whose collector records the stores. The objects of a test are made
// before the collector is put in place, as swapCollector requires.
struct Rig {
    MemoryManager mm;
    std::unique_ptr<Collector> original;
    RecordingCollector* rec{nullptr};

    void arm(bool marking) {
        auto c = std::make_unique<RecordingCollector>(mm.pollState());
        rec = c.get();
        original = mm.swapCollector(std::move(c));
        mm.pollState().marking = marking;
    }
    ~Rig() {
        mm.pollState().marking = false;
        if (original) {
            mm.swapCollector(std::move(original));
        }
    }
    ObjString* str(const char* s) {
        ObjString* o = mm.makeString(s);
        mm.pushTempRoot(o);
        return o;
    }
};

Value num(double d) { return Value{d}; }
Value ref(Obj* o) { return Value{o}; }

} // namespace

TEST(BarrierTest, AListStoreReportsTheOldAndTheNewValue) {
    Rig r;
    auto* list = r.mm.create<ObjList>(VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(list);
    list->push(num(1));
    list->push(num(2));
    list->push(num(3));
    r.arm(true);

    list->push(num(4));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Element);
    EXPECT_TRUE(is<Nil>(r.rec->stores[0].oldv));
    EXPECT_EQ(as<Number>(r.rec->stores[0].newv), 4.0);

    r.rec->stores.clear();
    list->set(1, num(9));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 2.0);
    EXPECT_EQ(as<Number>(r.rec->stores[0].newv), 9.0);

    r.rec->stores.clear();
    Value popped = list->pop();
    EXPECT_EQ(as<Number>(popped), 4.0);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 4.0);
    EXPECT_TRUE(is<Nil>(r.rec->stores[0].newv));
}

TEST(BarrierTest, AnErasedListElementReportsEachSlotItOverwrites) {
    Rig r;
    auto* list = r.mm.create<ObjList>(VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(list);
    for (int i = 1; i <= 4; ++i) {
        list->push(num(i));
    }
    r.arm(true);
    list->eraseAt(1);
    // Slots 1, 2 and 3 are overwritten: 2 by 3, 3 by 4, and 4 by nothing.
    ASSERT_EQ(r.rec->stores.size(), 3u);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 2.0);
    EXPECT_EQ(as<Number>(r.rec->stores[0].newv), 3.0);
    EXPECT_EQ(as<Number>(r.rec->stores[1].oldv), 3.0);
    EXPECT_EQ(as<Number>(r.rec->stores[1].newv), 4.0);
    EXPECT_EQ(as<Number>(r.rec->stores[2].oldv), 4.0);
    EXPECT_TRUE(is<Nil>(r.rec->stores[2].newv));
}

TEST(BarrierTest, AShrinkingListResizeReportsTheDroppedElements) {
    Rig r;
    auto* list = r.mm.create<ObjList>(VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(list);
    for (int i = 1; i <= 3; ++i) {
        list->push(num(i));
    }
    r.arm(true);
    list->resize(1);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 2.0);
    EXPECT_EQ(as<Number>(r.rec->stores[1].oldv), 3.0);
    r.rec->stores.clear();
    list->resize(5);
    EXPECT_TRUE(r.rec->stores.empty());
}

TEST(BarrierTest, NoMutatorCallsTheHookWhenNoMarkRuns) {
    Rig r;
    auto* list = r.mm.create<ObjList>(VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(list);
    r.arm(false);
    list->push(num(1));
    list->set(0, num(2));
    list->pop();
    EXPECT_TRUE(r.rec->stores.empty());
}

TEST(BarrierTest, AMapStoreReportsTheOverwrittenValueAndARemovedKeyAndValue) {
    Rig r;
    ObjString* k = r.str("k");
    ObjString* v1 = r.str("v1");
    ObjString* v2 = r.str("v2");
    auto* map = r.mm.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&r.mm});
    r.mm.pushTempRoot(map);
    map->mapSet(ref(k), ref(v1));
    r.arm(true);

    map->mapSet(ref(k), ref(v2)); // overwrite
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Entry);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), v1);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].newv), v2);

    r.rec->stores.clear();
    map->mapDel(ref(k)); // the key and the value, in two calls
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), k);
    EXPECT_EQ(as<Obj*>(r.rec->stores[1].oldv), v2);
    EXPECT_TRUE(is<Nil>(r.rec->stores[1].newv));
}

TEST(BarrierTest, AHashedMapStoreReportsTheKeyAndTheValueItReplaces) {
    Rig r;
    ObjString* k = r.str("k");
    ObjString* v1 = r.str("v1");
    ObjString* v2 = r.str("v2");
    auto* map = r.mm.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&r.mm});
    r.mm.pushTempRoot(map);
    map->mapSet(ref(k), ref(v1));
    r.arm(true);
    bool failed = false;
    ObjMap::KeyEq eq = [](const Value& a, const Value& b) { return a == b; };
    map->mapSetHashed(ref(k), ref(v2), hashValue(ref(k)), eq, failed);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), k);
    EXPECT_EQ(as<Obj*>(r.rec->stores[1].oldv), v1);
    EXPECT_EQ(as<Obj*>(r.rec->stores[1].newv), v2);
    r.rec->stores.clear();
    map->mapDelHashed(ref(k), hashValue(ref(k)), eq, failed);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), k);
    EXPECT_EQ(as<Obj*>(r.rec->stores[1].oldv), v2);
}

TEST(BarrierTest, AnInstanceFieldAndAClassMethodReportTheirOldValue) {
    Rig r;
    ObjString* name = r.str("n");
    ObjString* a = r.str("a");
    ObjString* b = r.str("b");
    auto* klass = r.mm.create<ObjClass>(name, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(klass);
    auto* sup = r.mm.create<ObjClass>(name, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(sup);
    auto* inst = r.mm.create<ObjInstance>(klass, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(inst);
    inst->setField(name, ref(a));
    klass->setMethod(name, ref(a));
    r.arm(true);

    inst->setField(name, ref(b));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Field);
    EXPECT_EQ(r.rec->stores[0].container, inst);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), a);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].newv), b);

    r.rec->stores.clear();
    klass->setMethod(name, ref(b));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), a);

    r.rec->stores.clear();
    klass->setSuperclass(sup);
    klass->setSuperclass(nullptr);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_TRUE(is<Nil>(r.rec->stores[0].oldv));
    EXPECT_EQ(as<Obj*>(r.rec->stores[1].oldv), sup);
}

TEST(BarrierTest, AnUpvalueStoreIsAnUpvalueStoreOrASnapshotStore) {
    Rig r;
    Value cell = num(1);
    auto* uv = r.mm.create<ObjUpvalue>(&cell);
    r.mm.pushTempRoot(uv);
    auto* co = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                         VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(co);
    r.arm(true);
    Collector& c = r.mm.collector();

    uv->set(c, num(2)); // open, on the value stack: a root
    EXPECT_TRUE(r.rec->stores.empty());

    uv->close(c);
    r.rec->stores.clear();
    uv->set(c, num(3)); // closed
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Upvalue);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 2.0);

    r.rec->stores.clear();
    Value snapshotCell = num(5);
    uv->setLocation(&snapshotCell);
    uv->setOwner(c, co);
    r.rec->stores.clear();
    uv->set(c, num(6)); // a cell of the snapshot of its owner
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Snapshot);
    EXPECT_EQ(r.rec->stores[0].container, co);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 5.0);

    r.rec->stores.clear();
    uv->setOwner(c, nullptr);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), co);
}

TEST(BarrierTest, AClosureSlotAndAnIteratorCacheReportTheirOldValue) {
    Rig r;
    auto* fn = r.mm.create<ObjFunction>();
    fn->upvalueCount = 1;
    r.mm.pushTempRoot(fn);
    auto* closure = r.mm.create<ObjClosure>(fn);
    r.mm.pushTempRoot(closure);
    Value cell = num(0);
    auto* uv1 = r.mm.create<ObjUpvalue>(&cell);
    r.mm.pushTempRoot(uv1);
    auto* uv2 = r.mm.create<ObjUpvalue>(&cell);
    r.mm.pushTempRoot(uv2);
    auto* it = r.mm.create<ObjIterator>(num(0));
    r.mm.pushTempRoot(it);
    Collector& c = r.mm.collector();
    closure->setUpvalue(c, 0, uv1);
    r.arm(true);
    // r.mm.collector() is the recording collector now.
    Collector& rc = r.mm.collector();
    (void)c;

    closure->setUpvalue(rc, 0, uv2);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Element);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].oldv), uv1);
    EXPECT_EQ(as<Obj*>(r.rec->stores[0].newv), uv2);

    r.rec->stores.clear();
    it->setCurrent(rc, num(7));
    it->setCurrent(rc, num(8));
    Value taken = it->takeCurrent(rc);
    EXPECT_EQ(as<Number>(taken), 8.0);
    ASSERT_EQ(r.rec->stores.size(), 3u);
    EXPECT_TRUE(is<Nil>(r.rec->stores[0].oldv));
    EXPECT_EQ(as<Number>(r.rec->stores[1].oldv), 7.0);
    EXPECT_EQ(as<Number>(r.rec->stores[2].oldv), 8.0);
}

TEST(BarrierTest, AMailboxAndASnapshotReportEachReferenceTheyDrop) {
    Rig r;
    auto* task = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(task);
    auto* co = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                         VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(co);
    task->pushMessage(num(1));
    task->pushMessage(num(2));
    const Value slice[2] = {num(10), num(11)};
    co->captureStack(slice, slice + 2);
    r.arm(true);

    EXPECT_EQ(as<Number>(task->popMessage()), 1.0);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Element);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 1.0);

    r.rec->stores.clear();
    task->setPendingValue(num(3));
    task->setPendingValue(num(4));
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Field);
    EXPECT_EQ(as<Number>(r.rec->stores[1].oldv), 3.0);

    r.rec->stores.clear();
    co->clearSnapshot();
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Snapshot);
    EXPECT_EQ(as<Number>(r.rec->stores[0].oldv), 10.0);
    EXPECT_EQ(as<Number>(r.rec->stores[1].oldv), 11.0);
}
