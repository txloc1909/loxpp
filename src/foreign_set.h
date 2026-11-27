#pragma once

#include "object.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// The foreign set of a heap: the objects of other heaps that this heap holds
// a count on, each with a side mark. A collector that must bound its steps
// cannot use a container whose work follows its size, so each operation here
// does a bounded amount of work:
//
//  - The side mark is an epoch. beginCycle makes every entry unmarked by one
//    increment. An entry is marked when its epoch is the current one.
//  - The hash table grows by incremental rehash: a table of twice the size is
//    made, and each insert moves a few buckets. A lookup reads both tables.
//    The new table comes from calloc, so a large one is zero pages that the
//    kernel maps when a bucket is first used, and no insert writes the whole
//    table.
//  - The entries are also on one doubly linked list. A scan of the entries
//    that drop the unmarked ones runs in parts of any size: scanStep visits
//    at most the number of entries that the caller gives.
//
// A scan visits the entries that were on the list when it began. An entry
// that a scan finds with the current epoch stays, so an entry that the
// program marks or adopts while the scan runs is never dropped by it.
class ForeignSet {
  public:
    ForeignSet() = default;
    ForeignSet(const ForeignSet&) = delete;
    ForeignSet& operator=(const ForeignSet&) = delete;
    ~ForeignSet() { clear(); }

    [[nodiscard]] std::size_t size() const { return m_size; }

    // The work that one touch spends on a rehash: it moves at most
    // kRehashBuckets chains, and it passes at most kRehashEmpty empty buckets.
    static constexpr int kRehashBuckets = 4;
    static constexpr int kRehashEmpty = 32;
    // The next bucket of the old table to move, or -1 when no rehash runs.
    [[nodiscard]] long rehashIndex() const { return m_rehashIdx; }

    // Every entry is unmarked.
    void beginCycle() { ++m_epoch; }

    [[nodiscard]] bool marked(const Obj* obj) const {
        const Node* n = find(const_cast<Obj*>(obj));
        return n != nullptr && n->epoch == m_epoch;
    }

    // Marks the entry of `obj`, or adds it, marked. True when it was added.
    bool touch(Obj* obj) {
        if (m_rehashIdx >= 0) {
            rehashStep();
        }
        if (Node* n = find(obj)) {
            n->epoch = m_epoch;
            return false;
        }
        if (m_rehashIdx < 0 && m_size >= m_cap[0]) {
            grow();
        }
        auto* n = new Node{obj, m_epoch, nullptr, nullptr, nullptr};
        const int t = m_rehashIdx >= 0 ? 1 : 0;
        Node** slot = &m_table[t][bucket(obj, m_cap[t])];
        n->chain = *slot;
        *slot = n;
        n->next = m_head;
        if (m_head != nullptr) {
            m_head->prev = n;
        }
        m_head = n;
        ++m_size;
        return true;
    }

    // Starts a scan of the entries that exist now.
    void beginScan() { m_scan = m_head; }
    void endScan() { m_scan = nullptr; }
    [[nodiscard]] bool scanning() const { return m_scan != nullptr; }

    // Visits at most `maxEntries` entries of the scan. Each entry that is not
    // marked leaves the set, and `drop(obj)` is called after that. Returns
    // the number of entries visited.
    template <class F>
    std::size_t scanStep(std::size_t maxEntries, F&& drop) {
        std::size_t visited = 0;
        while (m_scan != nullptr && visited < maxEntries) {
            Node* n = m_scan;
            m_scan = n->next;
            ++visited;
            if (n->epoch == m_epoch) {
                continue;
            }
            Obj* obj = n->key;
            erase(n);
            drop(obj);
        }
        return visited;
    }

    // Calls `f(obj)` for every entry. The set must not change meanwhile.
    template <class F>
    void forEach(F&& f) const {
        for (const Node* n = m_head; n != nullptr; n = n->next) {
            f(n->key);
        }
    }

    void clear() {
        for (Node* n = m_head; n != nullptr;) {
            Node* next = n->next;
            delete n;
            n = next;
        }
        m_head = nullptr;
        m_scan = nullptr;
        std::free(m_table[0]);
        std::free(m_table[1]);
        m_table[0] = m_table[1] = nullptr;
        m_cap[0] = m_cap[1] = 0;
        m_rehashIdx = -1;
        m_size = 0;
    }

  private:
    struct Node {
        Obj* key;
        std::uint64_t epoch;
        Node* chain; // the next entry of the bucket
        Node* next;  // the list of all entries
        Node* prev;
    };

    static std::size_t bucket(const Obj* obj, std::size_t cap) {
        std::uint64_t h = (reinterpret_cast<std::uintptr_t>(obj) >> 4) *
                          0x9E3779B97F4A7C15ULL;
        h ^= h >> 32;
        return static_cast<std::size_t>(h) & (cap - 1);
    }

    Node* find(Obj* obj) const {
        for (int t = 0; t < 2; ++t) {
            if (m_cap[t] == 0) {
                continue;
            }
            for (Node* n = m_table[t][bucket(obj, m_cap[t])]; n != nullptr;
                 n = n->chain) {
                if (n->key == obj) {
                    return n;
                }
            }
        }
        return nullptr;
    }

    void grow() {
        if (m_cap[0] == 0) {
            m_cap[0] = 16;
            m_table[0] = allocTable(16);
            return;
        }
        const std::size_t cap = m_cap[0] * 2;
        m_table[1] = allocTable(cap);
        m_cap[1] = cap;
        m_rehashIdx = 0;
    }

    static Node** allocTable(std::size_t cap) {
        void* p = std::calloc(cap, sizeof(Node*));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<Node**>(p);
    }

    // Moves a few buckets of the old table. The old table has at most
    // kRehashBuckets chains moved and kRehashEmpty empty buckets passed.
    void rehashStep() {
        int moves = kRehashBuckets;
        int empties = kRehashEmpty;
        while (moves > 0 && empties > 0) {
            if (static_cast<std::size_t>(m_rehashIdx) >= m_cap[0]) {
                std::free(m_table[0]);
                m_table[0] = m_table[1];
                m_cap[0] = m_cap[1];
                m_table[1] = nullptr;
                m_cap[1] = 0;
                m_rehashIdx = -1;
                return;
            }
            Node* n = m_table[0][m_rehashIdx];
            ++m_rehashIdx;
            if (n == nullptr) {
                --empties;
                continue;
            }
            --moves;
            m_table[0][m_rehashIdx - 1] = nullptr;
            while (n != nullptr) {
                Node* chain = n->chain;
                Node** slot = &m_table[1][bucket(n->key, m_cap[1])];
                n->chain = *slot;
                *slot = n;
                n = chain;
            }
        }
    }

    void erase(Node* n) {
        for (int t = 0; t < 2; ++t) {
            if (m_cap[t] == 0) {
                continue;
            }
            Node** slot = &m_table[t][bucket(n->key, m_cap[t])];
            while (*slot != nullptr && *slot != n) {
                slot = &(*slot)->chain;
            }
            if (*slot == n) {
                *slot = n->chain;
                break;
            }
        }
        if (n->prev != nullptr) {
            n->prev->next = n->next;
        } else {
            m_head = n->next;
        }
        if (n->next != nullptr) {
            n->next->prev = n->prev;
        }
        delete n;
        --m_size;
    }

    Node** m_table[2]{nullptr, nullptr};
    std::size_t m_cap[2]{0, 0};
    long m_rehashIdx{-1};
    std::size_t m_size{0};
    std::uint64_t m_epoch{1};
    Node* m_head{nullptr};
    Node* m_scan{nullptr};
};
