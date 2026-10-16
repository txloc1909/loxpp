# Message copy cost between two heaps, 2026-10-17

The measurement behind the heap axis of
`docs/decisions/concurrency-model-decision.md`: what a `send` costs when it
deep-copies a value from one worker's heap into another's. No send exists,
so `benchmarks/copy_bench.cpp` does what a native send would do, against the
real allocation path, the real string interning, and the real collector,
linked from `libloxrt.a`. It answers three questions: the cost per item by
shape, whether strings must be copied, and whether the allocator is the
bottleneck.

## Setup

| Item | Value |
|---|---|
| Commit | `4b609d2`, release `libloxrt.a`; harness built with clang 18 `-O3` in `loxpp-dev-env` |
| Machine | Intel Core 7 240H; Linux 7.2.5, Fedora 44; AC on, `balance_performance` |
| Pinning | core 2, a P-core, with `taskset` |
| Protocol | each copy goes into a fresh destination heap; 7 repetitions, median; `benchmarks/copy_cost.sh` |
| Copy | recursive, with an identity memo so a cycle arrives as a cycle; each new object is a temp root until its parent holds it |

The timed copy includes the collections the destination heap runs on its
own threshold while the copy allocates, as a real receiver would.

## Cost per item by shape

| Payload, per item | N = 100,000 | N = 1,000,000 |
|---|---|---|
| `memcpy` of 8 bytes, the floor | 0.3 ns | 0.4 ns |
| a number in a list | 2.2 ns | 2.4 ns |
| a distinct string, copied and re-interned | 128 to 134 ns | 205 ns |
| a distinct string, shared by reference | 4.8 to 5.0 ns | 5.6 ns |
| a record: a map with 5 entries, keys shared | 186 to 193 ns | 245 ns |
| a record, keys copied | 182 to 186 ns | 250 ns |
| a small list of 4 numbers | 95 to 104 ns | 120 ns |
| the identity memo, on a list of numbers | 0 ns | 0 ns |

Two runs at 100,000 agree within 6 percent; the ranges are both runs.

Read it as a cost per allocated object plus a cost per map entry: about 100
ns for an object with its storage, about 20 to 30 ns per map entry for the
hash and the probe, and 2 ns per scalar. A string costs an object plus a
hash and an intern-table insert; shared by reference it costs a pointer.

Past the cache, at a million items, every object-bearing shape rises 25 to
60 percent and a copied string rises 60 percent, because the intern table
and the heap both outgrow it.

## The receiver's collector

| Collection on the copied records | N = 100,000 | N = 1,000,000 |
|---|---|---|
| mark and sweep, all live | 42 to 45 ns per record | 47 ns |
| sweep, all dead | 43 ns per record | 118 ns |

The receiver pays about 45 ns per live record on every collection while the
message lives, and 43 to 118 ns per record to free it. A message that stays
for several collections costs more to keep than to copy.

## Does the allocator matter

`ARENA` rows run the same copy with a bump allocator behind `VmAllocator`,
which is where list elements, map buckets, and string characters live.
Object headers still go through `new`.

| Shape | malloc | arena |
|---|---|---|
| numbers | 2.2 ns | 4.1 to 4.5 ns |
| records, keys shared | 190 ns | 219 to 224 ns |
| nested lists | 95 to 104 ns | 89 to 92 ns |

No gain, within noise. The container storage is not where the time goes.
The cost of a record is one `new` for the map object, one to three bucket
arrays as the map grows from empty, and five hash-and-probe inserts. Of
those, a faster allocator reaches only the `new` and the bucket arrays,
about 25 ns of 190.

What would move the number is not the allocator but the copy itself:

- **Share strings.** 27 times cheaper than a copy, and they are immutable
  and interned already. A string that crosses a worker boundary needs a
  lifetime the sender's collector respects: a cross-worker count on the
  object, or a shared intern table. That is runtime design, not allocator
  design.
- **Clone a map's bucket array** instead of re-inserting entry by entry.
  The hashes are stored per entry and the keys are shared strings or
  scalars, so a map copy becomes one allocation and one `memcpy` with a
  pointer fix-up pass: an estimated 50 ns per record in place of 190. This
  needs one method on `CoreHashMap`.
- **Transfer a region instead of copying it.** With a region-aware heap,
  a message built in its own region moves to the receiver by relinking the
  region, and the send costs nothing per item. The sendable set already
  guarantees what a region needs: no pointer leaves the message except to
  a shared string. This is the allocator change that changes the game, and
  it is the collector's shape, not a drop-in allocator; it belongs to the
  GC item that follows the model.

## What it means for the decision

- A send of a small control message, a number or a short map, costs less
  than the 60 ns switch it travels with. The model is comfortable here.
- A send of medium records costs about 200 ns each. A worker can move about
  five million records per second between heaps, and the receiver pays
  again at each collection while they live. That is the regime share-nothing
  runtimes live in.
- A bulk payload, hundreds of thousands of records per send, costs tens of
  milliseconds per hop and the same again to drop. The decision serves this
  regime badly by construction, and the three items above are the ladder
  out of it: strings first, bucket clone second, regions last.
- Inside one worker the same copy is paid between tasks. The record gives
  the number that judgment call 2 of the decision must be weighed against.

## Not measured

- A send between threads. The copy was measured alone; a mailbox adds a
  lock or a queue per message on top.
- The cross-worker lifetime of a shared string.
- A region transfer. Nothing exists to measure.
