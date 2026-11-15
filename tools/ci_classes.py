"""Reads .github/ci-classes.txt and turns a list of changed files into the CI
work that they need (issue #1258).

The map has three kinds of lines:

    path  <glob> <class>[,<class>...]    the first path line that matches a
                                         file gives the classes of the file
    class <name> [gates=<g>,...] [labels=<l>,...]
                                         what a class runs: gates (units of CI
                                         work) and the CTest labels that the
                                         unit-test gate runs
    job   <job id of ci.yml> gates=<g>,...
                                         the gates that a job of ci.yml
                                         belongs to
    ignore <job id> <glob>               files that the job names but that no
                                         change of them needs it for. The line
                                         before it says why.

A glob is anchored at the repository root. `*` does not cross a `/`, and `**`
does. The gate `placement` always runs. `gates=all` and `labels=all` stand for
every gate and every label. A file that no path line matches selects the class
`ci`, which runs everything, so a new directory cannot go untested.
"""

import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAP_PATH = ROOT / ".github" / "ci-classes.txt"

UNKNOWN_CLASS = "ci"
ALWAYS = {"placement"}


@dataclass
class ClassMap:
    paths: list = field(default_factory=list)  # (glob, regex, [class])
    classes: dict = field(default_factory=dict)  # name -> (gates, labels)
    jobs: dict = field(default_factory=dict)  # job id -> gates
    ignores: list = field(default_factory=list)  # (job id, glob, regex)
    errors: list = field(default_factory=list)

    @property
    def all_gates(self):
        gates = set(ALWAYS)
        for class_gates, _ in self.classes.values():
            gates |= class_gates - {"all"}
        for job_gates in self.jobs.values():
            gates |= job_gates
        return gates


def glob_to_regex(glob):
    out = []
    i = 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif glob.startswith("**", i):
            out.append(".*")
            i += 2
        elif glob[i] == "*":
            out.append("[^/]*")
            i += 1
        elif glob[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(glob[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def split(value):
    return {item for item in value.split(",") if item}


def parse_map(text, source="ci-classes.txt"):
    result = ClassMap()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("  #", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        where = f"{source}:{number}"
        kind = fields[0]
        if kind == "path" and len(fields) == 3:
            result.paths.append((fields[1], glob_to_regex(fields[1]), sorted(split(fields[2]))))
        elif kind == "class" and len(fields) >= 2:
            options = dict(f.split("=", 1) for f in fields[2:] if "=" in f)
            unknown = set(options) - {"gates", "labels"}
            if unknown or len(options) != len(fields) - 2:
                result.errors.append(f"{where}: bad class line {line!r}")
                continue
            if fields[1] in result.classes:
                result.errors.append(f"{where}: the class {fields[1]} has a second line")
            result.classes[fields[1]] = (split(options.get("gates", "")), split(options.get("labels", "")))
        elif kind == "job" and len(fields) == 3 and fields[2].startswith("gates="):
            result.jobs[fields[1]] = split(fields[2][len("gates="):])
        elif kind == "ignore" and len(fields) == 3:
            result.ignores.append((fields[1], fields[2], glob_to_regex(fields[2])))
        else:
            result.errors.append(f"{where}: cannot read {line!r}")
    for glob, _, names in result.paths:
        for name in names:
            if name not in result.classes:
                result.errors.append(f"path {glob}: the class {name} has no class line")
    if UNKNOWN_CLASS not in result.classes:
        result.errors.append(f"the map has no class line for {UNKNOWN_CLASS}")
    return result


def load_map(path=MAP_PATH):
    return parse_map(path.read_text(), path.name)


def classes_of(class_map, file):
    """The classes of one file, or None when no path line matches it."""
    for _, regex, names in class_map.paths:
        if regex.match(file):
            return names
    return None


@dataclass
class Selection:
    classes: set
    gates: set
    labels: set  # contains "all" when every label runs


def select(class_map, files):
    classes = set()
    for file in files:
        names = classes_of(class_map, file)
        classes |= set(names) if names is not None else {UNKNOWN_CLASS}
    return select_classes(class_map, classes)


def select_classes(class_map, classes):
    gates = set(ALWAYS)
    labels = set()
    for name in classes:
        class_gates, class_labels = class_map.classes[name]
        gates |= class_gates
        labels |= class_labels
    if "all" in gates:
        gates = class_map.all_gates
    if "all" in labels:
        labels = {"all"}
    if labels:
        gates.add("native-unit")
    return Selection(set(classes), gates, labels)


def labels_selected(selection_labels, test_labels):
    return "all" in selection_labels or bool(set(selection_labels) & set(test_labels))


def changed_files(base, head="HEAD"):
    out = subprocess.run(
        ["git", "diff", "--name-only", "--no-renames", f"{base}...{head}"],
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, check=True, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line]


def main(argv):
    """tools/ci_classes.py [--base REF [--head REF] | FILE...]

    Prints the classes, the gates and the CTest labels that the files need, one
    `name=a,b,c` line each, in the form GitHub Actions reads from GITHUB_OUTPUT.
    Also prints `gate-<name>=true|false` for every gate.
    """
    args = argv[1:]
    class_map = load_map()
    if class_map.errors:
        print("\n".join(class_map.errors), file=sys.stderr)
        return 2
    if args[:1] == ["--base"]:
        head = args[3] if args[2:3] == ["--head"] else "HEAD"
        files = changed_files(args[1], head)
    else:
        files = args
    selection = select(class_map, files)
    print("classes=" + ",".join(sorted(selection.classes)))
    print("gates=" + ",".join(sorted(selection.gates)))
    print("labels=" + ",".join(sorted(selection.labels)))
    for gate in sorted(class_map.all_gates | {"native-unit"}):
        print(f"gate-{gate}={'true' if gate in selection.gates else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
