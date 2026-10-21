/**
 * Lox++ autonomous modes + mission roles.
 *
 * Hand-off modes (work to completion, no approval wait):
 *   /loop [goal]          fix-until-green; ends via loxpp_done after a green gate
 *   /resolve-issue <issue>  implementer role for one mission node (multi-agent-playbook)
 *
 * /goal has an approval gate by default, per AGENTS.md's planning policy:
 *   /goal <goal>              plan first, read-only, wait for /approve
 *   /goal --handoff <goal>    hand the approval decision to the agent and go
 *   /approve                  approve the plan and begin implementation
 *
 * Single-shot roles (inject a role prompt, run one turn):
 *   /review <subject>   reviewer: adversarial, reproduce the checkpoint, tag [Reviewer]
 *   /referee <subject>  researcher/referee: verify facts, rule, bound next round, [Researcher]
 *
 * Run-observation:
 *   /mission-status <issue>   show plan_resume.py + status.py output
 *
 * Tools:
 *   loxpp_gate     build/test gate, run inside the dev container
 *   loxpp_done     verify the done criteria (gate + optional CI + review) and end
 *   loxpp_issue    read a GitHub issue with the full --json --template form
 *   loxpp_pr       read a PR with the full form (body + comments + reviews)
 *
 * loxpp_done is the single termination point. It always re-runs the gate, so a
 * stale green gate can never end a mode, and it widens "done" beyond a local
 * green gate to CI status and reviewer approval when those criteria are named.
 * The turn_end continuation is bounded by a per-mode budget.
 */

import { spawn } from "node:child_process";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { StringEnum } from "@earendil-works/pi-ai";
import { Type } from "typebox";

// Human opt-in switch. false (default) keeps the /goal approval gate; true hands
// the approval decision to the agent for /goal. A per-command --handoff /
// --no-handoff flag overrides this for one invocation.
const HANDOFF = false;

const REPO = "txloc1909/loxpp";
const GATE_OUTPUT_CAP = 8000;
const READ_OUTPUT_CAP = 12000;
const STATUS_OUTPUT_CAP = 2000;

// Per-mode iteration budget. Goal/resolve-issue implement a feature and need more
// room than a fix-until-green loop.
const BUDGET: Record<"goal" | "loop" | "resolve-issue", number> = {
  goal: 20,
  loop: 12,
  "resolve-issue": 20,
};

// The gate always runs inside the dev container: AGENTS.md's dev model puts the
// agent on the host and program execution in the container, where the toolchain
// lives. The checkout (cwd) is mounted at /workspace and the task-loop commands
// run there.
const NATIVE_CONTAINER_GATE = `set -e
cd /workspace
cmake --preset debug
cmake --build build
ctest --test-dir build --output-on-failure -j$(nproc)`;

const BACKEND_CONTAINER_GATE = `set -e
cd /workspace
tools/build_lox_rt.sh
tools/check_managed_toolchains.sh
cmake --preset release
cmake --build build
ctest --test-dir build --output-on-failure -j$(nproc)
tools/diff_runtimes.py`;

// The exact template forms multi-agent-playbook.md requires. Plain `gh issue
// view` hides comment hazards; `--comments` drops the body. The tool hardcodes
// the correct form so the model cannot get either one wrong.
const ISSUE_TPL =
  `{{.title}}{{"\\n\\n"}}{{.body}}{{"\\n\\n=== comments: "}}{{len .comments}}{{"\\n"}}` +
  `{{range .comments}}{{"\\n--- "}}{{.author.login}} {{.createdAt}}{{"\\n"}}{{.body}}{{"\\n"}}{{end}}`;
const PR_TPL =
  ISSUE_TPL +
  `{{"\\n=== reviews: "}}{{len .reviews}}{{"\\n"}}` +
  `{{range .reviews}}{{"\\n--- "}}{{.author.login}} {{.createdAt}} {{.state}}{{"\\n"}}{{.body}}{{"\\n"}}{{end}}`;

