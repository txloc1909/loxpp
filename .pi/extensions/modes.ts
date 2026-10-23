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
 *   loxpp_gate     build/test gate, run inside the dev container, with fail-fast preflight
 *   loxpp_done     declare done: gate always; CI + review when a PR number is given
 *   loxpp_issue    read a GitHub issue with the full --json --template form
 *   loxpp_pr       read a PR with the full form (body + comments + reviews)
 *
 * loxpp_done is the single termination point. It always re-runs the gate, so a
 * stale green gate can never end a mode, and it widens "done" beyond a local
 * green gate to CI status and reviewer approval whenever a PR number is given.
 * turn_end continues the mode under the policy in lib/mode-policy.ts: turns before
 * the first change are free, gate runs are the attempt budget, done retries
 * have their own budget, a run of turns that moves nothing stops the mode, and
 * a total-turn ceiling bounds a mode that keeps acting. The progress signal
 * covers every checkout the mode touches: the session cwd, each worktree the
 * agent edits, and each checkout a gate run names. A mode also stops early when
 * the gate fails identically on an unchanged tree three times in a row.
 */

import { spawn } from "node:child_process";
import * as path from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { StringEnum } from "@earendil-works/pi-ai";
import { Type } from "typebox";
import {
  POLICY,
  decideTurn,
  freshPolicyState,
  normalizePolicyState,
  type Mode,
  type PolicyState,
} from "./lib/mode-policy.ts";

// Human opt-in switch. false (default) keeps the /goal approval gate; true hands
// the approval decision to the agent for /goal. A per-command --handoff /
// --no-handoff flag overrides this for one invocation.
const HANDOFF = false;

const REPO = "txloc1909/loxpp";
const GATE_OUTPUT_CAP = 8000;
const READ_OUTPUT_CAP = 12000;
const STATUS_OUTPUT_CAP = 2000;
const STAGNATION_LIMIT = 3; // identical gate failures on an unchanged tree before a mode stops

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

const GOAL_WORKING_PROMPT = (goal: string) => `[GOAL MODE — implement]\n\nGoal: ${goal}\n\nThe plan is approved. Implement it now, per AGENTS.md.\n\n1. Add a test or probe for the goal's behavior and connect it to ctest (AGENTS.md engineering rules).\n2. Implement, then run loxpp_gate (it builds and tests inside the dev container) until it passes.\n3. Commit when the code compiles and push after each commit (AGENTS.md). Conventional commits; split if the subject would say "and".\n4. Open a PR when the work is ready for review.\n5. Done means more than a local green gate. When you open a PR, call loxpp_done with the PR number; it then also requires CI green and reviewer approval.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

const GOAL_HANDOFF_PROMPT = (goal: string) => `[GOAL MODE — hand-off]\n\nGoal: ${goal}\n\nThe human handed the approval decision to you. Plan, implement, and finish without waiting.\n\n1. Plan: read the binding context (VISION.md, spec/, docs/decisions/, docs/README.md). If this is a language change, list every consumer first per AGENTS.md. Keep the plan as your working notes, not a waiting step.\n2. Add a test or probe for the goal's behavior and connect it to ctest (AGENTS.md engineering rules).\n3. Implement, then run loxpp_gate (it builds and tests inside the dev container) until it passes.\n4. Commit when the code compiles and push after each commit (AGENTS.md). Conventional commits; split if the subject would say "and".\n5. Open a PR when the work is ready for review.\n6. Done means more than a local green gate. When you open a PR, call loxpp_done with the PR number; it then also requires CI green and reviewer approval.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

const LOOP_PROMPT = (goal: string) => `[LOOP MODE — hand-off]\n\nGoal: ${goal}\n\nWork in a tight loop until the gate passes:\n1. Make one focused change (or fix the latest failure).\n2. Call loxpp_gate (it builds and tests inside the dev container).\n3. If it fails, read the tail of the output, fix the smallest thing that unblocks it, and repeat.\n4. When the gate is green, call loxpp_done to finish. If the work is on a PR, pass the PR number; the loop then does not stop before CI is green and the reviewer approved.\n\nCommit when the code compiles and push after each commit, per AGENTS.md. Do not ask to stop.`;

