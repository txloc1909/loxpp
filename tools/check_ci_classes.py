#!/usr/bin/env python3
"""Holds .github/ci-classes.txt to the repository (issue #1258).

The map says which CI work a changed file needs. A wrong line makes a pull
request skip a test that it needs, and nothing else shows that. This script
reads the repository as text, so it needs no build, and fails when:

1. a tracked file matches no path line;
2. the gates that a job of ci.yml tests in its `if:` lines differ from its
   `job` line, or the job `changes` does not publish a gate that a job tests;
   a file that a job of ci.yml names in its steps does not select that job
   (the job's gates, in the `job` lines, are the gates it belongs to);
3. a script or program that an add_test() command of test/CMakeLists.txt runs
   does not select a label of that test;
4. a C++ test source that a test target builds has a class other than
   `test-source`, or a file of that class is built by no target;
5. a file under src/backend has a class that its consumers do not give it.
   The consumers come from the #include graph: the JVM emitter, the QBE
   emitter and the C API for the QBE runtime. A file that both use is
   `shared-backend`, a file that only one uses has that backend's class, and
   a file that the native VM itself includes is `core`;
6. a class has a gate or label that nothing defines;
7. a case of tools/testdata/ci-classify.txt, which lists the files of a change
   and the gates and labels that it must select, does not hold.

A check of the text of a job cannot see a script that another script calls, so
2 and 3 hold the first level of references only.

Exit status 0 when the map holds, 1 when it does not.
"""

import fnmatch
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_classes  # noqa: E402

ROOT = ci_classes.ROOT
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
CMAKE = ROOT / "test" / "CMakeLists.txt"
LABELS = ROOT / "test" / "test_labels.txt"
CASES = ROOT / "tools" / "testdata" / "ci-classify.txt"

# A path that a job of ci.yml names, as a token. Globs and directories expand to
# the tracked files below.
REF = re.compile(
    r"(?<![\w/.\-])((?:tools|examples|bootstrap|test|editors|runtime|benchmarks|src|spec|packaging|completions)/"
    r"[\w./*{},\-]*|install\.sh)"
)

# The sweeps of examples/ and test/ are not recursive, so a job that names a
# directory does not name what is in the directory of a class of its own.
NON_RECURSIVE_EXEMPT = {
    "examples/codecrafters/": {"build-and-test", "managed-toolchains", "qbe-toolchain", "release-static", "tree-sitter"},
}


def job_texts(yml):
    """Returns {job id: text without comment lines}."""
    jobs = {}
    current = None
    in_jobs = False
    for line in yml.splitlines():
        if re.match(r"^jobs:\s*$", line):
            in_jobs = True
            continue
        header = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if in_jobs and header:
            current = header.group(1)
            jobs[current] = []
        elif current and not line.lstrip().startswith("#"):
            jobs[current].append(line)
    return {job: "\n".join(lines) for job, lines in jobs.items()}


def expand(token, files):
    """The tracked files that a path token in a job names."""
    token = token.rstrip(".,;:'\"\\)`")
    token = re.sub(r"\{[^}]*\}", "*", token)
    if token in files:
        return [token]
    if "*" in token:
        return [f for f in files if fnmatch.fnmatchcase(f, token)]
    prefix = token if token.endswith("/") else token + "/"
    return [f for f in files if f.startswith(prefix)]


def selects_job(class_map, file, gates):
    names = ci_classes.classes_of(class_map, file)
    if names is None:
        return True  # check 1 reports it
    selection = ci_classes.select_classes(class_map, set(names))
    return bool(selection.gates & gates)


def check_job_references(class_map, files, yml):
    errors = []
    fileset = set(files)
    for job, text in job_texts(yml).items():
        gates = class_map.jobs.get(job)
        if gates is None:
            errors.append(f"ci.yml: the job {job} has no job line in the map")
            continue
        seen = set()
        for token in REF.findall(text):
            raw = token.rstrip(".,;:'\"\\)`")
            named = expand(token, fileset)
            if raw not in fileset:
                for prefix, jobs in NON_RECURSIVE_EXEMPT.items():
                    if job in jobs and not raw.startswith(prefix.rstrip("/")):
                        named = [f for f in named if not f.startswith(prefix)]
            for file in named:
                if file in seen:
                    continue
                seen.add(file)
                if any(j == job and regex.match(file) for j, _, regex in class_map.ignores):
                    continue
                if not selects_job(class_map, file, gates):
                    names = ",".join(ci_classes.classes_of(class_map, file))
                    errors.append(f"{file} (class {names}) is named by the job {job}, which it does not select")
    for job, glob, _ in class_map.ignores:
        if job not in class_map.jobs:
            errors.append(f"the ignore line for {job} {glob} names a job that has no job line")
    for job in class_map.jobs:
        if job not in job_texts(yml):
            errors.append(f"the map has a job line for {job}, which ci.yml does not have")
    return errors