const PLANNING_PROMPT = `Work in plan-first mode. Do not edit files, commit, push, or open a PR.

1. Read the binding context first:
   - VISION.md — what Lox++ is for; test every plan against it.
   - spec/ — canonical semantics (the spec wins over the implementation).
   - docs/decisions/ — the why behind past decisions; avoid conflicting designs.
   - docs/README.md — document tiers and where a new document goes.
2. If the goal touches the spec, the bytecode, or the standard library surface,
   list every consumer before any work: the native VM, the JVM backend, the QBE
   backend, the bootstrap interpreter, and every tool that parses or lowers the
   construct (resolver, LSP, tree-sitter grammar, editor syntaxes). For each
   backend, state what the change owes it under the roles in VISION.md.
3. If the spec names a countable set (fault kinds, opcodes, exit paths, suspend
   points), write it as a table and name a script that checks the table is
   complete.
4. Write the plan and post it as an issue comment or PR description. Open the
   issue first if the work does not already have one.
5. Wait for explicit approval. Do not write code or open a PR until the owner
   approves. If the plan changes, acknowledge the revision before proceeding.

Answer with the plan only. The human runs /approve when the plan is accepted.`;

const GOAL_WORKING_PROMPT = (goal: string) => `[GOAL MODE — implement]\n\nGoal: ${goal}\n\nThe plan is approved. Implement it now, per AGENTS.md.\n\n1. Add a test or probe for the goal's behavior and connect it to ctest (AGENTS.md engineering rules).\n2. Implement, then run loxpp_gate (it builds and tests inside the dev container) until it passes.\n3. Commit when the code compiles and push after each commit (AGENTS.md). Conventional commits; split if the subject would say "and".\n4. Open a PR when the work is ready for review.\n5. Done means more than a local green gate. When you open a PR, call loxpp_done with the PR number and criteria ci+review, so CI green and reviewer approval are also required.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

const GOAL_HANDOFF_PROMPT = (goal: string) => `[GOAL MODE — hand-off]\n\nGoal: ${goal}\n\nThe human handed the approval decision to you. Plan, implement, and finish without waiting.\n\n1. Plan: read the binding context (VISION.md, spec/, docs/decisions/, docs/README.md). If this is a language change, list every consumer first per AGENTS.md. Keep the plan as your working notes, not a waiting step.\n2. Add a test or probe for the goal's behavior and connect it to ctest (AGENTS.md engineering rules).\n3. Implement, then run loxpp_gate (it builds and tests inside the dev container) until it passes.\n4. Commit when the code compiles and push after each commit (AGENTS.md). Conventional commits; split if the subject would say "and".\n5. Open a PR when the work is ready for review.\n6. Done means more than a local green gate. When you open a PR, call loxpp_done with the PR number and criteria ci+review, so CI green and reviewer approval are also required.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

const LOOP_PROMPT = (goal: string) => `[LOOP MODE — hand-off]\n\nGoal: ${goal}\n\nWork in a tight loop until the gate passes:\n1. Make one focused change (or fix the latest failure).\n2. Call loxpp_gate (it builds and tests inside the dev container).\n3. If it fails, read the tail of the output, fix the smallest thing that unblocks it, and repeat.\n4. When the gate is green, call loxpp_done to finish. If the work is on a PR, pass the PR number and criteria ci+review, so the loop does not stop before CI is green and the reviewer approved.\n\nCommit when the code compiles and push after each commit, per AGENTS.md. Do not ask to stop.`;

