#include "container_objects.h"
#include "value.h"

#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

// Resource destructors. GC timing is non-deterministic, so these are a
// safety net only: a program must close() a Socket/Server and wait() a
// Process for deterministic cleanup. A child still running when its process
// object is collected is not killed; it is reaped only if already exited.
// A descriptor has a registered wait only while a task waits on it, and
// that task keeps the object alive through WaitingCall::target. Every wait ends
// before the task does (releaseWait), and a close cancels its waits first. So
// no wait exists when a destructor runs, and a closed number cannot meet a
// stale one.
ObjSocket::~ObjSocket() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

ObjServer::~ObjServer() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

ObjProcess::~ObjProcess() {
    if (in != nullptr) {
        std::fclose(in);
        in = nullptr;
    }
    if (out != nullptr) {
        std::fclose(out);
        out = nullptr;
    }
    if (err != nullptr) {
        std::fclose(err);
        err = nullptr;
    }
    if (!reaped && pid > 0) {
        int st = 0;
        pid_t r = ::waitpid(static_cast<pid_t>(pid), &st, WNOHANG);
        if (r == static_cast<pid_t>(pid)) {
            reaped = true;
            status = WIFEXITED(st) ? WEXITSTATUS(st)
                                   : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
        }
    }
}

// ---------------------------------------------------------------------------
// ObjMap hash table implementation (delegates to CoreHashMap)
// ---------------------------------------------------------------------------

uint32_t MapPolicy::hashOf(const MapEntry& e) { return e.hash; }

// Scalar/string convenience path: identity equality, hash from hashValue().
// An Instance key must go through mapGetHashed/mapSetHashed/mapDelHashed so
// its class's __hash__/__eq__ run.
bool ObjMap::mapGet(const Value& key, Value& out) const {
    uint32_t hash = hashValue(key);
    const MapEntry* e =
        m_map.find(hash, [&key](const MapEntry& s) { return s.key == key; });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

void ObjMap::barrierRemoved(Collector* c, const MapEntry& old) {
    c->barrierEntry(this, old.key, barrier::nil());
    c->barrierEntry(this, old.value, barrier::nil());
}

bool ObjMap::mapSet(const Value& key, const Value& value) {
    const uint32_t hash = hashValue(key);
    // The growth can collect (VmAllocator), so the caller roots this ObjMap
    // before it calls mapSet. The barrier reads the old entry after it.
    m_map.ensureRoom();
    Collector* c = collector();
    if (barrier::active(c)) {
        const MapEntry* old = m_map.find(
            hash, [&key](const MapEntry& s) { return s.key == key; });
        if (old != nullptr && old->key.bits != key.bits) {
            // An equal key can be another object (an Int of the heap, a
            // String of another heap), and the entry keeps the new one.
            c->barrierEntry(this, old->key, key);
        }
        c->barrierEntry(this, old != nullptr ? old->value : barrier::nil(),
                        value);
    }
    bool inserted = m_map.set(MapEntry{key, value, hash, MapSlot::OCCUPIED});
    // Count only a new key: an overwrite leaves iteration valid, while a
    // paired erase plus insert must still trip the iterator check.
    if (inserted) {
        ++version;
    }
    return inserted;
}

bool ObjMap::mapDel(const Value& key) {
    const uint32_t hash = hashValue(key);
    auto match = [&key](const MapEntry& s) { return s.key == key; };
    Collector* c = collector();
    if (barrier::active(c)) {
        if (const MapEntry* old = m_map.find(hash, match)) {
            barrierRemoved(c, *old);
        }
    }
    bool removed = m_map.remove(hash, match);
    // Count only a real erase: a miss leaves iteration valid.
    if (removed) {
        ++version;
    }
    return removed;
}

bool ObjMap::mapGetHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                          Value& out) const {
    const MapEntry* e =
        m_map.find(hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSetHashed(const Value& key, const Value& value, uint32_t hash,
                          const KeyEq& eq, bool& eqFailed) {
    // Find first: an __eq__ that fails must not be followed by an insert.
    MapEntry* existing = m_map.findMutable(
        hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (eqFailed) {
        return false;
    }
    Collector* c = collector();
    if (existing != nullptr) {
        if (barrier::active(c)) {
            c->barrierEntry(this, existing->key, key);
            c->barrierEntry(this, existing->value, value);
        }
        *existing = MapEntry{key, value, hash, MapSlot::OCCUPIED};
        return false; // overwrite
    }
    // No equal key exists, so an identity match cannot miss it. No user code
    // runs here, so the locked map cannot change between the find and the set.
    m_map.ensureRoom();
    if (barrier::active(c)) {
        c->barrierEntry(this, barrier::nil(), value);
    }
    m_map.set(MapEntry{key, value, hash, MapSlot::OCCUPIED});
    ++version;
    return true;
}

bool ObjMap::mapDelHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                          bool& eqFailed) {
    MapEntry* existing = m_map.findMutable(
        hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (eqFailed || existing == nullptr) {
        return false;
    }
    Collector* c = collector();
    if (barrier::active(c)) {
        barrierRemoved(c, *existing);
    }
    m_map.removeAt(existing);
    ++version;
    return true;
}
