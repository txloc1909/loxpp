#include "collector.h"
#include "container_objects.h"
#include "memory_manager.h"
#include "objects.h"
#include "value.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
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
    // A collector that starts a mark when the heap polls, as an incremental
    // collector does in the middle of an allocation. The test arms it by
    // setting `startMarkOnPoll` and a threshold of 0.
    void poll() override {
        if (startMarkOnPoll) {
            startMarkOnPoll = false;
            m_poll.marking = true;
            m_poll.pollAt = ~std::size_t{0};
        }
    }
    bool startMarkOnPoll{false};
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
    // No mark runs now. The first allocation after this call starts one.
    void armMarkOnGrowth() {
        arm(false);
        rec->startMarkOnPoll = true;
        mm.pollState().pollAt = 0;
    }
    ~Rig() {
        mm.pollState().pollAt = ~std::size_t{0};
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

// The old and the new value of each recorded call, as objects (null for a
// value that is not an object).
Obj* objOf(const Value& v) { return isObj(v) ? as<Obj*>(v) : nullptr; }

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

// --- one probe for each protected mutator that has no case above. Each probe
// makes the old value matter: the old value of the store is a different
// object from the new one, so a hook that gets the new value, Nil or no call
// fails it.

TEST(BarrierTest, SetCoReportsTheOldCoroutine) {
    Rig r;
    auto* task = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(task);
    auto* a = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                        VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(a);
    auto* b = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                        VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(b);
    task->setCo(a);
    r.arm(true);
    task->setCo(b);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Field);
    EXPECT_EQ(r.rec->stores[0].container, task);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), a);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), b);
}

TEST(BarrierTest, TheHoldersOfASocketAndAServerReportTheOldTask) {
    Rig r;
    auto* t1 = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(t1);
    auto* t2 = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(t2);
    auto* sock = r.mm.create<ObjSocket>(nullptr);
    r.mm.pushTempRoot(sock);
    auto* srv = r.mm.create<ObjServer>(nullptr);
    r.mm.pushTempRoot(srv);
    r.arm(true);
    Collector& c = r.mm.collector();
    sock->setReadHolder(c, t1);
    sock->setReadHolder(c, t2);
    sock->setWriteHolder(c, t2);
    sock->setWriteHolder(c, t1);
    srv->setAcceptHolder(c, t1);
    srv->setAcceptHolder(c, t2);
    ASSERT_EQ(r.rec->stores.size(), 6u);
    EXPECT_EQ(r.rec->stores[0].container, sock);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), nullptr);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), t1);
    EXPECT_EQ(objOf(r.rec->stores[1].newv), t2);
    EXPECT_EQ(objOf(r.rec->stores[2].oldv), nullptr);
    EXPECT_EQ(objOf(r.rec->stores[3].oldv), t2);
    EXPECT_EQ(objOf(r.rec->stores[3].newv), t1);
    EXPECT_EQ(r.rec->stores[4].container, srv);
    EXPECT_EQ(objOf(r.rec->stores[5].oldv), t1);
    EXPECT_EQ(objOf(r.rec->stores[5].newv), t2);
}

TEST(BarrierTest, AMailboxReportsEachMessageThatItDropsOrShifts) {
    Rig r;
    ObjString* m0 = r.str("m0");
    ObjString* m1 = r.str("m1");
    ObjString* m2 = r.str("m2");
    auto* task = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(task);
    r.arm(true);
    task->pushMessage(ref(m0));
    task->pushMessage(ref(m1));
    task->pushMessage(ref(m2));
    ASSERT_EQ(r.rec->stores.size(), 3u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), nullptr);
    EXPECT_EQ(objOf(r.rec->stores[2].newv), m2);

    r.rec->stores.clear();
    task->eraseMessage(0);
    ASSERT_EQ(r.rec->stores.size(), 3u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), m0);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), m1);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), m1);
    EXPECT_EQ(objOf(r.rec->stores[1].newv), m2);
    EXPECT_EQ(objOf(r.rec->stores[2].oldv), m2);
    EXPECT_EQ(objOf(r.rec->stores[2].newv), nullptr);

    r.rec->stores.clear();
    task->clearMailbox(); // m1 and m2 are left
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), m1);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), m2);
}

TEST(BarrierTest, ATaskReportsTheOldStartArgumentAndTestFunction) {
    Rig r;
    ObjString* a = r.str("a");
    ObjString* b = r.str("b");
    ObjString* w = r.str("w");
    auto* task = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(task);
    task->setStartArg(ref(a));
    task->setSelectWant(ref(w));
    r.arm(true);
    task->setStartArg(ref(b));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), a);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), b);
    r.rec->stores.clear();
    task->resetSelect(); // drops the test function
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), w);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), nullptr);
    r.rec->stores.clear();
    task->setSelectWant(ref(b));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), nullptr);
}