def check_job_gates(class_map, yml):
    """The gates that a job tests in its `if:` lines are the gates of its job
    line, and the `changes` job publishes every gate that a job tests."""
    errors = []
    texts = job_texts(yml)
    tested_anywhere = set()
    for job, text in texts.items():
        if job == "changes":
            continue
        tested = set(re.findall(r"outputs\.gate-([a-z-]+)", text))
        if "outputs.native-legs" in text:
            tested.add("native-build")
        tested_anywhere |= tested
        want = class_map.jobs.get(job, set()) - ci_classes.ALWAYS
        if tested != want:
            errors.append(
                f"ci.yml: the job {job} tests the gates {sorted(tested)}, "
                f"but its job line in the map has {sorted(want)}"
            )
    published = set(re.findall(r"^      gate-([a-z-]+):", texts.get("changes", ""), re.M))
    for gate in sorted(tested_anywhere - published - {"native-build"}):
        errors.append(f"ci.yml: a job tests the gate {gate}, which the job changes does not publish")
    for gate in sorted(published - tested_anywhere):
        errors.append(f"ci.yml: the job changes publishes the gate {gate}, which no job tests")
    return errors


def parse_labels(text):
    rows = {}
    for line in text.splitlines():
        if line.strip() and not line.startswith("#"):
            name, labels = line.split()
            rows[name] = labels.split(",")
    return rows


def add_test_blocks(cmake):
    """Returns {test name: [paths of the repository that its command names]}."""
    text = re.sub(r"(?m)#.*$", "", cmake)
    blocks = {}
    for match in re.finditer(r"add_test\(\s*NAME\s+(\S+)(.*?)\n\s*\)?\s*(?:WORKING_DIRECTORY[^)]*)?\)", text, re.S):
        name = match.group(1)
        body = match.group(2)
        # the command may end at the first closing paren that is not a generator expression
        paths = re.findall(r"\$\{PROJECT_SOURCE_DIR\}/([\w./\-]+)", body.split("WORKING_DIRECTORY")[0])
        blocks[name] = paths
    return blocks


def check_test_references(class_map, files, cmake, labels):
    errors = []
    fileset = set(files)
    for name, paths in add_test_blocks(cmake).items():
        test_labels = labels.get(name)
        if test_labels is None:
            continue  # tools/check_test_labels.py reports it
        for path in paths:
            if path not in fileset:
                continue
            names = ci_classes.classes_of(class_map, path)
            if names is None:
                continue
            selection = ci_classes.select_classes(class_map, set(names))
            if not ci_classes.labels_selected(selection.labels, test_labels):
                errors.append(
                    f"{path} (class {','.join(names)}) is run by the test {name}, "
                    f"which has the labels {','.join(test_labels)}; its class runs {','.join(sorted(selection.labels)) or 'none'}"
                )
    return errors


def check_test_sources(class_map, files, cmake):
    """A C++ test source selects the labels of its own tests, so a target must
    build it. One that no target builds would select every label, and a test
    file that nothing builds is probably a mistake."""
    errors = []
    sources = ci_classes.test_sources(cmake)
    for file in files:
        names = ci_classes.classes_of(class_map, file)
        if names and "test-source" in names and file not in sources:
            errors.append(f"{file} is a C++ test source that no test target builds")
    for source in sorted(sources):
        names = ci_classes.classes_of(class_map, source)
        if names != ["test-source"]:
            errors.append(f"{source} is built by a test target, but its class is {','.join(names or ['none'])}, not test-source")
    return errors


JVM_ROOTS = {"jvm_emitter"}
QBE_ROOTS = {"qbe_emitter", "qbe_promotion", "qbe_frontend", "rt_capi"}
DRIVER = {"main"}  # src/main.cpp calls every backend, so it is no consumer of one


def include_graph(files):
    """Returns ({stem: set of stems it includes}, {stem: [files]}) for src/."""
    stems = {}
    for f in files:
        if f.startswith("src/") and f.endswith((".cpp", ".h")):
            stems.setdefault(Path(f).stem, []).append(f)
    graph = {}
    for stem, paths in stems.items():
        included = set()
        for path in paths:
            for line in (ROOT / path).read_text(errors="replace").splitlines():
                m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
                if m:
                    target = Path(m.group(1)).stem
                    if target in stems and target != stem:
                        included.add(target)
        graph[stem] = included
    return graph, stems


def reachable(graph, roots):
    seen = set()
    todo = [r for r in roots if r in graph]
    while todo:
        stem = todo.pop()
        if stem in seen:
            continue
        seen.add(stem)
        todo.extend(graph.get(stem, ()))
    return seen


def expected_backend_class(stem, graph, stems):
    jvm = reachable(graph, JVM_ROOTS)
    qbe = reachable(graph, QBE_ROOTS)
    native_roots = {
        other
        for other, paths in stems.items()
        if other not in DRIVER and not all(p.startswith(("src/backend/", "src/tooling/", "src/lsp/")) for p in paths)
    }
    if stem in reachable(graph, native_roots):
        return "core"
    in_jvm, in_qbe = stem in jvm, stem in qbe
    if in_jvm and in_qbe:
        return "shared-backend"
    if in_jvm:
        return "jvm"
    if in_qbe:
        return "qbe"
    return None


