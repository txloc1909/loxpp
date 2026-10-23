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
#include <set>
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
        if (entry.is_regular_file() && entry.path().extension() == ".lox") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The names of the spec probes that one consumer does not pass yet: the first
// field of each line of tools/spec-probe-xfail/<consumer>.txt, the file that
// tools/check_spec_probes.py reads with --xfail. Blank lines and lines that
// start with '#' are comments. A missing file is an error, and so is a name
// that is not a probe file, so a rename cannot turn a sweep that skips these
// names into one that skips fewer.
inline std::set<std::string> specProbeXfail(const std::string& consumer) {
    const std::filesystem::path path =
        projectRoot() / "tools" / "spec-probe-xfail" / (consumer + ".txt");
    std::istringstream lines(readFile(path));
    std::set<std::string> names;
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string name;
        if (fields >> name && name[0] != '#') {
            if (!std::filesystem::is_regular_file(
                    projectRoot() / "test" / "translation-probes" / name)) {
                throw std::runtime_error(path.string() + " lists " + name +
                                         ", which is not a probe file");
            }
            names.insert(name);
        }
    }
    return names;
}

// The directories the tooling tests sweep. The parser test guards each one
// separately, so a renamed directory cannot turn the sweep into a silent
// no-op hidden behind the others.
inline const std::vector<std::string>& corpusDirectories() {
    static const std::vector<std::string> dirs = {"examples", "bootstrap",
                                                  "test/translation-probes"};
    return dirs;
}

// The files the tooling tests parse. It leaves out the spec probes that the
// tooling does not handle yet (tools/spec-probe-xfail/tooling.txt).
inline std::vector<std::filesystem::path> corpusFiles() {
    const std::set<std::string> notYet = specProbeXfail("tooling");
    std::vector<std::filesystem::path> files;
    for (const std::string& dir : corpusDirectories()) {
        const std::filesystem::path base = projectRoot() / dir;
        if (!std::filesystem::is_directory(base)) {
            continue;
        }
        for (const std::filesystem::path& file : listLoxFiles(base)) {
            if (dir == "test/translation-probes" &&
                notYet.count(file.filename().string()) != 0) {
                continue;
            }
            files.push_back(file);
        }
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

// The translation probes that the native compiler must compile. A test that
// compiles every probe and inspects its bytecode sweeps these. It leaves out
// the static-error probes; ChunkDecoderTest.StaticErrorProbesDoNotCompile
// checks them. It also leaves out the spec probes that native does not pass
// yet (tools/spec-probe-xfail/native.txt): such a probe can use a form that
// the native compiler does not accept yet.
inline std::vector<std::filesystem::path> compiledTranslationProbes() {
    const std::set<std::string> notYet = specProbeXfail("native");
    std::vector<std::filesystem::path> probes;
    for (const std::filesystem::path& probe :
         listLoxFiles(projectRoot() / "test" / "translation-probes")) {
        if (hasStaticErrorMarker(probe) ||
            notYet.count(probe.filename().string()) != 0) {
            continue;
        }
        probes.push_back(probe);
    }
    return probes;
}

} // namespace loxpp_test
