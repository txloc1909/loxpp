#include "collector.h"
#include "container_objects.h"
#include "memory_manager.h"
#include "value.h"

#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

// The interface between the runtime and a collector
// (docs/decisions/garbage-collectors.md, section 5).

namespace {

// A collector that records what the write barrier hooks pass on. The hooks
// are inline in Collector, so only a collector whose mark flag is set shows
// that each hook reaches onStore with its own store kind.
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
    void markObject(Obj* obj) override { marked.push_back(obj); }
    void collectNow(CollectCause) override {}
    std::size_t collectionCount() const override { return 0; }
    void releaseAll() override {}

    std::vector<StoreCall> stores;
    std::vector<ObjString*> interned;
    std::vector<Obj*> marked;

  protected:
    void onStore(Store kind, Obj* c, const Value& o, const Value& n) override {
        stores.push_back({kind, c, o, n});
    }
    void onInternHit(ObjString* s) override { interned.push_back(s); }
};

// The five hooks, each with the store kind that it must report.
struct HookCase {
    const char* name;
    Store kind;
    void (Collector::*hook)(Obj*, const Value&, const Value&);
};

const HookCase kHooks[] = {
    {"barrierField", Store::Field, &Collector::barrierField},
    {"barrierElement", Store::Element, &Collector::barrierElement},
    {"barrierEntry", Store::Entry, &Collector::barrierEntry},
    {"barrierUpvalue", Store::Upvalue, &Collector::barrierUpvalue},
    {"barrierSnapshot", Store::Snapshot, &Collector::barrierSnapshot},
};

} // namespace

TEST(CollectorHooksTest, EachHookReportsItsOwnStoreKindWhileAMarkRuns) {
    MemoryManager mm;
    ObjString* container = mm.makeString("container");
    mm.pushTempRoot(container);
    for (const HookCase& h : kHooks) {
        PollState poll;
        poll.marking = true;
        RecordingCollector c(poll);
        const Value oldv = Value{static_cast<Obj*>(container)};
        const Value newv = Value{Nil{}};
        (c.*h.hook)(container, oldv, newv);
        ASSERT_EQ(c.stores.size(), 1u) << h.name;
        EXPECT_EQ(c.stores[0].kind, h.kind) << h.name;
        EXPECT_EQ(c.stores[0].container, container) << h.name;
        EXPECT_EQ(as<Obj*>(c.stores[0].oldv), container) << h.name;
        EXPECT_FALSE(isObj(c.stores[0].newv)) << h.name;
    }
}

TEST(CollectorHooksTest, NoHookDoesAnythingWhenNoMarkRuns) {
    MemoryManager mm;
    ObjString* s = mm.makeString("s");
    mm.pushTempRoot(s);
    PollState poll; // marking is false
    RecordingCollector c(poll);
    for (const HookCase& h : kHooks) {
        (c.*h.hook)(s, Value{Nil{}}, Value{Nil{}});
    }
    c.internHit(s);
    EXPECT_TRUE(c.stores.empty());
    EXPECT_TRUE(c.interned.empty());
}

TEST(CollectorHooksTest, InternHitReportsTheStringWhileAMarkRuns) {
    MemoryManager mm;
    ObjString* s = mm.makeString("found");
    mm.pushTempRoot(s);
    PollState poll;
    poll.internBarrier = true;
    RecordingCollector c(poll);
    c.internHit(s);
    ASSERT_EQ(c.interned.size(), 1u);
    EXPECT_EQ(c.interned[0], s);
}

TEST(CollectorHooksTest, MarkValueMarksAnObjectAndIgnoresAScalar) {
    MemoryManager mm;
    ObjString* s = mm.makeString("obj");
    mm.pushTempRoot(s);
    PollState poll;
    RecordingCollector c(poll);
    c.markValue(Value{static_cast<Obj*>(s)});
    c.markValue(Value{Nil{}});
    ASSERT_EQ(c.marked.size(), 1u);
    EXPECT_EQ(c.marked[0], s);
}

TEST(CollectorHooksTest, TheHooksOfTheStopTheWorldCollectorChangeNothing) {
    MemoryManager mm;
    ObjString* s = mm.makeString("s");
    mm.pushTempRoot(s);
    const std::size_t bytes = mm.bytesAllocated;
    const std::size_t count = mm.collectionCount();
    Collector& c = mm.collector();
    for (const HookCase& h : kHooks) {
        (c.*h.hook)(s, Value{Nil{}}, Value{static_cast<Obj*>(s)});
    }
    c.internHit(s);
    EXPECT_EQ(mm.bytesAllocated, bytes);
    EXPECT_EQ(mm.collectionCount(), count);
}

TEST(CollectorHooksTest, TheStopTheWorldCollectorNeverSetsTheMarkFlag) {
    // The flags gate every barrier hook. A stop-the-world collector that set
    // one would make each hook do work that this collector does not need.
    MemoryManager mm;
    mm.swapCollector(makeCollectorByName("stw", mm, mm.pollState(), 0));
    EXPECT_FALSE(mm.pollState().marking);
    EXPECT_FALSE(mm.pollState().internBarrier);
    mm.collectGarbage();
    mm.collector().collectNow(CollectCause::Exit);
    EXPECT_FALSE(mm.pollState().marking);
    EXPECT_FALSE(mm.pollState().internBarrier);
    // Enough allocation to pass the threshold of the first collection.
    for (int i = 0; i < 6; ++i) {
        mm.makeString(std::string(300000, static_cast<char>('a' + i)));
    }
    EXPECT_GT(mm.collectionCount(), 0u);
    EXPECT_FALSE(mm.pollState().marking);
    EXPECT_FALSE(mm.pollState().internBarrier);
}

