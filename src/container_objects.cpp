#include "container_objects.h"
#include "value.h"

#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

// Resource destructors. GC timing is non-deterministic, so these are a
// safety net only: a program must close() a Socket/Server and wait() a
// Process for deterministic cleanup. A child still running when its process
// object is collected is not killed; it is reaped only if already exited.
ObjSocket::~ObjSocket() {
    if (handle != nullptr) {
        std::fclose(handle); // closes fd too
        handle = nullptr;
        fd = -1;
    } else if (fd >= 0) {
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
        map.find(hash, [&key](const MapEntry& s) { return s.key == key; });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSet(const Value& key, const Value& value) {
    // map.set may grow (re-allocate via VmAllocator, which may trigger GC).
    // Caller must ensure this ObjMap is rooted before calling mapSet.
    bool inserted =
        map.set(MapEntry{key, value, hashValue(key), MapSlot::OCCUPIED});
    // Count only a new key: an overwrite leaves iteration valid, while a
    // paired erase plus insert must still trip the iterator check.
    if (inserted) {
        ++version;
    }
    return inserted;
}

bool ObjMap::mapDel(const Value& key) {
    bool removed = map.remove(
        hashValue(key), [&key](const MapEntry& s) { return s.key == key; });
    // Count only a real erase: a miss leaves iteration valid.
    if (removed) {
        ++version;
    }
    return removed;
}

bool ObjMap::mapGetHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                          Value& out) const {
    const MapEntry* e =
        map.find(hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (!e) {
        return false;
    }
    out = e->value;
    return true;
}

bool ObjMap::mapSetHashed(const Value& key, const Value& value, uint32_t hash,
                          const KeyEq& eq, bool& eqFailed) {
    // Find first: an __eq__ that fails must not be followed by an insert.
    MapEntry* existing = map.findMutable(
        hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (eqFailed) {
        return false;
    }
    if (existing != nullptr) {
        *existing = MapEntry{key, value, hash, MapSlot::OCCUPIED};
        return false; // overwrite
    }
    // No equal key exists, so an identity match cannot miss it. No user code
    // runs here, so the locked map cannot change between the find and the set.
    map.set(MapEntry{key, value, hash, MapSlot::OCCUPIED});
    ++version;
    return true;
}

bool ObjMap::mapDelHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                          bool& eqFailed) {
    MapEntry* existing = map.findMutable(
        hash, [&](const MapEntry& s) { return eq(s.key, key); });
    if (eqFailed || existing == nullptr) {
        return false;
    }
    map.removeAt(existing);
    ++version;
    return true;
}
