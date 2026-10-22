/**
 * Turn policy for the autonomous modes (/goal, /loop, /resolve-issue).
 *
 * A turn is one assistant response plus its tool calls. It is a poor cost unit:
 * one turn can carry a large command output, and a turn that only reads context
 * is cheap but necessary. The policy therefore bills three separate things:
 *
 * - gate runs: the expensive, deliberate attempt at the gate;
 * - idle turns: turns after the first change that changed nothing. A worktree
 *   that does not move is the signal, not a turn count;
 * - pre-mutation turns: reading turns. They have a generous ceiling and no
 *   other cap, because reading the issue, the decision, and the spec is the
 *   work, not a delay.
 */

export type Mode = "goal" | "loop" | "resolve-issue";

export interface Policy {
  /** Gate runs allowed before the mode stops. */
  gateBudget: number;
  /** Turns without progress, after the first change, before the mode stops. */
  idleTurnsMax: number;
  /** Turns allowed before the first change. Reading below it is free. */
  preMutationTurnsMax: number;
}

export const POLICY: Record<Mode, Policy> = {
  goal: { gateBudget: 20, idleTurnsMax: 10, preMutationTurnsMax: 40 },
  loop: { gateBudget: 12, idleTurnsMax: 10, preMutationTurnsMax: 40 },
  "resolve-issue": { gateBudget: 20, idleTurnsMax: 10, preMutationTurnsMax: 40 },
};

export interface PolicyState {
  /** A mutating tool call has happened: the mode is past the reading phase. */
  started: boolean;
  /** Turns since the last progress (tree change, new gate result, gate pass). */
  idleTurns: number;
  /** Turns counted before the first change. */
  preMutationTurns: number;
  /** loxpp_gate and loxpp_done invocations. */
  gateRuns: number;
  /** The one grace turn after a green gate at the budget was already given. */
  graceUsed: boolean;
}

export function freshPolicyState(): PolicyState {
  return { started: false, idleTurns: 0, preMutationTurns: 0, gateRuns: 0, graceUsed: false };
}

/** Fill in fields a state persisted by an older version of the extension lacks. */
export function normalizePolicyState(state: Partial<PolicyState> | undefined): PolicyState {
  return { ...freshPolicyState(), ...state };
}

export interface TurnSignals {
  /** The worktree changed, or the gate passed, or it failed in a new way. */
  progress: boolean;
  /** The last gate run passed. */
  gatePassed: boolean;
}

export type TurnDecision =
  | { stop: false; state: PolicyState; nudge: string }
  | { stop: true; state: PolicyState; reason: string };

/**
 * Decide what one finished turn does to the mode.
 *
 * @param state the state before the turn
 * @param policy caps for the active mode
 * @param signals what the turn produced
 * @param mode mode name, for messages
 */
export function decideTurn(
  state: PolicyState,
  policy: Policy,
  signals: TurnSignals,
  mode: Mode,
): TurnDecision {
  const next = { ...state };

  // Before the first change, no turn is billed against the idle cap. Only a
  // generous ceiling applies, so a read loop still ends.
  if (!next.started) next.preMutationTurns += 1;

  if (next.started) next.idleTurns = signals.progress ? 0 : next.idleTurns + 1;

  // The gate run is the expensive attempt, so it is what the attempt budget
  // counts, in both phases. A turn that only reads costs nothing here.
  if (next.gateRuns >= policy.gateBudget) {
    if (signals.gatePassed && !next.graceUsed) {
      next.graceUsed = true;
      return {
        stop: false,
        state: next,
        nudge: `[${mode}] the gate is green after ${next.gateRuns} gate runs. Call loxpp_done now; the attempt budget allows no more gate runs.`,
      };
    }
    return {
      stop: true,
      state: next,
      reason: `${mode} mode stopped after ${policy.gateBudget} gate runs without completion. Last gate: ${
        signals.gatePassed ? "green, but loxpp_done was not called" : "failed"
      }. Continue by hand or re-run the mode.`,
    };
  }

  if (!next.started) {
    if (next.preMutationTurns >= policy.preMutationTurnsMax) {
      return {
        stop: true,
        state: next,
        reason: `${mode} mode stopped: no file changed in ${policy.preMutationTurnsMax} turns. State what blocks the first change, then re-run the mode.`,
      };
    }
    return {
      stop: false,
      state: next,
      nudge: `[${mode}] turn ${next.preMutationTurns}: nothing changed yet. Keep reading if you need to; run loxpp_gate once you have a change.`,
    };
  }

  // After the first change, a run of turns that leaves the worktree and the
  // gate output unchanged means the mode is stuck, not careful.
  if (next.idleTurns >= policy.idleTurnsMax) {
    return {
      stop: true,
      state: next,
      reason: `${mode} mode stopped: ${policy.idleTurnsMax} turns in a row changed no file and produced no new gate result. Report the blocker, then re-run the mode.`,
    };
  }

  return {
    stop: false,
    state: next,
    nudge: signals.progress
      ? `[${mode}] ${next.gateRuns}/${policy.gateBudget} gate runs: progress. Continue; run loxpp_gate when the change is ready.`
      : `[${mode}] ${next.gateRuns}/${policy.gateBudget} gate runs, ${next.idleTurns}/${policy.idleTurnsMax} turns without a change. Confirm the change, then run loxpp_gate.`,
  };
}