const NODE_PROMPT = (n: string) => `[RESOLVE-ISSUE MODE — implementer, hand-off]\n\nNode issue: #${n}\n\n1. Read the node spec with loxpp_issue (full form — a hazard may live in a comment).\n2. Read docs/process/multi-agent-playbook.md, the decision in docs/decisions/ this node serves, and the spec/reference docs the issue names.\n3. Create a worktree and branch per AGENTS.md's task loop. Pass the checkout you work in to loxpp_gate and loxpp_done via their cwd argument, so the gate builds and tests that tree inside the dev container, not the main checkout.\n4. Implement the deliverable; respect the issue's Scope section — out-of-scope cases fail loudly and name this node.\n5. Checkpoint: rebuild fully, then run loxpp_gate until green (ctest does not rebuild).\n6. Commit atomically with conventional commits; push after each commit.\n7. Open a PR whose body contains \`Closes #${n}\`, and tag yourself \`[Implementer]\`. Do not poll CI.\n8. Call loxpp_done with the PR number and criteria ci+review. Done requires the local gate green, CI green, and the reviewer approved.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

interface AutonomyState {
  mode: "goal" | "loop" | "resolve-issue" | null;
  iterations: number;
  lastGatePassed: boolean | undefined;
  lastScope: "native" | "backend";
  lastCwd: string | undefined;
  declaredDone: boolean;
}

interface PlanState {
  active: boolean;
  goal: string;
  toolsBefore?: string[];
}

interface PersistedState {
  autonomy: AutonomyState;
  plan: PlanState;
}

function tail(text: string, cap: number): string {
  const bytes = Buffer.byteLength(text, "utf8");
  if (bytes <= cap) return text;
  let out = text.slice(-cap);
  while (Buffer.byteLength(out, "utf8") > cap) out = out.slice(1);
  return `[truncated: ${bytes} bytes, showing tail]\n\n${out}`;
}

function collect(
  proc: ReturnType<typeof spawn>,
  signal: AbortSignal | undefined,
): Promise<{ code: number; output: string }> {
  return new Promise((resolve) => {
    let output = "";
    proc.stdout.on("data", (d) => {
      output += d.toString();
    });
    proc.stderr.on("data", (d) => {
      output += d.toString();
    });
    proc.on("close", (code) => resolve({ code: code ?? 1, output }));
    proc.on("error", (err) => resolve({ code: 1, output: String(err) }));
    if (signal) {
      const abort = () => proc.kill("SIGTERM");
      if (signal.aborted) abort();
      else signal.addEventListener("abort", abort, { once: true });
    }
  });
}

// argv spawn (no shell) for gh and python: model-supplied values stay arguments,
// so they cannot be interpreted as shell syntax.
function runArgv(argv: string[], cwd: string, signal: AbortSignal | undefined) {
  return collect(spawn(argv[0], argv.slice(1), { cwd }), signal);
}

// The agent runs on the host; program execution runs inside the dev container
// (AGENTS.md), so the gate always enters the container via podman.
async function runGate(
  scope: "native" | "backend",
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ passed: boolean; output: string }> {
  const image = scope === "backend" ? "loxpp-dev-env-managed" : "loxpp-dev-env";
  const script = scope === "backend" ? BACKEND_CONTAINER_GATE : NATIVE_CONTAINER_GATE;
  const argv = [
    "podman", "run", "--rm",
    "-v", `${cwd}:/workspace:z`,
    "-v", "loxpp-ccache:/ccache",
    image,
    "bash", "-lc", script,
  ];
  const r = await runArgv(argv, cwd, signal);
  return { passed: r.code === 0, output: tail(r.output, GATE_OUTPUT_CAP) };
}

async function ciGreen(
  pr: string,
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ ok: boolean; output: string }> {
  const r = await runArgv(["gh", "pr", "checks", pr, "--repo", REPO], cwd, signal);
  return { ok: r.code === 0, output: r.output };
}

async function reviewApproved(
  pr: string,
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ ok: boolean; output: string }> {
  const r = await runArgv(["gh", "pr", "view", pr, "--repo", REPO, "--json", "reviews,comments"], cwd, signal);
  if (r.code !== 0) return { ok: false, output: r.output };
  try {
    const data = JSON.parse(r.output);
    const bodies: string[] = [
      ...(((data.reviews as { body?: string }[]) ?? []).map((x) => x.body ?? "")),
      ...(((data.comments as { body?: string }[]) ?? []).map((x) => x.body ?? "")),
    ];
    const ok = bodies.some((b) => (b.split("\n")[0] ?? "").trim() === "[Reviewer] APPROVED");
    return { ok, output: ok ? "reviewer approved" : "no [Reviewer] APPROVED comment" };
  } catch {
    return { ok: false, output: r.output };
  }
}

function issueArgs(number: string, repo: string): string[] {
  return ["gh", "issue", "view", number, "--repo", repo, "--json", "title,body,comments", "--template", ISSUE_TPL];
}

function prArgs(number: string, repo: string): string[] {
  return ["gh", "pr", "view", number, "--repo", repo, "--json", "title,body,comments,reviews", "--template", PR_TPL];
}

// Denylist for the /goal plan phase: it must still permit building, running the
// interpreter, and posting the plan (gh issue/comment). These are the verbs that
// mutate the repo, git history, or the PR surface before approval.
function isMutatingCommand(command: string): boolean {
  const c = command.trim();
  if (/^git\s+(add|commit|push|merge|rebase|reset|stash|checkout|switch|branch)\b/.test(c)) return true;
  if (/^gh\s+pr\s+(create|merge|reopen|close)\b/.test(c)) return true;
  if (/^gh\s+issue\s+(edit|close|reopen|delete)\b/.test(c)) return true;
  if (/^(rm|mv|cp|touch|mkdir|rmdir|tee)\b/.test(c)) return true;
  if (/^(sed|perl)\s+-i\b/.test(c)) return true;
  if (/^(vim|nano|nvim|code|emacs)\b/.test(c)) return true;
  return false;
}

function parseHandoff(args: string): { handoff: boolean; rest: string } {
  const parts = args.split(/\s+/).filter(Boolean);
  let handoff = HANDOFF;
  if (parts.includes("--handoff")) handoff = true;
  if (parts.includes("--no-handoff")) handoff = false;
  const rest = parts.filter((p) => p !== "--handoff" && p !== "--no-handoff").join(" ");
  return { handoff, rest };
}

export default function loxppModes(pi: ExtensionAPI): void {
  let autonomy: AutonomyState = {
    mode: null,
    iterations: 0,
    lastGatePassed: undefined,
    lastScope: "native",
    lastCwd: undefined,
    declaredDone: false,
  };
  let plan: PlanState = { active: false, goal: "", toolsBefore: undefined };

  const unique = (names: string[]): string[] => [...new Set(names)];

  function persist(): void {
    pi.appendEntry("loxpp-modes", { autonomy, plan } satisfies PersistedState);
  }

  function notify(ctx: ExtensionContext, text: string, kind: "info" | "error" = "info"): void {
    if (ctx.hasUI) ctx.ui.notify(text, kind);
  }

  function startWorking(mode: "goal" | "loop" | "resolve-issue", prompt: string): void {
    autonomy = {
      mode,
      iterations: 0,
      lastGatePassed: undefined,
      lastScope: "native",
      lastCwd: undefined,
      declaredDone: false,
    };
    persist();
    pi.sendUserMessage(prompt);
  }

  function enterPlanMode(goal: string): void {
    plan = { active: true, goal, toolsBefore: pi.getActiveTools() };
    pi.setActiveTools(unique([...pi.getActiveTools().filter((t) => t !== "edit" && t !== "write"), "read", "bash"]));
    persist();
    pi.sendUserMessage(`[GOAL MODE — plan first]\n\nGoal: ${goal}\n\n${PLANNING_PROMPT}`);
  }

  // ---- tools -----------------------------------------------------------

  pi.registerTool({
    name: "loxpp_gate",
    label: "Lox++ gate",
    description:
      "Run the Lox++ build/test gate and report pass or fail. Call it after each change. Modes keep running until loxpp_done confirms a green gate.",
    promptSnippet: "run the Lox++ build/test gate and report pass/fail",
    parameters: Type.Object({
      scope: Type.Optional(
        StringEnum(["native", "backend"] as const, {
          description:
            "native = debug build + ctest. backend = managed toolchains + release build + ctest + diff_runtimes. Default native.",
          default: "native",
        }),
      ),
      cwd: Type.Optional(
        Type.String({ description: "Checkout or worktree to gate. Default the session working directory." }),
      ),
    }),
    async execute(_id, params, signal, _onUpdate, ctx) {
      const scope: "native" | "backend" = params.scope ?? "native";
      const cwd = params.cwd ?? ctx.cwd;
      const r = await runGate(scope, cwd, signal);
      autonomy.lastGatePassed = r.passed;
      autonomy.lastScope = scope;
      autonomy.lastCwd = cwd;
      persist();
      return {
        content: [
          {
            type: "text",
            text: r.passed ? "GATE PASSED: build and tests are green." : `GATE FAILED.\n\n${r.output}`,
          },
        ],
        details: { passed: r.passed, scope, cwd },
      };
    },
  });

  pi.registerTool({
    name: "loxpp_done",
    label: "Declare done",
    description:
      "Verify the done criteria and end the mode. Always re-runs the local gate (inside the dev container); optionally also requires CI green and reviewer approval when those criteria are named.",
    promptSnippet: "verify the done criteria (gate, CI, review) and end the mode",
    parameters: Type.Object({
      summary: Type.String({ description: "What was done and what changed." }),
      pr: Type.Optional(Type.String({ description: "PR number, required when criteria includes ci or review." })),
      scope: Type.Optional(
        StringEnum(["native", "backend"] as const, {
          description: "Gate scope to re-run. Default the scope of the last loxpp_gate call.",
        }),
      ),
      cwd: Type.Optional(Type.String({ description: "Checkout to gate. Default the last loxpp_gate cwd." })),
      criteria: Type.Optional(
        Type.Array(
          StringEnum(["ci", "review"] as const, {
            description: "An extra done criterion beyond the local gate.",
          }),
          { description: "Extra done criteria. ci = CI checks green; review = [Reviewer] APPROVED present. The local gate is always checked." },
        ),
      ),
    }),
    async execute(_id, params, signal, _onUpdate, ctx) {
      const scope: "native" | "backend" = params.scope ?? autonomy.lastScope ?? "native";
      const cwd = params.cwd ?? autonomy.lastCwd ?? ctx.cwd;
      const criteria: Array<"ci" | "review"> = params.criteria ?? [];
      const pr = params.pr?.trim();

      if ((criteria.includes("ci") || criteria.includes("review")) && !pr) {
        return {
          isError: true,
          content: [{ type: "text", text: "Not done: the ci/review criteria need a PR number. Pass pr." }],
          details: { done: false },
        };
      }

      const failures: string[] = [];
      const r = await runGate(scope, cwd, signal);
      if (!r.passed) {
        failures.push(`gate not green:\n${r.output}`);
      } else {
        if (criteria.includes("ci")) {
          const ci = await ciGreen(pr!, cwd, signal);
          if (!ci.ok) failures.push(`CI not green:\n${ci.output}`);
        }
        if (criteria.includes("review")) {
          const rv = await reviewApproved(pr!, cwd, signal);
          if (!rv.ok) failures.push(`review not approved: ${rv.output}`);
        }
      }

      if (failures.length > 0) {
        return {
          isError: true,
          content: [{ type: "text", text: `Not done:\n- ${failures.join("\n- ")}` }],
          details: { done: false, scope, cwd },
        };
      }

      autonomy.declaredDone = true;
      autonomy.lastGatePassed = true;
      autonomy.lastScope = scope;
      autonomy.lastCwd = cwd;
      persist();
      const extra = criteria.length > 0 ? ` (criteria: gate, ${criteria.join(", ")})` : " (gate green)";
      return {
        content: [
          {
            type: "text",
            text: `Recorded completion${extra}.\n\n${params.summary}${params.pr ? `\n\nPR: ${params.pr}` : ""}`,
          },
        ],
        details: { done: true, pr: params.pr, scope, cwd },
      };
    },
  });

  pi.registerTool({
    name: "loxpp_issue",
    label: "Read issue",
    description:
      "Read a GitHub issue (title, body, every comment) with the full --json --template form required by docs/process/multi-agent-playbook.md. Use this, never a plain gh issue view.",
    promptSnippet: "read a GitHub issue with the full title+body+comments form",
    parameters: Type.Object({
      number: Type.String({ description: "Issue number." }),
      repo: Type.Optional(Type.String({ description: `Repo, default ${REPO}.` })),
    }),
    async execute(_id, params, signal, _onUpdate, ctx) {
      if (!/^\d+$/.test(params.number)) {
        return {
          isError: true,
          content: [{ type: "text", text: `Invalid issue number: ${params.number}` }],
          details: { number: params.number },
        };
      }
      const r = await runArgv(issueArgs(params.number, params.repo ?? REPO), ctx.cwd, signal);
      if (r.code !== 0) {
        return {
          isError: true,
          content: [{ type: "text", text: `gh issue view failed:\n${r.output}` }],
          details: { number: params.number },
        };
      }
      return {
        content: [{ type: "text", text: tail(r.output, READ_OUTPUT_CAP) }],
        details: { number: params.number },
      };
    },
  });

  pi.registerTool({
    name: "loxpp_pr",
    label: "Read PR",
    description:
      "Read a PR (title, body, comments, reviews) with the full --json --template form required by docs/process/multi-agent-playbook.md.",
    promptSnippet: "read a PR with the full title+body+comments+reviews form",
    parameters: Type.Object({
      number: Type.String({ description: "PR number." }),
      repo: Type.Optional(Type.String({ description: `Repo, default ${REPO}.` })),
    }),
    async execute(_id, params, signal, _onUpdate, ctx) {
      if (!/^\d+$/.test(params.number)) {
        return {
          isError: true,
          content: [{ type: "text", text: `Invalid PR number: ${params.number}` }],
          details: { number: params.number },
        };
      }
      const r = await runArgv(prArgs(params.number, params.repo ?? REPO), ctx.cwd, signal);
      if (r.code !== 0) {
        return {
          isError: true,
          content: [{ type: "text", text: `gh pr view failed:\n${r.output}` }],
          details: { number: params.number },
        };
      }
      return {
        content: [{ type: "text", text: tail(r.output, READ_OUTPUT_CAP) }],
        details: { number: params.number },
      };
    },
  });

  // ---- commands ----------------------------------------------------------

  pi.registerCommand("goal", {
    description: "Plan-first goal (approval gate) unless --handoff hands the decision to the agent",
    handler: async (args, ctx) => {
      const { handoff, rest } = parseHandoff(args);
      const goal = rest.trim();
      if (!goal) {
        notify(ctx, "Usage: /goal [--handoff|--no-handoff] <goal>", "error");
        return;
      }
      if (handoff) {
        startWorking("goal", GOAL_HANDOFF_PROMPT(goal));
        notify(ctx, "Goal mode (hand-off): working to completion.", "info");
      } else {
        enterPlanMode(goal);
        notify(ctx, "Goal mode: plan first. Approve with /approve.", "info");
      }
    },
  });

  pi.registerCommand("approve", {
    description: "Approve the plan from /goal and begin implementation",
    handler: async (_args, ctx) => {
      if (!plan.active) {
        notify(ctx, "No plan awaits approval.", "info");
        return;
      }
      const goal = plan.goal;
      const toolsBefore = plan.toolsBefore;
      plan = { active: false, goal: "", toolsBefore: undefined };
      pi.setActiveTools(toolsBefore ?? pi.getActiveTools());
      persist();
      notify(ctx, "Plan approved — implementing.", "info");
      startWorking("goal", GOAL_WORKING_PROMPT(goal));
    },
  });

  pi.registerCommand("loop", {
    description: "Autonomous fix-until-green loop over the Lox++ gate",
    handler: async (args, ctx) => {
      const goal = args.trim() || "make the tree green";
      startWorking("loop", LOOP_PROMPT(goal));
      notify(ctx, "Loop mode on. /stop to exit.", "info");
    },
  });

  pi.registerCommand("resolve-issue", {
    description: "Implementer mode for one mission node (hand-off)",
    handler: async (args, ctx) => {
      const n = args.trim().replace(/^#/, "");
      if (!/^\d+$/.test(n)) {
        notify(ctx, "Usage: /resolve-issue <issue-number>", "error");
        return;
      }
      startWorking("resolve-issue", NODE_PROMPT(n));
      notify(ctx, `Implementer mode on for #${n}. /stop to exit.`, "info");
    },
  });

  pi.registerCommand("stop", {
    description: "Exit any active mode (goal/loop/resolve-issue)",
    handler: async (_args, ctx) => {
      if (!autonomy.mode) {
        notify(ctx, "No mode is active.", "info");
        return;
      }
      autonomy.mode = null;
      persist();
      notify(ctx, "Mode off.", "info");
    },
  });

  // ---- single-shot role commands -----------------------------------------

  pi.registerCommand("review", {
    description: "Reviewer mode: adversarially review a PR or node",
    handler: async (args, ctx) => {
      const subject = args.trim();
      if (!subject) {
        notify(ctx, "Usage: /review <pr-or-issue>", "error");
        return;
      }
      pi.sendUserMessage(
        `[REVIEW MODE — reviewer]\n\nSubject: ${subject}\n\n1. Read the PR with loxpp_pr (full form) and the linked node issue with loxpp_issue.\n2. Reproduce the checkpoint in your own worktree; run the programs, do not only read the diff.\n3. Review adversarially: assume the change is wrong until you prove it right.\n4. Report findings as PR comments, each tagged \`[Reviewer]\`. Do not fix code yourself.\n5. Approve only with a PR comment whose first line is exactly: \`[Reviewer] APPROVED\``,
      );
    },
  });

  pi.registerCommand("referee", {
    description: "Researcher/referee mode: verify facts, rule, bound the next round",
    handler: async (args, ctx) => {
      const subject = args.trim();
      if (!subject) {
        notify(ctx, "Usage: /referee <issue>", "error");
        return;
      }
      pi.sendUserMessage(
        `[REFEREE MODE — researcher]\n\nIssue: ${subject}\n\n1. Read docs/records/referee-rulings-*.md first.\n2. Verify the facts yourself: read spec/ and src/, run the code. Take neither side's word.\n3. Rule: assign the tag to the implementer, the reviewer, or a third option.\n4. Bound the next round: state the exact action that must now happen, so the loop cannot repeat.\n\nTag your comment \`[Researcher]\`.`,
      );
    },
  });

  // ---- run-observation -----------------------------------------------------

  pi.registerCommand("mission-status", {
    description: "Show mission state (plan_resume.py + status.py)",
    handler: async (args, ctx) => {
      const n = args.trim().replace(/^#/, "");
      if (!/^\d+$/.test(n)) {
        notify(ctx, "Usage: /mission-status <tracking-issue-number>", "error");
        return;
      }
      let all = "";
      const argv1 = ["python3", "tools/agent-workflow/plan_resume.py", n];
      const argv2 = ["python3", "tools/agent-workflow/status.py"];
      for (const argv of [argv1, argv2]) {
        const r = await runArgv(argv, ctx.cwd, undefined);
        all += `\n$ ${argv.join(" ")}\n${r.output}`;
      }
      pi.sendMessage({ customType: "mission-status", content: tail(all, STATUS_OUTPUT_CAP), display: true });
      notify(ctx, "Mission status written to the transcript.", "info");
    },
  });

  // ---- engine ---------------------------------------------------------------

  // The /goal plan phase blocks writes; the working modes rely on loxpp_done to
  // re-verify the gate, so no edit/write invalidation is needed here.
  pi.on("tool_call", async (event) => {
    if (!plan.active) return;
    if (event.toolName === "edit" || event.toolName === "write") {
      return {
        block: true,
        reason: "Goal plan mode: file edits are blocked until the plan is approved (/approve).",
      };
    }
    if (event.toolName === "bash") {
      const command = (event.input.command as string) ?? "";
      if (isMutatingCommand(command)) {
        return { block: true, reason: `Goal plan mode: mutating command blocked.\nCommand: ${command}` };
      }
    }
  });

  pi.on("turn_end", async () => {
    const mode = autonomy.mode;
    if (!mode) return;
    if (autonomy.declaredDone) {
      autonomy.mode = null;
      persist();
      return;
    }
    autonomy.iterations += 1;

    const budget = BUDGET[mode];
    if (autonomy.iterations >= budget) {
      autonomy.mode = null;
      persist();
      return {
        entries: [
          {
            type: "custom_message",
            customType: "mode-limit",
            content: `${mode} mode stopped after ${budget} iterations without completion. Continue by hand or re-run the mode.`,
            display: true,
          },
        ],
      };
    }

    const nudge =
      autonomy.lastGatePassed === true
        ? `[${mode} mode] ${autonomy.iterations}/${budget}: the local gate is green. Verify any remaining done criteria (CI, review) and call loxpp_done.`
        : autonomy.lastGatePassed === false
          ? `[${mode} mode] ${autonomy.iterations}/${budget}: the gate failed. Read the latest failure output, fix it, then call loxpp_gate again.`
          : `[${mode} mode] ${autonomy.iterations}/${budget}: make a change and run loxpp_gate.`;
    persist();
    return {
      entries: [{ type: "custom_message", customType: "mode-continue", content: nudge, display: true }],
      continue: true,
    };
  });

  // ---- restore on resume -----------------------------------------------------

  pi.on("session_start", async (_event, ctx) => {
    const entries = ctx.sessionManager.getBranch();
    const last = [...entries]
      .reverse()
      .find(
        (e: { type: string; customType?: string }) =>
          e.type === "custom" && e.customType === "loxpp-modes",
      ) as { data?: PersistedState } | undefined;
    if (last?.data) {
      autonomy = { ...autonomy, ...last.data.autonomy };
      plan = { ...plan, ...last.data.plan };
    }
    if (plan.active) {
      pi.setActiveTools(unique([...pi.getActiveTools().filter((t) => t !== "edit" && t !== "write"), "read", "bash"]));
    }
    if (autonomy.mode) {
      pi.sendMessage({
        customType: "mode-resumed",
        content: `[${autonomy.mode} mode was active when this session paused. Send a message to continue it, or run /stop to end it.]`,
        display: true,
      });
    }
  });
}
