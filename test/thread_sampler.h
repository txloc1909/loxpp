#pragma once

#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// Samples the CPU use and the state of a named thread of this process, to
// check that a worker sleeps and does not spin.

// The CPU time in clock ticks, utime + stime, and the state of the thread
// named `name`, from /proc/self/task/<tid>/stat. Ticks are -1 when no
// thread has the name.
struct ThreadSample {
    long ticks{-1};
    char state{'?'};
};

inline ThreadSample sampleThread(const std::string& name) {
    ThreadSample out;
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return out;
    }
    while (dirent* entry = readdir(dir)) {
        std::string tid = entry->d_name;
        if (tid == "." || tid == "..") {
            continue;
        }
        std::ifstream comm("/proc/self/task/" + tid + "/comm");
        std::string threadName;
        std::getline(comm, threadName);
        if (threadName != name) {
            continue;
        }
        // read(2), not iostreams: a thread that exits during the read makes
        // the read fail with ESRCH, and a stream buffer iterator throws then.
        std::string stat;
        const int fd = ::open(("/proc/self/task/" + tid + "/stat").c_str(),
                              O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buf[512];
            ssize_t n;
            while ((n = ::read(fd, buf, sizeof buf)) > 0) {
                stat.append(buf, static_cast<std::size_t>(n));
            }
            ::close(fd);
        }
        // A thread that exits between the read of its name and the read of its
        // stat leaves no stat text. That thread is gone: report no sample.
        const std::size_t paren = stat.rfind(')');
        if (paren == std::string::npos || paren + 2 > stat.size()) {
            break;
        }
        // The fields after the ")" of the command name: state is field 3,
        // utime and stime are fields 14 and 15.
        std::istringstream rest(stat.substr(paren + 2));
        std::vector<std::string> fields;
        std::string field;
        while (rest >> field) {
            fields.push_back(field);
        }
        if (fields.size() > 12) {
            out.state = fields[0][0];
            out.ticks = std::stol(fields[11]) + std::stol(fields[12]);
        }
        break;
    }
    closedir(dir);
    return out;
}

// True when the thread named `name` shows a quiet window: a window of 100 ms
// with at most one clock tick of CPU time that ends with the thread in the
// sleep state. The thread may start slowly under load, and a start in a debug
// build or under GC stress uses CPU for a while, so the windows run until one
// is quiet or `maxMs` pass. A thread that spins is busy in every window and
// is never quiet.
inline bool seesQuietWindow(const std::string& name, int maxMs) {
    ThreadSample last = sampleThread(name);
    for (int waited = 0; waited < maxMs; waited += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ThreadSample now = sampleThread(name);
        if (last.ticks >= 0 && now.ticks >= 0 && now.ticks - last.ticks <= 1 &&
            now.state == 'S') {
            return true;
        }
        last = now;
    }
    return false;
}
