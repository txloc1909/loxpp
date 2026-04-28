#pragma once

#include <cstdint>
#include <vector>

// CoreHashMap<Entry, Policy, Alloc>
// Shared open-addressing hash map used by both Table and ObjMap.
//
// Policy must provide:
//   static bool isEmpty(const Entry&)
//   static bool isTombstone(const Entry&)
//   static void makeTombstone(Entry&)
//   static uint32_t hashOf(const Entry&)   // hash of the entry's key
//   static bool keyMatch(const Entry& slot, const Entry& needle)

template <typename Entry, typename Policy, typename Alloc>
class CoreHashMap {
    using Storage = std::vector<Entry, Alloc>;
    Storage m_buckets;
    int m_count{0};
    int m_dead{0};

    // Works for both Entry* and const Entry* by template deduction on E.
    template <typename E, typename KeyMatch>
    static E* findSlotIn(E* data, int cap, uint32_t hash, KeyMatch match) {
        uint32_t idx = hash % static_cast<uint32_t>(cap);
        E* tombstone = nullptr;
        for (;;) {
            E* e = &data[idx];
            if (Policy::isEmpty(*e)) {
                return tombstone != nullptr ? tombstone : e;
            }
            if (Policy::isTombstone(*e)) {
                if (tombstone == nullptr) {
                    tombstone = e;
                }
            } else if (match(*e)) {
                return e;
            }
            idx = (idx + 1) % static_cast<uint32_t>(cap);
        }
    }

    void grow() {
        int cap = static_cast<int>(m_buckets.size());
        int newCap = cap < 8 ? 8 : cap * 2;
        Storage fresh(newCap, Entry{}, m_buckets.get_allocator());
        int saved = 0;
        for (const Entry& e : m_buckets) {
            if (Policy::isEmpty(e) || Policy::isTombstone(e)) {
                continue;
            }
            Entry* dest = findSlotIn(
                fresh.data(), newCap, Policy::hashOf(e),
                [&](const Entry& s) { return Policy::keyMatch(s, e); });
            *dest = e;
            ++saved;
        }
        m_count = saved;
        m_dead = 0;
        m_buckets = std::move(fresh);
    }

  public:
    static constexpr double MAX_LOAD = 0.75;

    explicit CoreHashMap(Alloc alloc) : m_buckets(std::move(alloc)) {}

    // Returns a pointer to the live matching entry, or nullptr if not found.
    template <typename KeyMatch>
    [[nodiscard]] const Entry* find(uint32_t hash, KeyMatch match) const {
        if (m_buckets.empty()) {
            return nullptr;
        }
        int cap = static_cast<int>(m_buckets.size());
        const Entry* slot = findSlotIn(m_buckets.data(), cap, hash, match);
        if (Policy::isEmpty(*slot) || Policy::isTombstone(*slot)) {
            return nullptr;
        }
        return slot;
    }

    // Insert or update entry e. Returns true if e's key was newly inserted.
    // May grow (and re-allocate via Alloc) when the load factor is exceeded.
    bool set(const Entry& e) {
        int cap = static_cast<int>(m_buckets.size());
        if (m_count + m_dead + 1 > static_cast<int>(cap * MAX_LOAD)) {
            grow();
            cap = static_cast<int>(m_buckets.size());
        }
        Entry* slot =
            findSlotIn(m_buckets.data(), cap, Policy::hashOf(e),
                       [&](const Entry& s) { return Policy::keyMatch(s, e); });
        const bool wasEmpty = Policy::isEmpty(*slot);
        const bool wasTombstone = !wasEmpty && Policy::isTombstone(*slot);
        if (wasEmpty || wasTombstone) {
            ++m_count;
        }
        if (wasTombstone) {
            --m_dead;
        }
        *slot = e;
        return wasEmpty || wasTombstone;
    }

    // Mark the entry matching hash+match as a tombstone. Returns true if found.
    template <typename KeyMatch>
    bool remove(uint32_t hash, KeyMatch match) {
        if (m_count == 0 || m_buckets.empty()) {
            return false;
        }
        int cap = static_cast<int>(m_buckets.size());
        Entry* slot = findSlotIn(m_buckets.data(), cap, hash, match);
        if (Policy::isEmpty(*slot) || Policy::isTombstone(*slot)) {
            return false;
        }
        Policy::makeTombstone(*slot);
        --m_count;
        ++m_dead;
        return true;
    }

    // Iterate live entries.
    template <typename F>
    void forEach(F&& fn) const {
        for (const Entry& e : m_buckets) {
            if (!Policy::isEmpty(e) && !Policy::isTombstone(e)) {
                fn(e);
            }
        }
    }

    // Mark live entries matching shouldRemove as tombstones.
    template <typename Pred>
    void removeIf(Pred shouldRemove) {
        for (Entry& e : m_buckets) {
            if (!Policy::isEmpty(e) && !Policy::isTombstone(e) &&
                shouldRemove(e)) {
                Policy::makeTombstone(e);
                --m_count;
                ++m_dead;
            }
        }
    }

    [[nodiscard]] int count() const { return m_count; }
    [[nodiscard]] int capacity() const {
        return static_cast<int>(m_buckets.size());
    }
    [[nodiscard]] const Entry* entryAt(int i) const { return &m_buckets[i]; }
};
