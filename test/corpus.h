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
#include <set>
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
        if (entry.is_regular_file() && entry.path().extension() == ".lox") {
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
        const std::vector<std::filesystem::path> dirFiles = listLoxFiles(base);
        files.insert(files.end(), dirFiles.begin(), dirFiles.end());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A translation probe that carries this line is a static error: every
// consumer must reject it, so the native compiler produces no bytecode for it.
inline bool hasStaticErrorMarker(const std::filesystem::path& probe) {
    std::istringstream lines(readFile(probe));
    std::string line;
    while (std::getline(lines, line)) {
        if (line == "// STATIC-ERROR") {
            return true;
        }
    }
    return false;
}

// The probe file names in tools/xfail/spec-probes-native.txt: the spec probes
// that native does not pass yet. Native may reject such a probe at compile
// time, so it may have no bytecode either.
inline std::set<std::string> nativeExpectedFailureProbes() {
    std::istringstream lines(
        readFile(projectRoot() / "tools" / "xfail" / "spec-probes-native.txt"));
    std::set<std::string> names;
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        const std::size_t end = line.find(' ', start);
        names.insert(line.substr(start, end - start));
    }
    return names;
}

// The translation probes that the native compiler must compile. A test that
// compiles every probe and inspects its bytecode sweeps these. It leaves out
// the static-error probes and the probes on native's expected-failure list;
// ChunkDecoderTest.StaticErrorProbesDoNotCompile checks the first group.
inline std::vector<std::filesystem::path> compiledTranslationProbes() {
    const std::set<std::string> expectedFailures =
        nativeExpectedFailureProbes();
    std::vector<std::filesystem::path> probes;
    for (const std::filesystem::path& probe :
         listLoxFiles(projectRoot() / "test" / "translation-probes")) {
        if (expectedFailures.count(probe.filename().string()) != 0U ||
            hasStaticErrorMarker(probe)) {
            continue;
        }
        probes.push_back(probe);
    }
    return probes;
}

} // namespace loxpp_test
