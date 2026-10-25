/**
 * The container gate behind each `loxpp_gate` scope.
 *
 * A scope is one image plus one script: the image carries the toolchain, and
 * the script builds the tree and runs the checks CI's own job runs for that
 * backend. The scopes live here, outside the extension, so the drift test in
 * gate-scopes.test.ts can import them without loading the pi extension API.
 *
 * The script text is not a copy of CI's YAML by hand-count alone: the test
 * compares the script names of each `qbe` step against the `QBE toolchain`
 * job of .github/workflows/ci.yml, so a step CI gains or loses fails the
 * test instead of the next QBE node's checkpoint.
 */

export const GATE_SCOPES = ["native", "backend", "qbe"] as const;
export type GateScope = (typeof GATE_SCOPES)[number];

/** The image each scope runs in. AGENTS.md's task loop builds the first two. */
export const GATE_IMAGES: Record<GateScope, string> = {
  native: "loxpp-dev-env",
  backend: "loxpp-dev-env-managed",
  qbe: "loxpp-dev-qbe",
};

const NATIVE_SCRIPT = `set -e
cd /workspace
cmake --preset debug
cmake --build build
ctest --test-dir build --output-on-failure -j$(nproc)`;

// The debug preset's sanitizer flags live in CMAKE_CXX_FLAGS, which the
// release preset does not set. A cache written by an earlier debug gate in the
// same build/ therefore survives `cmake --preset release`, and every object
// keeps -fsanitize=address,undefined while the release linker flags lose the
// sanitizer runtime: a QBE program's own plain `cc` link then fails on undefined
// __asan_* symbols. Clear the value so a release gate is a release build no
// matter which gate configured the directory first.
const RELEASE_CONFIGURE = `cmake --preset release -DCMAKE_CXX_FLAGS=`;

const BACKEND_SCRIPT = `set -e
cd /workspace
tools/build_lox_rt.sh
tools/check_managed_toolchains.sh
${RELEASE_CONFIGURE}
cmake --build build
ctest --test-dir build --output-on-failure -j$(nproc)
# The differential phase is the one value this gate cannot guess: it needs a
# native binary, a runner, and a corpus. CI's "JVM differential suite -
# probes" step is the reference. A node on another managed backend adds its
# own runner line here rather than replacing this one (#855).
python3 tools/diff_runtimes.py build/loxpp tools/loxpp_jvm.sh \\
  test/translation-probes/ test/translation-probes/jvm-only/ \\
  --xfail tools/spec-probe-xfail/jvm.txt \\
  --oracle-xfail tools/spec-probe-xfail/native.txt`;

/**
 * The steps of CI's `QBE toolchain` job, in the job's own order, with the
 * build prefix of each step lifted out: the gate builds once, before the
 * steps. Each step's name is the CI step's name, so the drift test compares
 * the two lists position by position.
 *
 * The build is the release preset, because a debug build's output carries
 * LOXPP_DEBUG_PRINT_CODE's bytecode trace and every comparison with the
 * QBE-compiled binary would then "diverge" (AGENTS.md, task loop).
 */
