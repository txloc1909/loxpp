/**
 * Tests for the mode turn policy. Run with:
 *   node --test .pi/extensions/mode-policy.test.ts
 *
 * Each test states one property the policy must hold, so a regression in one
 * guard fails one test by name.
 */

import assert from "node:assert/strict";
import test from "node:test";

import {
  POLICY,
  decideTurn,
  freshPolicyState,
  normalizePolicyState,
  type Mode,
  type PolicyState,
} from "./mode-policy.ts";

const MODE: Mode = "loop";
const POLICY_UNDER_TEST = POLICY[MODE];

/** Drive `turns` turns, each yielding the given signals, and return the trace. */
function run(
  state: PolicyState,
  turns: { progress: boolean; gatePassed: boolean }[],
  policy = POLICY_UNDER_TEST,
) {
  const decisions = [];
  for (const signals of turns) {
    const d = decideTurn(state, policy, signals, MODE);
    state = d.state;
    decisions.push(d);
    if (d.stop) break;
  }
  return { state, decisions };
}

const progress = { progress: true, gatePassed: false };
const idle = { progress: false, gatePassed: false };
const green = { progress: true, gatePassed: true };

test("reading turns are free: a long read phase never stops the mode", () => {
  const { state, decisions } = run(freshPolicyState(), Array(30).fill(idle));
  assert.equal(decisions.filter((d) => d.stop).length, 0);
  assert.equal(state.preMutationTurns, 30);
  assert.equal(state.idleTurns, 0, "the idle cap must not bill reading turns");
});

test("the read phase still has a ceiling, so a read loop ends", () => {
  const { decisions } = run(freshPolicyState(), Array(POLICY_UNDER_TEST.preMutationTurnsMax + 5).fill(idle));
  const stop = decisions.at(-1);
  assert.ok(stop && stop.stop, "the pre-mutation ceiling must stop the mode");
  assert.match(stop.reason, /no file changed in \d+ turns/);
});

test("after the first change, ten turns with no change stop the mode", () => {
  const started = { ...freshPolicyState(), started: true };
  const { decisions } = run(started, Array(POLICY_UNDER_TEST.idleTurnsMax + 5).fill(idle));
  const stop = decisions.at(-1);
  assert.ok(stop && stop.stop, "idle turns must stop the mode");
  assert.match(stop.reason, /changed no file and produced no new gate result/);
  assert.equal(decisions.length, POLICY_UNDER_TEST.idleTurnsMax, "stop on the limit, not after it");
});

test("one progress turn resets the idle count", () => {
  const started = { ...freshPolicyState(), started: true };
  const turns = [
    ...Array(POLICY_UNDER_TEST.idleTurnsMax - 1).fill(idle),
    progress,
    ...Array(POLICY_UNDER_TEST.idleTurnsMax - 1).fill(idle),
  ];
  const { decisions } = run(started, turns);
  assert.equal(decisions.filter((d) => d.stop).length, 0, "progress must reset the idle counter");
});

test("the attempt budget counts gate runs, not turns", () => {
  // Two turns per gate run: a turn budget of 12 would stop after 6 gate runs.
  // The gate-run budget must allow all 12, so this test fails if the counter
  // goes back to counting turns.
  let state = { ...freshPolicyState(), started: true };
  const decisions = [];
  for (let i = 0; i < 2 * (POLICY_UNDER_TEST.gateBudget + 1); i += 1) {
    const gateRunNow = i % 2 === 1;
    if (gateRunNow) state = { ...state, gateRuns: state.gateRuns + 1 };
    const d = decideTurn(state, POLICY_UNDER_TEST, gateRunNow ? progress : idle, MODE);
    state = d.state;
    decisions.push(d);
    if (d.stop) break;
  }
  const stop = decisions.at(-1);
  assert.ok(stop && stop.stop, "the gate-run budget must stop the mode");
  assert.match(stop.reason, new RegExp(`after ${POLICY_UNDER_TEST.gateBudget} gate runs`));
  assert.equal(state.gateRuns, POLICY_UNDER_TEST.gateBudget);
  assert.ok(decisions.length > POLICY_UNDER_TEST.gateBudget, "the run must outlive a turn-count budget");
  assert.ok(
    decisions.length > POLICY_UNDER_TEST.idleTurnsMax,
    "gate runs count as progress, so the idle cap must not fire first",
  );
});