TEST(BarrierTest, TheWaitingCallOfATaskReportsTheOldTargetAndTheOldText) {
    Rig r;
    ObjString* t1 = r.str("t1");
    ObjString* x1 = r.str("x1");
    ObjString* t2 = r.str("t2");
    ObjString* x2 = r.str("x2");
    auto* task = r.mm.create<ObjTask>(nullptr, 0, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(task);
    WaitingCall first;
    first.target = t1;
    first.text = x1;
    task->setWait(std::move(first));
    r.arm(true);
    WaitingCall second;
    second.target = t2;
    second.text = x2;
    task->setWait(std::move(second));
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), t1);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), t2);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), x1);
    EXPECT_EQ(objOf(r.rec->stores[1].newv), x2);
    r.rec->stores.clear();
    task->clearWait();
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), t2);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), x2);
}

TEST(BarrierTest, AnEnumReportsTheOldFieldAndTheDroppedFields) {
    Rig r;
    ObjString* name = r.str("n");
    ObjString* a = r.str("a");
    ObjString* b = r.str("b");
    ObjString* c = r.str("c");
    auto* ctor = r.mm.create<ObjEnumCtor>(0, 0, name, name);
    r.mm.pushTempRoot(ctor);
    auto* e = r.mm.create<ObjEnum>(ctor, VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(e);
    e->resize(3);
    e->set(0, ref(a));
    e->set(1, ref(b));
    e->set(2, ref(c));
    r.arm(true);
    e->set(0, ref(b));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Element);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), a);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), b);
    r.rec->stores.clear();
    e->resize(1);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), b);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), c);
}

TEST(BarrierTest, ADeferredCallReportsTheArgumentThatItAdds) {
    Rig r;
    ObjString* a = r.str("a");
    auto* d = r.mm.create<ObjDeferredCall>(num(0), VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(d);
    r.arm(true);
    d->addArg(ref(a));
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Element);
    EXPECT_EQ(r.rec->stores[0].container, d);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), nullptr);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), a);
}

TEST(BarrierTest, InheritingMethodsReportsTheMethodThatItReplaces) {
    Rig r;
    ObjString* name = r.str("m");
    ObjString* mine = r.str("mine");
    ObjString* theirs = r.str("theirs");
    auto* sub = r.mm.create<ObjClass>(name, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(sub);
    auto* sup = r.mm.create<ObjClass>(name, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(sup);
    sub->setMethod(name, ref(mine));
    sup->setMethod(name, ref(theirs));
    r.arm(true);
    sub->inheritMethods(*sup);
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].container, sub);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), mine);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), theirs);
}

TEST(BarrierTest, ClosingAnUpvalueReportsItsOldOwner) {
    Rig r;
    Value cell = num(1);
    auto* uv = r.mm.create<ObjUpvalue>(&cell);
    r.mm.pushTempRoot(uv);
    auto* co = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                         VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(co);
    uv->setOwner(r.mm.collector(), co);
    r.arm(true);
    uv->close(r.mm.collector());
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Upvalue);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), co);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), nullptr);
}