TEST(CollectorHooksTest, AFreshPollStateNeverPollsBeforeACollectorSetsIt) {
    PollState poll;
    EXPECT_FALSE(poll.stress);
    EXPECT_FALSE(poll.marking);
    EXPECT_EQ(poll.pollAt, std::numeric_limits<std::size_t>::max());
}

TEST(CollectorChoiceTest, TheStopTheWorldCollectorIsAKnownName) {
    MemoryManager mm;
    PollState poll;
    EXPECT_NE(makeCollectorByName("stw", mm, poll, 0), nullptr);
}

TEST(CollectorChoiceTest, TheIncrementalCollectorIsAKnownName) {
    MemoryManager mm;
    PollState poll;
    EXPECT_NE(makeCollectorByName("incremental", mm, poll, 0), nullptr);
}

TEST(CollectorChoiceTest, AnUnknownNameIsNotACollector) {
    MemoryManager mm;
    PollState poll;
    EXPECT_EQ(makeCollectorByName("Incremental", mm, poll, 0), nullptr);
    EXPECT_EQ(makeCollectorByName("", mm, poll, 0), nullptr);
}

TEST(CollectorHeapTest, ACollectionFreesAnObjectThatNothingReaches) {
    MemoryManager mm;
    ObjString* kept = mm.makeString("kept");
    mm.pushTempRoot(kept);
    mm.makeString("dropped"); // reachable from nothing
    const std::size_t before = mm.bytesAllocated;
    const std::size_t count = mm.collectionCount();
    mm.collectGarbage();
    EXPECT_EQ(mm.collectionCount(), count + 1);
    EXPECT_LT(mm.bytesAllocated, before);
    EXPECT_EQ(mm.findString("kept"), kept);
    EXPECT_EQ(mm.findString("dropped"), nullptr);
}

TEST(CollectorHeapTest, AnObjectThatTheHeapMadeIsRegisteredWithTheCollector) {
    // An object that onCreate did not register is never swept, so its bytes
    // stay counted after a collection that nothing reaches.
    MemoryManager mm;
    const std::size_t before = mm.bytesAllocated;
    for (int i = 0; i < 100; ++i) {
        mm.create<ObjList>(VmAllocator<Value>{&mm}); // dropped at once
    }
    EXPECT_GT(mm.bytesAllocated, before);
    mm.collectGarbage();
    EXPECT_EQ(mm.bytesAllocated, before);
}

TEST(CollectorHeapTest, EachLookupOfTheInternTableCallsTheHook) {
    // `other` is declared first: it must outlive `mm`, whose foreign set holds
    // its Strings.
    MemoryManager other;
    MemoryManager mm;
    // No cycle may be open when the recording collector comes in.
    mm.swapCollector(makeCollectorByName("stw", mm, mm.pollState(), 0));
    ObjString* shared = mm.makeString("shared");
    mm.pushTempRoot(shared);
    ObjString* theirs = other.makeString("shared"); // same text, other heap
    other.pushTempRoot(theirs);
    ObjString* fresh = other.makeString("only-there");
    other.pushTempRoot(fresh);
    LoxString text("shared", VmAllocator<char>{&mm});

    auto stw =
        mm.swapCollector(std::make_unique<RecordingCollector>(mm.pollState()));
    auto& rec = static_cast<RecordingCollector&>(mm.collector());

    // No mark runs: the lookups find Strings, and the hook stays silent.
    EXPECT_EQ(mm.makeString(std::string_view{"shared"}), shared);
    EXPECT_EQ(mm.findString("shared"), shared);
    EXPECT_TRUE(rec.interned.empty());

    mm.pollState().internBarrier = true;
    std::size_t hits = 0;
    auto expectOneMoreHit = [&](const char* site) {
        ++hits;
        EXPECT_EQ(rec.interned.size(), hits) << site;
    };

    EXPECT_EQ(mm.makeString(std::string_view{"shared"}), shared);
    expectOneMoreHit("makeString(string_view)");
    EXPECT_EQ(mm.makeString(std::string{"shared"}), shared);
    expectOneMoreHit("makeString(std::string&&)");
    EXPECT_EQ(mm.makeString(std::move(text)), shared);
    expectOneMoreHit("makeString(LoxString&&)");
    EXPECT_EQ(mm.findString("shared"), shared);
    expectOneMoreHit("findString, a hit");
    EXPECT_EQ(mm.findString("absent"), nullptr);
    EXPECT_EQ(rec.interned.size(), hits) << "findString, a miss";

    // A String from another heap with the text of one that this heap has.
    MemoryManager::holdValue(Value{static_cast<Obj*>(theirs)});
    EXPECT_EQ(as<Obj*>(mm.receiveMessage(Value{static_cast<Obj*>(theirs)})),
              static_cast<Obj*>(shared));
    expectOneMoreHit("receiveMessage, an equal String");
    // A String that this heap does not have: the first receive adds it to the
    // table (no hit), and the second one finds that same String.
    MemoryManager::holdValue(Value{static_cast<Obj*>(fresh)});
    mm.receiveMessage(Value{static_cast<Obj*>(fresh)});
    EXPECT_EQ(rec.interned.size(), hits) << "receiveMessage, a new String";
    MemoryManager::holdValue(Value{static_cast<Obj*>(fresh)});
    mm.receiveMessage(Value{static_cast<Obj*>(fresh)});
    expectOneMoreHit("receiveMessage, the same String again");

    mm.pollState().internBarrier = false;
    mm.swapCollector(std::move(stw));
}

TEST(CollectorHeapTest, AnExitCollectionCounts) {
    MemoryManager mm;
    const std::size_t count = mm.collectionCount();
    mm.collector().collectNow(CollectCause::Exit);
    EXPECT_EQ(mm.collectionCount(), count + 1);
}
