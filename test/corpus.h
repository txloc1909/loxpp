// test/corpus.h — shared corpus scanning for tests.
//
// The backend and tooling tests all walk the same .lox corpora on disk. Keep
// the path resolution and the directory scan in one place so the two families
// cannot drift. Deliberately free of VM headers: the tooling tests include it
// without pulling in value.h/vm.h.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef LOXPP_PROJECT_SOURCE_DIR
#error                                                                         \
    "LOXPP_PROJECT_SOURCE_DIR must be defined by the build (see test/CMakeLists.txt)"
#endif

namespace loxpp_test {

inline std::filesystem::path projectRoot() {
    return std::filesystem::path(LOXPP_PROJECT_SOURCE_DIR);
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

inline std::vector<std::filesystem::path>
listLoxFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".lox") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The directories the tooling tests sweep. The parser test guards each one
// separately, so a renamed directory cannot turn the sweep into a silent
// no-op hidden behind the others.
inline const std::vector<std::string>& corpusDirectories() {
    static const std::vector<std::string> dirs = {"examples", "bootstrap",
                                                  "test/translation-probes"};
    return dirs;
}

inline std::vector<std::filesystem::path> corpusFiles() {
    std::vector<std::filesystem::path> files;
    for (const std::string& dir : corpusDirectories()) {
        const std::filesystem::path base = projectRoot() / dir;
        if (!std::filesystem::is_directory(base)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().extension() == ".lox") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace loxpp_test
