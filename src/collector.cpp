#include "collector.h"
#include "incremental_collector.h"
#include "stw_collector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

std::uint64_t gcMonotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Trace line formats are documented in tools/gc_report.py.
std::FILE* openGcTraceFile(int workerNumber) {
    const char* env = std::getenv("LOXPP_GC_TRACE");
    if (env == nullptr || env[0] == '\0') {
        return nullptr;
    }
    std::string path = env;
    if (workerNumber > 0) {
        path += ".w" + std::to_string(workerNumber);
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        std::fprintf(stderr, "LOXPP_GC_TRACE: cannot open '%s'\n",
                     path.c_str());
        return nullptr;
    }
    std::fprintf(f, "# loxpp-gc-trace v3\nstart t=%llu\n",
                 static_cast<unsigned long long>(gcMonotonicNs()));
    std::fflush(f);
    return f;
}

std::unique_ptr<Collector> makeCollectorByName(const char* name,
                                               MemoryManager& heap,
                                               PollState& poll,
                                               int workerNumber) {
    if (std::strcmp(name, "stw") == 0) {
        return std::make_unique<StwCollector>(heap, poll, workerNumber);
    }
    if (std::strcmp(name, "incremental") == 0) {
        return std::make_unique<IncrementalCollector>(heap, poll, workerNumber);
    }
    return nullptr;
}

std::unique_ptr<Collector> makeCollector(MemoryManager& heap, PollState& poll,
                                         int workerNumber) {
    const char* name = std::getenv("LOXPP_GC");
    if (name == nullptr || name[0] == '\0') {
        name = kDefaultCollector;
    }
    std::unique_ptr<Collector> collector =
        makeCollectorByName(name, heap, poll, workerNumber);
    if (collector == nullptr) {
        std::fprintf(stderr,
                     "error: LOXPP_GC: unknown collector '%s' (known: stw, "
                     "incremental)\n",
                     name);
        std::exit(64);
    }
    return collector;
}
