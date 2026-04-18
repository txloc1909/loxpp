#pragma once

#ifdef LOXPP_PROFILE

#include "chunk.h"
#include "function.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Data store
// ---------------------------------------------------------------------------

struct OpcodeStats {
    uint64_t count{0};
};

struct FunctionStats {
    std::string name;
    uint64_t callCount{0};
    int64_t totalNs{0}; // inclusive time (including callees)
    int64_t selfNs{0};  // exclusive time (totalNs - time in direct callees)
};

struct GcEventRecord {
    std::size_t beforeBytes;
    std::size_t afterBytes;
    int64_t pauseNs;
};

struct GcStats {
    uint64_t gcCount{0};
    int64_t totalPauseNs{0};
    int64_t maxPauseNs{0};
    std::size_t totalBytesFreed{0};
    std::size_t peakBytesAllocated{0};
    std::vector<GcEventRecord> events; // capped at 1024 entries
};

struct ProfilerData {
    // Per-opcode dispatch counts. Indexed by static_cast<uint8_t>(Op).
    // Array size 64 is larger than the current opcode count (42) for headroom.
    std::array<OpcodeStats, 64> opcodeTable{};

    // Per-function stats. Key is ObjFunction* (stable for program lifetime).
    std::unordered_map<ObjFunction*, FunctionStats> funcTable;

    // Per-frame call-entry timestamps, parallel to VM::m_frames[].
    // Size matches VM::FRAMES_MAX = 64.
    std::array<int64_t, 64> frameEnterNs{};

    GcStats gc{};
    int64_t programStartNs{0};
    int64_t programEndNs{0};

    int64_t nowNs() const noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return ts.tv_sec * int64_t{1'000'000'000} + ts.tv_nsec;
    }

    void report(std::FILE* out) const;
};

// ---------------------------------------------------------------------------
// Opcode name table (used by report())
// ---------------------------------------------------------------------------

