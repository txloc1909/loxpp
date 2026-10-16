// The cost of a message copy between two heaps, for the concurrency
// decision (docs/decisions/concurrency-model-decision.md, "The message and
// value story"). No send exists yet, so this harness does what a native
// `send` would do: deep-copy a value graph from one MemoryManager into
// another, with an identity memo so a cycle arrives as a cycle, and times
// it per element. It links the runtime library, so the allocation path, the
// string interning, and the collector are the real ones.
//
// Built and run by benchmarks/copy_cost.sh. Prints one line per case:
//   <case> <N> <ns per element, median of R> <min> <max>

#include "container_objects.h"
#include "memory_manager.h"

#include <algorithm>
#include <chrono>
#include <new>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <vector>

namespace {
bool inArena(void* p);
}
void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}
void operator delete(void* p) noexcept {
    if (p != nullptr && !inArena(p)) {
        std::free(p);
    }
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

namespace {

using Clock = std::chrono::steady_clock;

// Marks a vector of roots; both heaps use one of these.
struct Roots {
    MemoryManager* mm;
    std::vector<Obj*> objs;
    explicit Roots(MemoryManager* m) : mm(m) {
        mm->setMarkRootsCallback([this] {
            for (Obj* o : objs) {
                mm->markObject(o);
            }
        });
    }
};

// A bump allocator for container storage (list elements, map buckets, string
// characters) so that the same copy can run against an arena instead of
// malloc. Obj headers still go through `new` in MemoryManager::create; the
// arena covers only what VmAllocator reaches.
//
// VmAllocBase::rawFree is not virtual and calls `operator delete` on every
// pointer it is given, so this file replaces the global `operator delete`
// with one that ignores a pointer inside an arena block. The blocks are
// never freed; this is a benchmark, and the leak is bounded by the run.
struct ArenaBlock {
    char* begin;
    char* end;
};
std::vector<ArenaBlock>& arenaBlocks() {
    static std::vector<ArenaBlock> blocks;
    return blocks;
}
bool inArena(void* p) {
    for (const ArenaBlock& b : arenaBlocks()) {
        if (p >= b.begin && p < b.end) {
            return true;
        }
    }
    return false;
}

struct ArenaMM : MemoryManager {
    char* cur{nullptr};
    std::size_t left{0};
    static constexpr std::size_t kBlock = 8u << 20;
    void* rawAlloc(std::size_t bytes) override {
        bytesAllocated += bytes;
        bytes = (bytes + 15) & ~std::size_t{15};
        if (bytes > left) {
            std::size_t sz = std::max(bytes, kBlock);
            cur = static_cast<char*>(std::malloc(sz));
            arenaBlocks().push_back({cur, cur + sz});
            left = sz;
        }
        void* p = cur;
        cur += bytes;
        left -= bytes;
        return p;
    }
};

struct Copier {
    MemoryManager& dst;
    bool shareStrings;
    bool useMemo;
    std::unordered_map<Obj*, Obj*> memo;

    Value copy(const Value& v) {
        if (!isObj(v)) {
            return v;
        }
        Obj* o = asObj(v);
        if (useMemo) {
            auto it = memo.find(o);
            if (it != memo.end()) {
                return from<Obj*>(it->second);
            }
        }
        switch (o->type) {
        case ObjType::STRING: {
            if (shareStrings) {
                return v;
            }
            auto* s = asObjString(o);
            ObjString* c = dst.makeString(
                std::string_view{s->chars.data(), s->chars.size()});
            if (useMemo) {
                memo.emplace(o, c);
            }
            return from<Obj*>(c);
        }
        case ObjType::LIST: {
            auto* l = static_cast<ObjList*>(o);
            auto* c = dst.create<ObjList>(VmAllocator<Value>{&dst});
            dst.pushTempRoot(c);
            if (useMemo) {
                memo.emplace(o, c);
            }
            c->elements.reserve(l->elements.size());
            for (const Value& e : l->elements) {
                c->elements.push_back(copy(e));
            }
            dst.popTempRoot();
            return from<Obj*>(c);
        }
        case ObjType::MAP: {
            auto* m = static_cast<ObjMap*>(o);
            auto* c = dst.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&dst});
            dst.pushTempRoot(c);
            if (useMemo) {
                memo.emplace(o, c);
            }
            m->map.forEach([&](const MapEntry& e) {
                Value k = copy(e.key);
                Value val = copy(e.value);
                c->mapSet(k, val);
            });
            dst.popTempRoot();
            return from<Obj*>(c);
        }
        default:
            std::fprintf(stderr, "not sendable: type %d\n",
                         static_cast<int>(o->type));
            std::exit(2);
        }
    }
};

ObjList* newList(MemoryManager& mm) {
    return mm.create<ObjList>(VmAllocator<Value>{&mm});
}