const NODE_PROMPT = (n: string) => `[RESOLVE-ISSUE MODE — implementer, hand-off]\n\nNode issue: #${n}\n\n1. Read the node spec with loxpp_issue (full form — a hazard may live in a comment).\n2. Read docs/process/multi-agent-playbook.md, the decision in docs/decisions/ this node serves, and the spec/reference docs the issue names.\n3. Create a worktree and branch per AGENTS.md's task loop. Pass the checkout you work in to loxpp_gate and loxpp_done via their cwd argument, so the gate builds and tests that tree inside the dev container, not the main checkout.\n4. Implement the deliverable; respect the issue's Scope section — out-of-scope cases fail loudly and name this node.\n5. Checkpoint: rebuild fully, then run loxpp_gate until green (ctest does not rebuild).\n6. Commit atomically with conventional commits; push after each commit.\n7. Open a PR whose body contains \`Closes #${n}\`, and tag yourself \`[Implementer]\`. Do not poll CI.\n8. Call loxpp_done with the PR number. Done requires the local gate green, CI green, and the reviewer approved.\n\nEnd only by calling loxpp_done once every done criterion holds.`;

interface AutonomyState {
  mode: Mode | null;
  policy: PolicyState;
  lastGatePassed: boolean | undefined;
  lastScope: "native" | "backend";
  lastCwd: string | undefined;
  declaredDone: boolean;
  sameFailureCount: number;
  lastFailureSignature: string | undefined;
  /** Worktree signature at the last turn or gate run, for the progress signal. */
  lastTreeSignature: string | undefined;
  /** The current turn changed the tree or produced a new gate result. */
  progressThisTurn: boolean;
  /** Checkouts the mode watches: the session cwd, edited worktrees, gate cwds. */
  roots: string[];
  /** The turn ran a mutating shell command, whose target is not knowable. */
  shellMutation: boolean;
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
): Promise<{ code: number; stdout: string; stderr: string }> {
  return new Promise((resolve) => {
    let stdout = "";
    let stderr = "";
    proc.stdout.on("data", (d) => {
      stdout += d.toString();
    });
    proc.stderr.on("data", (d) => {
      stderr += d.toString();
    });
    proc.on("close", (code) => resolve({ code: code ?? 1, stdout, stderr }));
    proc.on("error", (err) => resolve({ code: 1, stdout, stderr: String(err) }));
    if (signal) {
      const abort = () => proc.kill("SIGTERM");
      if (signal.aborted) abort();
      else signal.addEventListener("abort", abort, { once: true });
    }
  });
}

// argv spawn (no shell) for gh and python: model-supplied values stay arguments,
// so they cannot be interpreted as shell syntax. stdout and stderr are kept
// separate so callers that parse JSON read only stdout.
async function runArgv(argv: string[], cwd: string, signal: AbortSignal | undefined) {
  const r = await collect(spawn(argv[0], argv.slice(1), { cwd }), signal);
  return { ...r, output: r.stdout + r.stderr };
}

// The repo root that owns `dir`, so a signature follows the agent into a
// worktree instead of watching the checkout the session started in.
async function repoRoot(dir: string): Promise<string | undefined> {
  const r = await runArgv(["git", "-C", dir, "rev-parse", "--show-toplevel"], dir, undefined);
  if (r.code !== 0) return undefined;
  const root = r.stdout.trim();
  return root.length > 0 ? root : undefined;
}

// A stable picture of one checkout: HEAD plus the content hash of every path
// `git status` reports. Content, not mtime, so a rewrite with identical bytes
// is not progress. `-z` gives raw paths, so a name with a space or a non-ASCII
// character is the path on disk and not a quoted form that `hash-object` cannot
// open. Returns undefined outside a git checkout.
async function checkoutSignature(root: string): Promise<string | undefined> {
  const head = await runArgv(["git", "-C", root, "rev-parse", "HEAD"], root, undefined);
  if (head.code !== 0) return undefined;
  const status = await runArgv(["git", "-C", root, "status", "--porcelain", "-z"], root, undefined);
  if (status.code !== 0) return undefined;
  const entries: string[] = [];
  const paths: string[] = [];
  const fields = status.stdout.split("\u0000");
  for (let i = 0; i < fields.length; i += 1) {
    const field = fields[i];
    if (!field) continue;
    const statusCode = field.slice(0, 2);
    const file = field.slice(3);
    entries.push(`${statusCode} ${file}`);
    if (file) paths.push(file);
    // A rename or copy record is followed by its original path as its own field.
    if (statusCode.startsWith("R") || statusCode.startsWith("C")) i += 1;
  }
  const hashed =
    paths.length > 0
      ? await runArgv(["git", "-C", root, "hash-object", "--", ...paths.slice(0, 200)], root, undefined)
      : { stdout: "" };
  return `${head.stdout.trim()}\n${entries.join("\n")}\n${hashed.stdout}`;
}