inline const char* opcodeName(Op op) {
    switch (op) {
    case Op::CONSTANT:
        return "CONSTANT";
    case Op::NIL:
        return "NIL";
    case Op::TRUE:
        return "TRUE";
    case Op::FALSE:
        return "FALSE";
    case Op::EQUAL:
        return "EQUAL";
    case Op::GREATER:
        return "GREATER";
    case Op::LESS:
        return "LESS";
    case Op::NEGATE:
        return "NEGATE";
    case Op::ADD:
        return "ADD";
    case Op::SUBTRACT:
        return "SUBTRACT";
    case Op::MULTIPLY:
        return "MULTIPLY";
    case Op::DIVIDE:
        return "DIVIDE";
    case Op::MODULO:
        return "MODULO";
    case Op::NOT:
        return "NOT";
    case Op::PRINT:
        return "PRINT";
    case Op::POP:
        return "POP";
    case Op::GET_LOCAL:
        return "GET_LOCAL";
    case Op::SET_LOCAL:
        return "SET_LOCAL";
    case Op::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case Op::GET_GLOBAL:
        return "GET_GLOBAL";
    case Op::SET_GLOBAL:
        return "SET_GLOBAL";
    case Op::JUMP:
        return "JUMP";
    case Op::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case Op::LOOP:
        return "LOOP";
    case Op::CALL:
        return "CALL";
    case Op::RETURN:
        return "RETURN";
    case Op::CLOSURE:
        return "CLOSURE";
    case Op::GET_UPVALUE:
        return "GET_UPVALUE";
    case Op::SET_UPVALUE:
        return "SET_UPVALUE";
    case Op::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    case Op::CLASS:
        return "CLASS";
    case Op::GET_PROPERTY:
        return "GET_PROPERTY";
    case Op::SET_PROPERTY:
        return "SET_PROPERTY";
    case Op::DEFINE_METHOD:
        return "DEFINE_METHOD";
    case Op::INVOKE:
        return "INVOKE";
    case Op::INHERIT:
        return "INHERIT";
    case Op::GET_SUPER:
        return "GET_SUPER";
    case Op::SUPER_INVOKE:
        return "SUPER_INVOKE";
    case Op::BUILD_LIST:
        return "BUILD_LIST";
    case Op::GET_INDEX:
        return "GET_INDEX";
    case Op::SET_INDEX:
        return "SET_INDEX";
    case Op::IN:
        return "IN";
    default:
        return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// ProfileGcScope — RAII wrapper for one GC collection
//
// Constructor: records pre-GC bytes and start timestamp.
// Destructor:  reads bytesAllocated by const-ref (already updated by sweep),
//              computes pause duration and bytes freed, updates GcStats.
// ---------------------------------------------------------------------------

class ProfileGcScope {
  public:
    ProfileGcScope(ProfilerData& data, const std::size_t& bytesAllocated)
        : m_data(data), m_bytesRef(bytesAllocated),
          m_enterBytes(bytesAllocated), m_enterNs(data.nowNs()) {}

    ~ProfileGcScope() {
        int64_t pauseNs = m_data.nowNs() - m_enterNs;
        std::size_t freed =
            (m_enterBytes > m_bytesRef) ? (m_enterBytes - m_bytesRef) : 0;

        GcStats& gc = m_data.gc;
        gc.gcCount++;
        gc.totalPauseNs += pauseNs;
        gc.maxPauseNs = std::max(gc.maxPauseNs, pauseNs);
        gc.totalBytesFreed += freed;
        gc.peakBytesAllocated = std::max(gc.peakBytesAllocated, m_enterBytes);
        if (gc.events.size() < 1024)
            gc.events.push_back({m_enterBytes, m_bytesRef, pauseNs});
    }

    // Non-copyable, non-movable (holds references).
    ProfileGcScope(const ProfileGcScope&) = delete;
    ProfileGcScope& operator=(const ProfileGcScope&) = delete;

  private:
    ProfilerData& m_data;
    const std::size_t&
        m_bytesRef;           // live reference into VmAllocBase::bytesAllocated
    std::size_t m_enterBytes; // snapshot at construction
    int64_t m_enterNs;
};

// ---------------------------------------------------------------------------
// ProfileFunctionScope — RAII wrapper for one Lox function activation
//
// Constructor: increments call count, snapshots name, records entry timestamp.
// Destructor:  computes elapsed time, updates totalNs/selfNs for this function
//              and subtracts elapsed from the parent's selfNs (Graham 1982
//              self-time algorithm).
//
// Stored in std::optional<ProfileFunctionScope> in a parallel array indexed
// by frame depth. Explicit .reset() at Op::RETURN triggers the destructor.
// ---------------------------------------------------------------------------

class ProfileFunctionScope {
  public:
    ProfileFunctionScope(ProfilerData& data, ObjClosure* closure,
                         int frameDepth, ObjClosure* parentClosure)
        : m_data(&data), m_fn(closure->function),
          m_parentFn(parentClosure ? parentClosure->function : nullptr),
          m_depth(frameDepth) {
        FunctionStats& s = data.funcTable[m_fn];
        if (s.name.empty()) {
            if (m_fn->name)
                s.name = std::string(m_fn->name->chars.data(),
                                     m_fn->name->chars.size());
            else
                s.name = "<script>";
        }
        s.callCount++;
        data.frameEnterNs[frameDepth] = data.nowNs();
    }

    ~ProfileFunctionScope() {
        if (!m_data)
            return; // moved-from guard
        int64_t elapsed = m_data->nowNs() - m_data->frameEnterNs[m_depth];
        FunctionStats& callee = m_data->funcTable[m_fn];
        callee.totalNs += elapsed;
        callee.selfNs += elapsed;
        // Graham 1982: subtract callee's elapsed from parent's self time.
        if (m_parentFn)
            m_data->funcTable[m_parentFn].selfNs -= elapsed;
    }

    // Non-copyable; movable so std::optional can construct it.
    ProfileFunctionScope(const ProfileFunctionScope&) = delete;
    ProfileFunctionScope& operator=(const ProfileFunctionScope&) = delete;

    ProfileFunctionScope(ProfileFunctionScope&& o) noexcept
        : m_data(o.m_data), m_fn(o.m_fn), m_parentFn(o.m_parentFn),
          m_depth(o.m_depth) {
        o.m_data = nullptr; // mark moved-from so dtor is a no-op
    }

  private:
    ProfilerData* m_data; // pointer (not ref) to allow move
    ObjFunction* m_fn;
    ObjFunction* m_parentFn; // nullptr for top-level <script>
    int m_depth;
};

// ---------------------------------------------------------------------------
// ProfileProgramScope — RAII wrapper for the entire VM::interpret() call
//
// Constructor: records program start time.
// Destructor:  records end time and prints the full profiler report to stderr.
// ---------------------------------------------------------------------------

class ProfileProgramScope {
  public:
    explicit ProfileProgramScope(ProfilerData& data) : m_data(data) {
        data.programStartNs = data.nowNs();
    }

    ~ProfileProgramScope() {
        m_data.programEndNs = m_data.nowNs();
        m_data.report(stderr);
    }

    // Non-copyable, non-movable.
    ProfileProgramScope(const ProfileProgramScope&) = delete;
    ProfileProgramScope& operator=(const ProfileProgramScope&) = delete;

  private:
    ProfilerData& m_data;
};

// ---------------------------------------------------------------------------
// ProfilerData::report() implementation
// ---------------------------------------------------------------------------

inline void ProfilerData::report(std::FILE* out) const {
    const char* sep = "========================================================"
                      "================"
                      "========\n";

    std::fprintf(out, "%s", sep);
    std::fprintf(out, "loxpp PROFILER REPORT\n");
    std::fprintf(out, "%s\n", sep);

    // --- Program Summary ---
    double totalMs = static_cast<double>(programEndNs - programStartNs) / 1e6;
    double gcTotalMs = static_cast<double>(gc.totalPauseNs) / 1e6;
    double gcMaxMs = static_cast<double>(gc.maxPauseNs) / 1e6;

    std::fprintf(out, "--- Program Summary ---\n");
    std::fprintf(out, "  Total CPU time : %10.3f ms\n", totalMs);
    std::fprintf(out,
                 "  GC pauses      : %5llu collections, %8.3f ms total,"
                 " %8.3f ms max\n",
                 static_cast<unsigned long long>(gc.gcCount), gcTotalMs,
                 gcMaxMs);
    std::fprintf(out, "\n");

    // --- Opcode Dispatch Counts ---
    std::fprintf(out,
                 "--- Opcode Dispatch Counts (non-zero, descending) ---\n");
    std::fprintf(out, "  %-20s  %14s  %10s\n", "Opcode", "Count", "% of Total");

    // Build sorted list of (count, index) pairs.
    struct OpcodeEntry {
        uint64_t count;
        int index;
    };
    std::vector<OpcodeEntry> entries;
    uint64_t totalOps = 0;
    for (int i = 0; i < 64; ++i) {
        if (opcodeTable[i].count > 0) {
            entries.push_back({opcodeTable[i].count, i});
            totalOps += opcodeTable[i].count;
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const OpcodeEntry& a, const OpcodeEntry& b) {
                  return a.count > b.count;
              });
    for (const auto& e : entries) {
        double pct = totalOps > 0 ? 100.0 * static_cast<double>(e.count) /
                                        static_cast<double>(totalOps)
                                  : 0.0;
        std::fprintf(out, "  %-20s  %14llu  %9.2f %%\n",
                     opcodeName(static_cast<Op>(e.index)),
                     static_cast<unsigned long long>(e.count), pct);
    }
    std::fprintf(out, "\n");

    // --- Function Call Profile ---
    std::fprintf(out,
                 "--- Function Call Profile (by total inclusive time) ---\n");
    std::fprintf(out, "  %-24s  %8s  %10s  %10s  %7s\n", "Function", "Calls",
                 "Total(ms)", "Self(ms)", "Self%");

    struct FnEntry {
        const FunctionStats* stats;
    };
    std::vector<FnEntry> fnEntries;
    for (const auto& [fn, stats] : funcTable)
        fnEntries.push_back({&stats});
    std::sort(fnEntries.begin(), fnEntries.end(),
              [](const FnEntry& a, const FnEntry& b) {
                  return a.stats->totalNs > b.stats->totalNs;
              });

    for (const auto& e : fnEntries) {
        const FunctionStats& s = *e.stats;
        double fnTotalMs = static_cast<double>(s.totalNs) / 1e6;
        double fnSelfMs = static_cast<double>(s.selfNs) / 1e6;
        double selfPct = (s.totalNs > 0)
                             ? 100.0 * static_cast<double>(s.selfNs) /
                                   static_cast<double>(s.totalNs)
                             : 0.0;
        std::fprintf(out, "  %-24s  %8llu  %10.3f  %10.3f  %6.2f %%\n",
                     s.name.c_str(),
                     static_cast<unsigned long long>(s.callCount), fnTotalMs,
                     fnSelfMs, selfPct);
    }
    std::fprintf(out, "\n");

    // --- GC Event Log ---
    if (gc.gcCount > 0) {
        std::fprintf(out, "--- GC Event Log ---\n");
        const auto& events = gc.events;
        std::size_t shown = events.size();
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& ev = events[i];
            double pauseMs = static_cast<double>(ev.pauseNs) / 1e6;
            std::size_t freed = (ev.beforeBytes > ev.afterBytes)
                                    ? (ev.beforeBytes - ev.afterBytes)
                                    : 0;
            std::fprintf(out,
                         "  #%-4zu  before=%8zu B  after=%8zu B  freed=%8zu B"
                         "  pause=%8.3f ms\n",
                         i + 1, ev.beforeBytes, ev.afterBytes, freed, pauseMs);
        }
        if (gc.gcCount > shown)
            std::fprintf(out, "  (showing first %zu of %llu collections)\n",
                         shown, static_cast<unsigned long long>(gc.gcCount));
        std::fprintf(out, "\n");
    }

    std::fprintf(out, "%s", sep);
}

#endif // LOXPP_PROFILE