test("a green gate at the budget gets one grace turn to call loxpp_done", () => {
  const atBudget = { ...freshPolicyState(), started: true, gateRuns: POLICY_UNDER_TEST.gateBudget };
  const grace = decideTurn(atBudget, POLICY_UNDER_TEST, green, MODE);
  assert.equal(grace.stop, false);
  assert.match(grace.stop === false ? grace.nudge : "", /Call loxpp_done now/);

  const after = decideTurn(grace.state, POLICY_UNDER_TEST, green, MODE);
  assert.equal(after.stop, true, "the grace turn is given once");
});

test("a failed gate at the budget stops without a grace turn", () => {
  const atBudget = { ...freshPolicyState(), started: true, gateRuns: POLICY_UNDER_TEST.gateBudget };
  const d = decideTurn(atBudget, POLICY_UNDER_TEST, progress, MODE);
  assert.equal(d.stop, true);
});

test("the attempt budget also bounds gate runs before the first change", () => {
  // A gate loop with no edit is the same runaway as an edit loop, so the gate
  // budget applies in the reading phase too.
  const reading = { ...freshPolicyState(), gateRuns: POLICY_UNDER_TEST.gateBudget };
  const d = decideTurn(reading, POLICY_UNDER_TEST, progress, MODE);
  assert.equal(d.stop, true);
  assert.match(d.stop === true ? d.reason : "", /after \d+ gate runs/);
});

test("done retries have their own budget and do not spend gate attempts", () => {
  // A waiting CI run costs done attempts, not fix attempts.
  const policy = { ...POLICY_UNDER_TEST, doneBudget: 3 };
  let state = { ...freshPolicyState(), started: true };
  const decisions = [];
  for (let i = 0; i < 6; i += 1) {
    state = { ...state, doneRuns: state.doneRuns + 1 };
    const d = decideTurn(state, policy, progress, MODE);
    state = d.state;
    decisions.push(d);
    if (d.stop) break;
  }
  const stop = decisions.at(-1);
  assert.ok(stop && stop.stop, "the done budget must stop the mode");
  assert.match(stop.reason, /loxpp_done attempts/);
  assert.equal(state.gateRuns, 0, "a done attempt is not a gate attempt");
  assert.equal(state.doneRuns, policy.doneBudget);
});

test("the total-turn ceiling bounds a mode that keeps making progress", () => {
  // Progress on every turn resets the idle counter and never runs the gate, so
  // only the total-turn backstop bounds it.
  const policy = { ...POLICY_UNDER_TEST, totalTurnsMax: 5 };
  const { decisions } = run({ ...freshPolicyState(), started: true }, Array(20).fill(progress), policy);
  const stop = decisions.at(-1);
  assert.ok(stop && stop.stop, "the total-turn ceiling must stop the mode");
  assert.match(stop.reason, /stopped at 5 turns/);
  assert.equal(decisions.length, policy.totalTurnsMax, "stop at the ceiling, not after it");
});

test("the progress nudge reports done attempts once they happen", () => {
  const d = decideTurn({ ...freshPolicyState(), started: true, doneRuns: 2 }, POLICY_UNDER_TEST, progress, MODE);
  assert.equal(d.stop, false);
  assert.match(d.stop === false ? d.nudge : "", /2\/\d+ done attempts/);
});

test("state from an older session restores with defaults", () => {
  const restored = normalizePolicyState({ started: true } as Partial<PolicyState>);
  assert.equal(restored.gateRuns, 0);
  assert.equal(restored.doneRuns, 0);
  assert.equal(restored.turns, 0);
  assert.equal(restored.idleTurns, 0);
  assert.equal(restored.graceUsed, false);
  assert.equal(normalizePolicyState(undefined).started, false);
});