// A mode can span checkouts: the session cwd, each worktree the agent edits,
// and each checkout a gate run names. Combine them so a change anywhere counts.
async function treeSignature(roots: readonly string[]): Promise<string | undefined> {
  const parts: string[] = [];
  for (const root of [...roots].sort()) {
    const one = await checkoutSignature(root);
    if (one !== undefined) parts.push(`${root}\n${one}`);
  }
  return parts.length > 0 ? parts.join("\n\n") : undefined;
}

// The agent runs on the host; program execution runs inside the dev container
// (AGENTS.md), so the gate always enters the container via podman.
async function gatePreflight(
  scope: "native" | "backend",
  signal: AbortSignal | undefined,
): Promise<{ ok: boolean; output: string }> {
  const image = scope === "backend" ? "loxpp-dev-env-managed" : "loxpp-dev-env";
  // These checks do not depend on the checkout; run them from process.cwd() so a
  // missing checkout is not misreported as a missing podman.
  const podman = await runArgv(["podman", "--version"], process.cwd(), signal);
  if (podman.code !== 0) {
    return {
      ok: false,
      output: "podman is not installed on the host. The gate builds and tests inside the dev container, so podman is required.",
    };
  }
  const imageCheck = await runArgv(["podman", "image", "exists", image], process.cwd(), signal);
  if (imageCheck.code !== 0) {
    return {
      ok: false,
      output: `The ${image} image is missing. Build it first (see AGENTS.md task loop), then re-run the gate.`,
    };
  }
  return { ok: true, output: "" };
}

async function runGate(
  scope: "native" | "backend",
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ passed: boolean; output: string }> {
  const preflight = await gatePreflight(scope, signal);
  if (!preflight.ok) {
    return { passed: false, output: preflight.output };
  }
  const image = scope === "backend" ? "loxpp-dev-env-managed" : "loxpp-dev-env";
  const script = scope === "backend" ? BACKEND_CONTAINER_GATE : NATIVE_CONTAINER_GATE;
  // podman -v needs an absolute host path; a relative one becomes a named volume.
  const absCwd = path.resolve(cwd);
  const argv = [
    "podman", "run", "--rm",
    "-v", `${absCwd}:/workspace:z`,
    "-v", "loxpp-ccache:/ccache",
    image,
    "bash", "-lc", script,
  ];
  const r = await runArgv(argv, absCwd, signal);
  return { passed: r.code === 0, output: tail(r.output, GATE_OUTPUT_CAP) };
}

async function ciGreen(
  pr: string,
  repo: string,
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ ok: boolean; state: "green" | "pending" | "failed"; output: string }> {
  const r = await runArgv(["gh", "pr", "checks", pr, "--repo", repo], cwd, signal);
  if (r.code === 0) return { ok: true, state: "green", output: r.output };
  // Failure before pending: a mixed run with a failed and a pending check must
  // read as failed, not "still running".
  const state = /fail/i.test(r.stdout) ? "failed" : /pending/i.test(r.stdout) ? "pending" : "failed";
  return { ok: false, state, output: r.output };
}

async function reviewApproved(
  pr: string,
  repo: string,
  cwd: string,
  signal: AbortSignal | undefined,
): Promise<{ ok: boolean; output: string }> {
  const r = await runArgv(["gh", "pr", "view", pr, "--repo", repo, "--json", "reviews,comments"], cwd, signal);
  if (r.code !== 0) return { ok: false, output: r.output };
  try {
    const data = JSON.parse(r.stdout);
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

// A redirection to a file writes without a write tool, so the reading phase must
// end for it too. A descriptor target, `/dev/null`, and `-` write no file. This
// is a heuristic: a write from inside an interpreter is missed, and a miss only
// delays the phase switch to the reading ceiling.
function redirectsToFile(command: string): boolean {
  const pattern = /(?:^|[^>&|])\d?>>?\s*([^\s;&|]+)/g;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(command)) !== null) {
    const target = match[1];
    if (target.startsWith("&")) continue;
    if (/^\/dev\/(null|stdout|stderr|tty)$/.test(target)) continue;
    return true;
  }
  return /\btee\b/.test(command) && !/tee\s+-a?\s*\/dev\/null/.test(command);
}

