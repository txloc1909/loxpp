/**
 * Tests for the gate scopes. Run with:
 *   node --test .pi/extensions/lib/gate-scopes.test.ts
 *
 * The `qbe` scope exists so a QBE node's checkpoint is one gate run instead
 * of a hand-copied list of CI steps. A copy drifts: a step CI gains is missing
 * from the copy, and the gap shows only after the merge. The first two tests
 * compare the scope's steps against CI's own `QBE toolchain` job, so the two
 * cannot disagree without failing here.
 */

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as path from "node:path";
import test from "node:test";

import { GATE_IMAGES, QBE_GATE_STEPS, gatePlan } from "./gate-scopes.ts";

const CI_PATH = path.join(import.meta.dirname, "../../../.github/workflows/ci.yml");
const CI = readFileSync(CI_PATH, "utf8");

interface CiStep {
  name: string;
  /** The body of the step's `run: |` block; undefined when the step has none. */
  run: string | undefined;
}

/** The `qbe-toolchain` job's own text, up to the next top-level job key. */
function qbeJob(): string {
  const lines = CI.split("\n");
  const start = lines.findIndex((line) => /^ {2}qbe-toolchain:\s*$/.test(line));
  assert.notEqual(start, -1, "ci.yml has no qbe-toolchain job");
  const end = lines.findIndex((line, i) => i > start && /^ {2}[A-Za-z0-9_-]+:\s*$/.test(line));
  return lines.slice(start, end === -1 ? undefined : end).join("\n");
}

/** The job's named steps, in order, with the body of each `run:` block. */
function ciSteps(job: string): CiStep[] {
  return job
    .split(/^ {6}- name: /m)
    .slice(1)
    .map((chunk) => {
      const [name, ...rest] = chunk.split("\n");
      const runAt = rest.findIndex((line) => /^ {8}run: [|>]/.test(line));
      if (runAt === -1) return { name: name.trim(), run: undefined };
      const body: string[] = [];
      for (const line of rest.slice(runAt + 1)) {
        // A block body is indented past its `run:` key at 8 spaces; the first
        // line that is not ends it, so the prose comments above a step never
        // count.
        if (line.trim() !== "" && !/^ {9,}/.test(line)) break;
        body.push(line.trim());
      }
      return { name: name.trim(), run: body.join(" ") };
    });
}

/**
 * Every `tools/` path a shell text names, deduplicated in order. The check
 * scripts are what the steps actually run, and the arguments a step passes
 * (a runner, or an expected-failure file) are part of that. A single list per
 * step also catches a step whose script gained a `--xfail` file. It cannot
 * catch inline shell that names no `tools/` path: the standalone-binary step
 * is inline, so its own presence is checked by name instead.
 */
function toolsPaths(text: string | undefined): string[] {
  if (text === undefined) return [];
  return [...new Set(text.match(/tools\/[A-Za-z0-9_][A-Za-z0-9_./-]*/g) ?? [])];
}

test("the qbe scope runs CI's QBE toolchain steps, in the job's own order", () => {
  const steps = ciSteps(qbeJob());
  const imageAt = steps.findIndex((step) => step.name === "Build dev-qbe image");
  assert.notEqual(imageAt, -1, "the QBE job builds its own image step first");
  const jobSteps = steps.slice(imageAt + 1);

  assert.equal(
    QBE_GATE_STEPS.length,
    jobSteps.length,
    `the qbe scope has ${QBE_GATE_STEPS.length} steps, CI's QBE job has ${jobSteps.length}`,
  );
  QBE_GATE_STEPS.forEach((gateStep, i) => {
    const ciStep = jobSteps[i];
    assert.equal(gateStep.name, ciStep.name, `step ${i} is not the CI step at that position`);
    assert.deepEqual(
      toolsPaths(gateStep.script),
      toolsPaths(ciStep.run),
      `step "${ciStep.name}" runs different tools/ paths than CI`,
    );
  });
});

test("the qbe plan names the QBE image, the release preset, and every step", () => {
  const plan = gatePlan("qbe");
  assert.equal(plan.image, "loxpp-dev-qbe");
  assert.equal(GATE_IMAGES.qbe, plan.image, "the preflight image is the image the gate runs");

  const head = "set -e\ncd /workspace\ncmake --preset release -DCMAKE_CXX_FLAGS=\ncmake --build build\n";
  assert.ok(plan.script.startsWith(head), "the qbe gate builds release before it checks");

  // One build ahead of the steps covers every target CI builds per step, so a
  // step's checkpoint driver (qbe_emit_probe, qbe_emit_program) is present.
  let at = head.length;
  for (const step of QBE_GATE_STEPS) {
    const found = plan.script.indexOf(step.script, at);
    assert.notEqual(found, -1, `the qbe gate omits step "${step.name}"`);
    at = found + step.script.length;
  }
});

test("a release scope clears a cached debug sanitizer flag", () => {
  // AGENTS.md's task loop builds the debug preset first, so a release gate in
  // the same worktree sees a build/ the debug preset configured. The debug
  // preset's -fsanitize=address,undefined lives in CMAKE_CXX_FLAGS, which the
  // release preset does not set, so it survives the reconfigure; the QBE
  // program's own plain `cc` link then fails on undefined __asan_* symbols.
  for (const scope of ["backend", "qbe"] as const) {
    const configure = gatePlan(scope)
      .script.split("\n")
      .find((line) => line.startsWith("cmake --preset release"));
    assert.ok(configure, `${scope} configures the release preset`);
    assert.match(configure, /-DCMAKE_CXX_FLAGS=$/, `${scope} clears the cached sanitizer flags`);
  }
});

test("the other scopes keep the image and the differential line they had", () => {
  // The backend scope's diff line is the #855 fix: a managed backend names its
  // own runner there. Moving the scopes into a module must not drop it.
  const backend = gatePlan("backend");
  assert.equal(backend.image, "loxpp-dev-env-managed");
  assert.match(backend.script, /tools\/loxpp_jvm\.sh/);
  assert.match(backend.script, /tools\/diff_runtimes\.py/);

  const native = gatePlan("native");
  assert.equal(native.image, "loxpp-dev-env");
  assert.match(native.script, /cmake --preset debug/);
  assert.match(native.script, /ctest --test-dir build/);
});