// Builders for the source heap. Each returns the root list of N items.
ObjList* numbers(MemoryManager& mm, Roots& r, int n) {
    ObjList* l = newList(mm);
    r.objs.push_back(l);
    for (int i = 0; i < n; i++) {
        l->elements.push_back(from<Number>(static_cast<double>(i)));
    }
    return l;
}
ObjList* strings(MemoryManager& mm, Roots& r, int n) {
    ObjList* l = newList(mm);
    r.objs.push_back(l);
    for (int i = 0; i < n; i++) {
        l->elements.push_back(
            from<Obj*>(mm.makeString("item-" + std::to_string(i))));
    }
    return l;
}
ObjList* records(MemoryManager& mm, Roots& r, int n) {
    ObjList* l = newList(mm);
    r.objs.push_back(l);
    const char* keys[5] = {"id", "name", "x", "y", "tag"};
    for (int i = 0; i < n; i++) {
        auto* m = mm.create<ObjMap>(nullptr, VmAllocator<MapEntry>{&mm});
        l->elements.push_back(from<Obj*>(m));
        for (int k = 0; k < 5; k++) {
            m->mapSet(from<Obj*>(mm.makeString(keys[k])),
                      from<Number>(static_cast<double>(i * 5 + k)));
        }
    }
    return l;
}
ObjList* nested(MemoryManager& mm, Roots& r, int n) {
    ObjList* l = newList(mm);
    r.objs.push_back(l);
    for (int i = 0; i < n; i++) {
        ObjList* inner = newList(mm);
        l->elements.push_back(from<Obj*>(inner));
        for (int k = 0; k < 4; k++) {
            inner->elements.push_back(from<Number>(static_cast<double>(k)));
        }
    }
    return l;
}

// Times R copies of `root` into a fresh destination heap each time and
// prints the median ns per item.
void run(const char* name, ObjList* root, int n, int reps, bool share,
         bool memo, bool arena) {
    std::vector<double> per;
    for (int r = 0; r < reps; r++) {
        MemoryManager* dst = arena ? new ArenaMM : new MemoryManager;
        Roots droots(dst);
        Copier c{*dst, share, memo, {}};
        auto t0 = Clock::now();
        Value out = c.copy(from<Obj*>(root));
        auto t1 = Clock::now();
        droots.objs.push_back(asObj(out));
        per.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        delete dst;
    }
    std::sort(per.begin(), per.end());
    std::printf("%-34s N=%d  %8.1f ns/item  (min %.1f, max %.1f)\n", name, n,
                per[per.size() / 2], per.front(), per.back());
}

} // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 100000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;

    MemoryManager src;
    Roots sroots(&src);
    ObjList* nums = numbers(src, sroots, n);
    ObjList* strs = strings(src, sroots, n);
    ObjList* recs = records(src, sroots, n);
    ObjList* nest = nested(src, sroots, n);

    // The floor: the same bytes as N Values, moved with memcpy.
    {
        std::vector<double> per;
        std::vector<Value> a(static_cast<std::size_t>(n), from<Number>(1.0));
        std::vector<Value> b(static_cast<std::size_t>(n), from<Number>(0.0));
        for (int r = 0; r < reps; r++) {
            auto t0 = Clock::now();
            std::memcpy(b.data(), a.data(), a.size() * sizeof(Value));
            auto t1 = Clock::now();
            per.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        }
        std::sort(per.begin(), per.end());
        std::printf("%-34s N=%d  %8.1f ns/item  (min %.1f, max %.1f)\n",
                    "memcpy floor, 8 bytes per item", n, per[per.size() / 2],
                    per.front(), per.back());
    }

    run("numbers, memo", nums, n, reps, false, true, false);
    run("numbers, no memo", nums, n, reps, false, false, false);
    run("strings, copied and re-interned", strs, n, reps, false, true, false);
    run("strings, shared by reference", strs, n, reps, true, true, false);
    run("records (5-entry maps), keys copied", recs, n, reps, false, true, false);
    run("records (5-entry maps), keys shared", recs, n, reps, true, true, false);
    run("nested (4-number lists)", nest, n, reps, false, true, false);
    run("ARENA numbers, memo", nums, n, reps, false, true, true);
    run("ARENA records, keys shared", recs, n, reps, true, true, true);
    run("ARENA nested", nest, n, reps, false, true, true);

    // The receiving side's collector: a full collection with the copied
    // records live, then one more after they are dropped.
    {
        MemoryManager dst;
        Roots droots(&dst);
        Copier c{dst, true, true, {}};
        Value out = c.copy(from<Obj*>(recs));
        droots.objs.push_back(asObj(out));
        auto t0 = Clock::now();
        dst.collectGarbage();
        auto t1 = Clock::now();
        droots.objs.clear();
        auto t2 = Clock::now();
        dst.collectGarbage();
        auto t3 = Clock::now();
        std::printf("%-34s N=%d  %8.1f ns/item  (mark+sweep, all live)\n",
                    "receiver GC, records live", n,
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        std::printf("%-34s N=%d  %8.1f ns/item  (sweep, all dead)\n",
                    "receiver GC, records dropped", n,
                    std::chrono::duration<double, std::nano>(t3 - t2).count() / n);
    }

    // A cycle must arrive as a cycle.
    {
        ObjList* cyc = newList(src);
        sroots.objs.push_back(cyc);
        cyc->elements.push_back(from<Obj*>(cyc));
        MemoryManager dst;
        Roots droots(&dst);
        Copier c{dst, true, true, {}};
        Value out = c.copy(from<Obj*>(cyc));
        auto* l = static_cast<ObjList*>(asObj(out));
        std::printf("cycle preserved: %s\n",
                    asObj(l->elements[0]) == l ? "yes" : "NO");
    }
    return 0;
}