function looksLikeWrite(command: string): boolean {
  return isMutatingCommand(command) || redirectsToFile(command);
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
    policy: freshPolicyState(),
    lastGatePassed: undefined,
    lastScope: "native",
    lastCwd: undefined,
    declaredDone: false,
    sameFailureCount: 0,
    lastFailureSignature: undefined,
    lastTreeSignature: undefined,
    progressThisTurn: false,
    roots: [],
    shellMutation: false,
  };
  let plan: PlanState = { active: false, goal: "", toolsBefore: undefined };

  const unique = (names: string[]): string[] => [...new Set(names)];

  function persist(): void {
    pi.appendEntry("loxpp-modes", { autonomy, plan } satisfies PersistedState);
  }

  // Watch the checkout that owns `dir`. A mode moves into a worktree before its
  // first gate run, so the session cwd alone does not see the agent's edits.
  async function addRoot(dir: string): Promise<void> {
    const root = await repoRoot(dir);
    if (root && !autonomy.roots.includes(root)) autonomy.roots.push(root);
  }

  async function recordGate(
    scope: "native" | "backend",
    cwd: string,
    passed: boolean,
    output: string,
    kind: "gate" | "done",
  ): Promise<void> {
    autonomy.lastGatePassed = passed;
    autonomy.lastScope = scope;
    autonomy.lastCwd = cwd;
    // A done attempt waits for CI and review, not for a fix, so it is bounded
    // by its own budget.
    if (kind === "gate") autonomy.policy.gateRuns += 1;
    else autonomy.policy.doneRuns += 1;
    await addRoot(cwd);
    const tree = await treeSignature(autonomy.roots);
    if (tree !== autonomy.lastTreeSignature) autonomy.progressThisTurn = true;
    if (tree !== undefined) autonomy.lastTreeSignature = tree;
    if (passed) {
      autonomy.sameFailureCount = 0;
      autonomy.lastFailureSignature = undefined;
      autonomy.progressThisTurn = true;
    } else {
      // The tree is part of the signature: the same failure on a changed tree
      // is a new attempt, not stagnation.
      const sig = `${output.slice(-800)}\n${tree ?? ""}`;
      if (sig !== autonomy.lastFailureSignature) autonomy.progressThisTurn = true;
      autonomy.sameFailureCount =
        sig === autonomy.lastFailureSignature ? autonomy.sameFailureCount + 1 : 1;
      autonomy.lastFailureSignature = sig;
    }
  }

  function notify(ctx: ExtensionContext, text: string, kind: "info" | "error" = "info"): void {
    if (ctx.hasUI) ctx.ui.notify(text, kind);
  }

  function startWorking(mode: Mode, prompt: string): void {
    autonomy = {
      mode,
      policy: freshPolicyState(),
      lastGatePassed: undefined,
      lastScope: "native",
      lastCwd: undefined,
      declaredDone: false,
      sameFailureCount: 0,
      lastFailureSignature: undefined,
      lastTreeSignature: undefined,
      progressThisTurn: false,
      roots: [],
      shellMutation: false,
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
      const cwd = params.cwd ? path.resolve(ctx.cwd, params.cwd) : ctx.cwd;
      const r = await runGate(scope, cwd, signal);
      await recordGate(scope, cwd, r.passed, r.output, "gate");
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
      "Declare the task complete and end the mode. Always re-runs the local gate (inside the dev container); when a PR number is given, also requires CI green and reviewer approval.",
    promptSnippet: "verify done (gate, plus CI and review when a PR is given) and end the mode",
    parameters: Type.Object({
      summary: Type.String({ description: "What was done and what changed." }),
      pr: Type.Optional(Type.String({ description: "PR number or URL. When given, CI green and [Reviewer] APPROVED are also required." })),
      repo: Type.Optional(Type.String({ description: `Repo, default ${REPO}.` })),
      scope: Type.Optional(
        StringEnum(["native", "backend"] as const, {
          description: "Gate scope to re-run. Default the scope of the last loxpp_gate call.",
        }),
      ),
      cwd: Type.Optional(Type.String({ description: "Checkout to gate. Default the last loxpp_gate cwd." })),
    }),
    async execute(_id, params, signal, _onUpdate, ctx) {
      const scope: "native" | "backend" = params.scope ?? autonomy.lastScope ?? "native";
      const cwd = params.cwd ? path.resolve(ctx.cwd, params.cwd) : (autonomy.lastCwd ?? ctx.cwd);
      const repo = params.repo ?? REPO;
      const pr = params.pr?.trim();

      const failures: string[] = [];
      const r = await runGate(scope, cwd, signal);
      await recordGate(scope, cwd, r.passed, r.output, "done");
      if (!r.passed) {
        failures.push(`gate not green:\n${r.output}`);
      } else if (pr) {
        const ci = await ciGreen(pr, repo, cwd, signal);
        if (!ci.ok) {
          failures.push(
            ci.state === "pending"
              ? "CI still running. Wait for it to finish, then call loxpp_done again."
              : `CI not green:\n${ci.output}`,
          );
        }
        const rv = await reviewApproved(pr, repo, cwd, signal);
        if (!rv.ok) failures.push(`review not approved: ${rv.output}`);
      }

      if (failures.length > 0) {
        return {
          isError: true,
          content: [{ type: "text", text: `Not done:\n- ${failures.join("\n- ")}` }],
          details: { done: false, scope, cwd },
        };
      }

      autonomy.declaredDone = true;
      persist();
      return {
        content: [
          {
            type: "text",
            text: `Recorded completion${pr ? " (gate + CI + review)" : " (gate green)"}.\n\n${params.summary}${pr ? `\n\nPR: ${pr}` : ""}`,
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

  // The policy leaves its reading phase on the first call that can change the
  // tree. Plan mode blocks those calls, and it runs with no active mode.
  pi.on("tool_call", async (event, ctx) => {
    if (autonomy.mode) {
      if (event.toolName === "edit" || event.toolName === "write") {
        autonomy.policy.started = true;
        const target = String((event.input as { path?: unknown }).path ?? "");
        if (target) await addRoot(path.dirname(path.resolve(ctx.cwd, target)));
      } else if (event.toolName === "bash") {
        const command = String((event.input as { command?: unknown }).command ?? "");
        if (looksLikeWrite(command)) {
          autonomy.policy.started = true;
          // The target of a shell mutation is not knowable, so the command is
          // the progress signal for this turn.
          autonomy.shellMutation = true;
        }
      }
    }
    // The /goal plan phase blocks writes; the working modes rely on loxpp_done
    // to re-verify the gate, so no edit/write invalidation is needed here.
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

  pi.on("turn_end", async (_event, ctx) => {
    const mode = autonomy.mode;
    if (!mode) return;
    if (autonomy.declaredDone) {
      autonomy.mode = null;
      persist();
      return;
    }

    if (autonomy.sameFailureCount >= STAGNATION_LIMIT) {
      autonomy.mode = null;
      persist();
      return {
        entries: [
          {
            type: "custom_message",
            customType: "mode-stagnant",
            content: `${mode} mode stopped: the gate failed the same way ${STAGNATION_LIMIT} times in a row on an unchanged tree. This is likely an environment or toolchain problem, not the code. Fix the environment, then re-run the mode.`,
            display: true,
          },
        ],
      };
    }

    // Tree progress since the last turn. The gate's own record may already have
    // seen the change; this covers turns that edit without running the gate.
    // The session cwd is added here because a mode starts before its first gate
    // run names a checkout.
    await addRoot(ctx.cwd);
    const tree = await treeSignature(autonomy.roots);
    if (tree !== undefined && tree !== autonomy.lastTreeSignature) autonomy.progressThisTurn = true;
    if (tree !== undefined) autonomy.lastTreeSignature = tree;
    if (autonomy.shellMutation) autonomy.progressThisTurn = true;
    autonomy.shellMutation = false;

    const decision = decideTurn(
      autonomy.policy,
      POLICY[mode],
      { progress: autonomy.progressThisTurn, gatePassed: autonomy.lastGatePassed === true },
      mode,
    );
    autonomy.policy = decision.state;
    autonomy.progressThisTurn = false;
    persist();

    if (decision.stop) {
      autonomy.mode = null;
      persist();
      return {
        entries: [
          {
            type: "custom_message",
            customType: "mode-stop",
            content: decision.reason,
            display: true,
          },
        ],
      };
    }

    return {
      entries: [{ type: "custom_message", customType: "mode-continue", content: decision.nudge, display: true }],
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
      const saved = last.data.autonomy as Partial<AutonomyState> | undefined;
      autonomy = {
        ...autonomy,
        ...saved,
        policy: normalizePolicyState(saved?.policy),
        roots: saved?.roots ?? [],
        shellMutation: false,
      };
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