export const QBE_GATE_STEPS: readonly { name: string; script: string }[] = [
  {
    name: "Check QBE toolchain",
    script: `tools/check_qbe_toolchain.sh`,
  },
  {
    name: "Check libloxrt.a + startup (S2)",
    script: `tools/check_qbe_libloxrt.sh`,
  },
  {
    name: "Check straight-line code and jumps (S3)",
    script: `tools/check_qbe_s3_straight_line.sh`,
  },
  {
    name: "Check calls and closures (S4)",
    script: `tools/check_qbe_s4_calls_closures.sh`,
  },
  {
    name: "Check the rest of the language (S5)",
    script: `tools/check_qbe_s5_rest_of_language.sh`,
  },
  {
    name: "Check errors (S6)",
    script: `tools/check_qbe_s6_errors.sh`,
  },
  {
    name: "Check QBE probes (S7)",
    script: `tools/check_qbe_probes.sh`,
  },
  {
    name: "Check spec probes - QBE",
    script: `python3 tools/check_spec_probes.py tools/loxpp_qbe.sh test/translation-probes/ \
  --xfail tools/spec-probe-xfail/qbe.txt`,
  },
  {
    name: "QBE differential suite - compile errors",
    script: `python3 tools/diff_runtimes.py build/loxpp tools/loxpp_qbe.sh \
  test/compile-errors/ --compile-errors`,
  },
  {
    name: "Check print lines of two workers - QBE",
    script: `python3 tools/check_print_lines.py tools/loxpp_qbe.sh`,
  },
  {
    name: "Check register promotion under stress GC (S8)",
    script: `tools/check_qbe_s8_promotion.sh`,
  },
  {
    name: "QBE differential suite - full example corpus",
    script: `python3 tools/diff_runtimes.py build/loxpp tools/loxpp_qbe.sh examples/`,
  },
  {
    name: "--target qbe front end - standalone binary",
    // Compile to a standalone executable, DELETE the source, and run the
    // executable without it. Deleting the source is the point: an
    // implementation that reads the .lox at run time by path passes a test
    // that leaves the source in place.
    script: `src=$(mktemp -d)
cp examples/fibonacci.lox "$src/prog.lox"
build/loxpp --target qbe -o /tmp/standalone "$src/prog.lox"
rm -rf "$src"
native=$(build/loxpp examples/fibonacci.lox)
moved=$(cd /tmp && ./standalone)
if [ "$native" != "$moved" ]; then
  echo "standalone output differs from native" >&2
  exit 1
fi
printf "print args();" > /tmp/args.lox
build/loxpp --target qbe -o /tmp/argbin /tmp/args.lox
rm -f /tmp/args.lox
got=$(/tmp/argbin alpha beta)
if [ "$got" != "[alpha, beta]" ]; then
  echo "args() mismatch: expected [alpha, beta], got $got" >&2
  exit 1
fi
echo "OK: standalone binary embeds its source and exposes args()"`,
  },
  {
    name: "QBE differential suite - full example corpus through the self-hosted interpreter",
    // bench_jump_table.lox is left out by name, not through an exclusion
    // file: a 5,000,000-iteration microbenchmark, interpreted twice over,
    // does not finish inside this step's timeout on either side — measured,
    // not a QBE defect.
    script: `files=$(find examples -maxdepth 1 -name "*.lox" ! -name "bench_jump_table.lox" | sort)
LANGUAGE=LOXPP python3 tools/diff_runtimes.py bootstrap/lox_wrapper.sh \\
  tools/loxpp_qbe_bootstrap.sh $files --timeout 60`,
  },
  {
    name: "QBE bootstrap wrapper exits with stdin open",
    script: `python3 tools/check_bootstrap_stdin_exit.py tools/loxpp_qbe_bootstrap.sh`,
  },
  {
    name: "QBE differential suite - bootstrap wrapper NUL and last-line probes",
    script: `LANGUAGE=LOXPP python3 tools/diff_runtimes.py build/loxpp tools/loxpp_qbe_bootstrap.sh \\
  test/translation-probes/32_string_nul.lox \\
  test/translation-probes/56_file_nul.lox \\
  test/translation-probes/bootstrap-only/stdout_no_trailing_newline.lox \\
  --timeout 300
if LANGUAGE=LOXPP LOXPP_BIN=/nonexistent tools/loxpp_qbe_bootstrap.sh \\
    test/translation-probes/32_string_nul.lox >/dev/null 2>&1; then
  echo "the QBE bootstrap wrapper exited 0 when its host failed" >&2
  exit 1
fi`,
  },
];

const QBE_SCRIPT = `set -e
cd /workspace
${RELEASE_CONFIGURE}
cmake --build build
${QBE_GATE_STEPS.map((step) => step.script).join("\n")}`;

const SCRIPTS: Record<GateScope, string> = {
  native: NATIVE_SCRIPT,
  backend: BACKEND_SCRIPT,
  qbe: QBE_SCRIPT,
};

/** The image and the bash script one gate run executes. */
export function gatePlan(scope: GateScope): { image: string; script: string } {
  return { image: GATE_IMAGES[scope], script: SCRIPTS[scope] };
}