TEST(BarrierTest, ASnapshotReportsEachReferenceItAddsAndDrops) {
    Rig r;
    auto* fn = r.mm.create<ObjFunction>();
    r.mm.pushTempRoot(fn);
    auto* closure = r.mm.create<ObjClosure>(fn);
    r.mm.pushTempRoot(closure);
    ObjString* dv = r.str("defer");
    ObjString* ov = r.str("override");
    ObjString* sv = r.str("stack");
    Value cell = num(0);
    auto* uv = r.mm.create<ObjUpvalue>(&cell);
    r.mm.pushTempRoot(uv);
    auto* co = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                         VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(co);
    r.arm(true);

    const Value stackSlice[1] = {ref(sv)};
    co->captureStack(stackSlice, stackSlice + 1);
    co->reserveFrames(1);
    co->pushFrame(CoroutineFrameSnapshot{closure, 0, 0, 0});
    const std::vector<Value> defers{ref(dv)};
    co->pushDefers(defers.begin(), defers.end());
    co->pushResult(1, true, ref(ov));
    co->reserveOpenUpvalues(1);
    co->pushOpenUpvalue(uv, 0);
    // Each push reports its new reference, with Nil as the old value.
    ASSERT_EQ(r.rec->stores.size(), 5u);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), sv);
    EXPECT_EQ(objOf(r.rec->stores[1].newv), closure);
    EXPECT_EQ(objOf(r.rec->stores[2].newv), dv);
    EXPECT_EQ(objOf(r.rec->stores[3].newv), ov);
    EXPECT_EQ(objOf(r.rec->stores[4].newv), uv);
    for (const auto& st : r.rec->stores) {
        EXPECT_EQ(st.kind, Store::Snapshot);
        EXPECT_EQ(st.container, co);
        EXPECT_EQ(objOf(st.oldv), nullptr);
    }

    // A resume drops all of them: each one is the old value of a call.
    r.rec->stores.clear();
    co->clearSnapshot();
    ASSERT_EQ(r.rec->stores.size(), 5u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), sv);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), closure);
    EXPECT_EQ(objOf(r.rec->stores[2].oldv), dv);
    EXPECT_EQ(objOf(r.rec->stores[3].oldv), uv);
    EXPECT_EQ(objOf(r.rec->stores[4].oldv), ov);
    for (const auto& st : r.rec->stores) {
        EXPECT_EQ(objOf(st.newv), nullptr);
    }
}

TEST(BarrierTest, ATaskSwitchReportsTheOldInnerCoroutineAndTheOldIterator) {
    Rig r;
    auto* co = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                         VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(co);
    auto* in1 = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                          VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(in1);
    auto* in2 = r.mm.create<ObjCoroutine>(nullptr, Value{Nil{}},
                                          VmAllocator<Value>{&r.mm});
    r.mm.pushTempRoot(in2);
    auto* it1 = r.mm.create<ObjIterator>(num(0));
    r.mm.pushTempRoot(it1);
    auto* it2 = r.mm.create<ObjIterator>(num(0));
    r.mm.pushTempRoot(it2);
    co->setWaiting(in1, it1);
    r.arm(true);
    co->setWaiting(in2, it2);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), in1);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), in2);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), it1);
    EXPECT_EQ(objOf(r.rec->stores[1].newv), it2);
    r.rec->stores.clear();
    co->setWaiting(nullptr, nullptr);
    ASSERT_EQ(r.rec->stores.size(), 2u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), in2);
    EXPECT_EQ(objOf(r.rec->stores[1].oldv), it2);
}

// --- the order rule: the old value is read after the growth. A mark that
// starts inside the growth must still see the store. The table has 6 entries
// of a capacity of 8, so the next store grows it, also a store to an old key.

TEST(BarrierTest,
     AnInstanceFieldStoreThatGrowsReportsTheOldValueAfterTheGrowth) {
    Rig r;
    ObjString* name = r.str("n");
    auto* klass = r.mm.create<ObjClass>(name, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(klass);
    auto* inst = r.mm.create<ObjInstance>(klass, VmAllocator<Entry>{&r.mm});
    r.mm.pushTempRoot(inst);
    ObjString* keys[6];
    ObjString* oldv = r.str("old");
    ObjString* newv = r.str("new");
    for (int i = 0; i < 6; ++i) {
        keys[i] = r.str(("k" + std::to_string(i)).c_str());
        inst->setField(keys[i], i == 0 ? ref(oldv) : num(i));
    }
    r.armMarkOnGrowth();
    inst->setField(keys[0], ref(newv));
    ASSERT_TRUE(r.mm.pollState().marking) << "the store did not grow the table";
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), oldv);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), newv);
}

TEST(BarrierTest, AMapStoreThatGrowsReportsTheOldValueAfterTheGrowth) {
    Rig r;
    auto* map = r.mm.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&r.mm});
    r.mm.pushTempRoot(map);
    ObjString* keys[6];
    ObjString* oldv = r.str("old");
    ObjString* newv = r.str("new");
    for (int i = 0; i < 6; ++i) {
        keys[i] = r.str(("k" + std::to_string(i)).c_str());
        map->mapSet(ref(keys[i]), i == 0 ? ref(oldv) : num(i));
    }
    r.armMarkOnGrowth();
    map->mapSet(ref(keys[0]), ref(newv));
    ASSERT_TRUE(r.mm.pollState().marking) << "the store did not grow the map";
    ASSERT_EQ(r.rec->stores.size(), 1u);
    EXPECT_EQ(r.rec->stores[0].kind, Store::Entry);
    EXPECT_EQ(objOf(r.rec->stores[0].oldv), oldv);
    EXPECT_EQ(objOf(r.rec->stores[0].newv), newv);
}