def check_backend_classes(class_map, files):
    errors = []
    graph, stems = include_graph(files)
    for stem, paths in sorted(stems.items()):
        if not paths[0].startswith("src/backend/"):
            continue
        expected = expected_backend_class(stem, graph, stems)
        for path in paths:
            names = ci_classes.classes_of(class_map, path)
            if names is None:
                continue
            if expected is None:
                errors.append(f"{path}: no backend includes it, so its class {','.join(names)} has no consumer to match")
            elif names != [expected]:
                errors.append(f"{path}: the include graph gives it the class {expected}, but the map says {','.join(names)}")
    return errors


def check_classes(class_map, files):
    errors = list(class_map.errors)
    known_gates = set(class_map.all_gates) | {"all", "native-unit"}
    known_labels = set()
    for labels in parse_labels(LABELS.read_text()).values():
        known_labels |= set(labels)
    for name, (gates, labels) in class_map.classes.items():
        for gate in gates - known_gates:
            errors.append(f"the class {name} has the gate {gate}, which no job line carries")
        job_gates = set().union(*class_map.jobs.values()) if class_map.jobs else set()
        for gate in gates - {"all"}:
            if gate not in job_gates and gate not in ci_classes.ALWAYS:
                errors.append(f"the class {name} has the gate {gate}, which no job line carries")
        for label in labels - {"all"} - known_labels:
            errors.append(f"the class {name} has the label {label}, which no test has")
    for gate in sorted(set().union(*class_map.jobs.values()) - {"native-unit"}):
        runs = [n for n, (g, _) in class_map.classes.items() if gate in g or "all" in g]
        if not runs:
            errors.append(f"the gate {gate} is in a job line, but no class selects it")
    return errors


def parse_cases(text):
    cases = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        keyword, _, rest = line.partition(" ")
        if keyword == "case":
            cases.append({"name": rest, "files": [], "line": number})
        elif keyword in ("files", "gates", "labels", "legs", "targets") and cases:
            cases[-1].setdefault(keyword, [])
            cases[-1][keyword] += rest.replace(",", " ").split() if keyword != "files" else rest.split()
        else:
            raise ValueError(f"{CASES.name}:{number}: cannot read {line!r}")
    return cases


def check_cases(class_map, text, rows, needs):
    errors = []
    for case in parse_cases(text):
        name = case["name"]
        for key in ("files", "gates", "labels"):
            if not case.get(key):
                errors.append(f"{CASES.name}: the case {name!r} has no {key} line")
        if any(not case.get(k) for k in ("files", "gates", "labels")):
            continue
        selection = ci_classes.select(class_map, case["files"])
        want_gates = class_map.all_gates | {"native-unit"} if case["gates"] == ["ALL"] else set(case["gates"])
        want_labels = set() if case["labels"] == ["-"] else set(case["labels"])
        if selection.gates != want_gates:
            errors.append(
                f"{CASES.name}: {name!r}: gates differ; "
                f"missing {sorted(want_gates - selection.gates)}, extra {sorted(selection.gates - want_gates)}"
            )
        if selection.labels != want_labels:
            errors.append(
                f"{CASES.name}: {name!r}: labels are {sorted(selection.labels)}, want {sorted(want_labels)}"
            )
        if "legs" in case:
            legs = ci_classes.native_legs(selection.gates)
            want = [] if case["legs"] == ["-"] else case["legs"]
            if legs != want:
                errors.append(f"{CASES.name}: {name!r}: the native legs are {legs}, want {want}")
        if "targets" in case:
            _, targets = ci_classes.ctest_selection(selection.labels, rows, needs)
            got = "all" if targets is None else (" ".join(targets) or "-")
            want = " ".join(case["targets"])
            if got != want:
                errors.append(f"{CASES.name}: {name!r}: the test targets are {got!r}, want {want!r}")
    return errors


def check(class_map, files, yml, cmake, labels):
    errors = check_classes(class_map, files)
    for file in files:
        if ci_classes.classes_of(class_map, file) is None:
            errors.append(f"{file} matches no path line")
    errors += check_job_references(class_map, files, yml)
    errors += check_job_gates(class_map, yml)
    errors += check_test_references(class_map, files, cmake, labels)
    errors += check_test_sources(class_map, files, cmake)
    errors += check_backend_classes(class_map, files)
    return errors


def main():
    class_map = ci_classes.load_map()
    if class_map.errors:
        print("\n".join(class_map.errors), file=sys.stderr)
        return 1
    errors = check(
        class_map,
        ci_classes.tracked_files(),
        CI_YML.read_text(),
        CMAKE.read_text(),
        parse_labels(LABELS.read_text()),
    )
    errors += check_cases(
        class_map,
        CASES.read_text(),
        ci_classes.read_label_rows(LABELS.read_text()),
        ci_classes.test_targets(CMAKE.read_text()),
    )
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print("the CI class map holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
