#include "collector.h"
#include "stw_collector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

std::unique_ptr<Collector> makeCollectorByName(const char* name,
                                               MemoryManager& heap,
                                               PollState& poll,
                                               int workerNumber) {
    if (std::strcmp(name, "stw") == 0) {
        return std::make_unique<StwCollector>(heap, poll, workerNumber);
    }
    return nullptr;
}

std::unique_ptr<Collector> makeCollector(MemoryManager& heap, PollState& poll,
                                         int workerNumber) {
    const char* name = std::getenv("LOXPP_GC");
    if (name == nullptr || name[0] == '\0') {
        name = "stw";
    }
    std::unique_ptr<Collector> collector =
        makeCollectorByName(name, heap, poll, workerNumber);
    if (collector == nullptr) {
        std::fprintf(stderr,
                     "error: LOXPP_GC: unknown collector '%s' (known: stw)\n",
                     name);
        std::exit(64);
    }
    return collector;
}
