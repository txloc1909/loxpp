/**
 * Lox++ autonomous modes + mission roles.
 *
 * Hand-off modes (no approval gate — the agent works to completion):
 *   /goal <goal>     plan + implement + verify + commit/push, end via loxpp_done
 *   /loop [goal]     fix-until-green; ends automatically when loxpp_gate passes
 *   /resolve-issue <issue>  implementer role for one mission node (multi-agent-playbook)
 *
 * Single-shot roles (inject a role prompt, run one turn):
 *   /review <subject>   reviewer: adversarial, reproduce the checkpoint, tag [Reviewer]
 *   /referee <subject>  researcher/referee: verify facts, rule, bound next round, [Researcher]
 *
 * Run-observation:
 *   /mission-status <issue>  show plan_resume.py + status.py output
 *
 * Tools:
 *   loxpp_gate     build/test gate (machine-verified pass/fail)
 *   loxpp_done     declare completion; refuses unless the last gate passed
 *   loxpp_issue    read a GitHub issue with the full --json --template form
 *   loxpp_pr       read a PR with the full form (body + comments + reviews)
 *
 * The continuation loop is driven by the gate's real exit code, never by the
 * model's say-so: turn_end keeps running while a mode is active and the gate is
 * not green, bounded by a per-mode iteration budget.
 */

import { spawn } from "node:child_process";
import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { StringEnum } from "@earendil-works/pi-ai";
import { Type } from "typebox";

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

const GATE_COMMANDS: Record<"native" | "backend", string[]> = {
  native: [
    "cmake --preset debug && cmake --build build",
    "ctest --test-dir build --output-on-failure -j$(nproc)",
  ],
  backend: [
    "tools/build_lox_rt.sh && tools/check_managed_toolchains.sh",
    "cmake --preset release && cmake --build build",
    "ctest --test-dir build --output-on-failure -j$(nproc)",
    "tools/diff_runtimes.py",
  ],
};

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

interface AutonomyState {
  mode: "goal" | "loop" | "resolve-issue" | null;
  iterations: number;
  lastGatePassed: boolean | undefined;
  declaredDone: boolean;
}

function tail(text: string, cap: number): string {
  const bytes = Buffer.byteLength(text, "utf8");
  if (bytes <= cap) return text;
  let out = text.slice(-cap);
  while (Buffer.byteLength(out, "utf8") > cap) out = out.slice(1);
  return `[truncated: ${bytes} bytes, showing tail]\n\n${out}`;
}

function run(
  command: string,
  signal: AbortSignal | undefined,
): Promise<{ code: number; output: string }> {
  return new Promise((resolve) => {
    const proc = spawn("/bin/bash", ["-lc", command], { cwd: process.cwd() });
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

function issueCmd(number: string, repo: string): string {
  return `gh issue view ${number} --repo ${repo} --json title,body,comments --template '${ISSUE_TPL}'`;
}

function prCmd(number: string, repo: string): string {
  return `gh pr view ${number} --repo ${repo} --json title,body,comments,reviews --template '${PR_TPL}'`;
}

export default function loxppModes(pi: ExtensionAPI): void {
  let autonomy: AutonomyState = {
    mode: null,
    iterations: 0,
    lastGatePassed: undefined,
    declaredDone: false,
  };

  function persist(): void {
    pi.appendEntry("loxpp-modes", autonomy);
  }

  function startMode(mode: "goal" | "loop" | "resolve-issue", prompt: string): void {
    autonomy = {
      mode,
      iterations: 0,
      lastGatePassed: undefined,
      declaredDone: false,
    };
    persist();
    pi.sendUserMessage(prompt);
  }

  // ---- tools -----------------------------------------------------------

  pi.registerTool({
    name: "loxpp_gate",
    label: "Lox++ gate",
    description:
      "Run the Lox++ build/test gate and report pass or fail. Call it after each change. Modes keep running until this reports GATE PASSED.",
    promptSnippet: "run the Lox++ build/test gate and report pass/fail",
    parameters: Type.Object({
      scope: Type.Optional(
        StringEnum(["native", "backend"] as const, {
          description:
            "native = debug build + ctest. backend = managed toolchains + release build + ctest + diff_runtimes. Default native.",
          default: "native",
        }),
      ),
    }),
    async execute(_id, params, signal) {
      const scope: "native" | "backend" = params.scope ?? "native";
      let all = "";
      let failed = false;
      for (const cmd of GATE_COMMANDS[scope]) {
        const r = await run(cmd, signal);
        all += `\n$ ${cmd}\n${r.output}`;
        if (r.code !== 0) {
          failed = true;
          break;
        }
      }
      autonomy.lastGatePassed = !failed;
      persist();
      const shown = tail(all, GATE_OUTPUT_CAP);
      return {
        content: [
          {
            type: "text",
            text: failed ? `GATE FAILED.\n\n${shown}` : "GATE PASSED: build and tests are green.",
          },
        ],
        details: { passed: !failed, scope },
      };
    },
  });

  pi.registerTool({
    name: "loxpp_done",
    label: "Declare done",
    description:
      "Declare the task complete and end the mode. Refuses unless loxpp_gate last passed and no file was edited since.",
    promptSnippet: "end the mode: declare the task complete",
    parameters: Type.Object({
      summary: Type.String({ description: "What was done and what changed." }),
      pr: Type.Optional(Type.String({ description: "PR number or URL, if one was opened." })),
    }),
    async execute(_id, params) {
      if (autonomy.lastGatePassed !== true) {
        return {
          isError: true,
          content: [
            {
              type: "text",
              text: "Not done: the gate is not green. Run loxpp_gate, fix the failures, then call loxpp_done again.",
            },
          ],
          details: { done: false },
        };
      }
      autonomy.declaredDone = true;
      persist();
      return {
        content: [
          {
            type: "text",
            text: `Recorded completion.\n\n${params.summary}${params.pr ? `\n\nPR: ${params.pr}` : ""}`,
          },
        ],
        details: { done: true, pr: params.pr },
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
    async execute(_id, params, signal) {
      const r = await run(issueCmd(params.number, params.repo ?? REPO), signal);
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
    async execute(_id, params, signal) {
      const r = await run(prCmd(params.number, params.repo ?? REPO), signal);
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

  // ---- hand-off commands ------------------------------------------------

  pi.registerCommand("goal", {
    description: "Autonomous hand-off: plan, implement, verify, and finish a goal",
    handler: async (args, ctx) => {
      if (ctx.mode !== "tui") {
        ctx.ui.notify("goal requires interactive mode", "error");
        return;
      }
      const goal = args.trim();
      if (!goal) {
        ctx.ui.notify("Usage: /goal <goal>", "error");
        return;
      }
      startMode(
        "goal",
        `[GOAL MODE — hand-off]\n\nGoal: ${goal}\n\nWork autonomously to the goal. No approval gate — plan, implement, and finish.\n\n1. Plan: read the binding context (VISION.md, spec/, docs/decisions/, docs/README.md). If this is a language change, list every consumer first per AGENTS.md. Keep the plan as your working notes, not a waiting step.\n2. Add a test or probe for the goal's behavior and connect it to ctest (AGENTS.md engineering rules). A goal without a check is unproven.\n3. Implement, then run loxpp_gate until it passes.\n4. Commit when the code compiles and push after each commit (AGENTS.md). Conventional commits; split if the subject would say "and".\n5. When the gate is green and the work is complete, call loxpp_done with a summary.\n\nDo not stop to ask for approval. End only by calling loxpp_done after a green gate.`,
      );
    },
  });

  pi.registerCommand("loop", {
    description: "Autonomous fix-until-green loop over the Lox++ gate",
    handler: async (args, ctx) => {
      if (ctx.mode !== "tui") {
        ctx.ui.notify("loop requires interactive mode", "error");
        return;
      }
      const goal = args.trim() || "make the tree green";
      startMode(
        "loop",
        `[LOOP MODE — hand-off]\n\nGoal: ${goal}\n\nWork in a tight loop until the gate passes:\n1. Make one focused change (or fix the latest failure).\n2. Call loxpp_gate.\n3. If it fails, read the tail of the output, fix the smallest thing that unblocks it, and repeat.\n\nThe loop ends automatically when the gate passes. Commit when the code compiles and push after each commit, per AGENTS.md. Do not ask to stop.`,
      );
    },
  });

  pi.registerCommand("resolve-issue", {
    description: "Implementer mode for one mission node (hand-off)",
    handler: async (args, ctx) => {
      if (ctx.mode !== "tui") {
        ctx.ui.notify("resolve-issue requires interactive mode", "error");
        return;
      }
      const n = args.trim().replace(/^#/, "");
      if (!/^\d+$/.test(n)) {
        ctx.ui.notify("Usage: /resolve-issue <issue-number>", "error");
        return;
      }
      startMode(
        "resolve-issue",
        `[RESOLVE-ISSUE MODE — implementer, hand-off]\n\nNode issue: #${n}\n\n1. Read the node spec with loxpp_issue (full form — a hazard may live in a comment).\n2. Read docs/process/multi-agent-playbook.md, the decision in docs/decisions/ this node serves, and the spec/reference docs the issue names.\n3. Create a worktree and branch per AGENTS.md's task loop.\n4. Implement the deliverable; respect the issue's Scope section — out-of-scope cases fail loudly and name this node.\n5. Checkpoint: rebuild fully, then run loxpp_gate until green (ctest does not rebuild).\n6. Commit atomically with conventional commits; push after each commit.\n7. Open a PR whose body contains \`Closes #${n}\`, and tag yourself \`[Implementer]\`. Do not poll CI.\n8. Call loxpp_done with the PR number once the gate is green.\n\nDo not stop to ask for approval.`,
      );
    },
  });

  pi.registerCommand("stop", {
    description: "Exit any active mode (goal/loop/resolve-issue)",
    handler: async (_args, ctx) => {
      if (!autonomy.mode) {
        ctx.ui.notify("No mode is active.", "info");
        return;
      }
      autonomy.mode = null;
      persist();
      ctx.ui.notify("Mode off.", "info");
    },
  });

  // ---- single-shot role commands ----------------------------------------

  pi.registerCommand("review", {
    description: "Reviewer mode: adversarially review a PR or node",
    handler: async (args, ctx) => {
      if (ctx.mode !== "tui") {
        ctx.ui.notify("review requires interactive mode", "error");
        return;
      }
      const subject = args.trim();
      if (!subject) {
        ctx.ui.notify("Usage: /review <pr-or-issue>", "error");
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
      if (ctx.mode !== "tui") {
        ctx.ui.notify("referee requires interactive mode", "error");
        return;
      }
      const subject = args.trim();
      if (!subject) {
        ctx.ui.notify("Usage: /referee <issue>", "error");
        return;
      }
      pi.sendUserMessage(
        `[REFEREE MODE — researcher]\n\nIssue: ${subject}\n\n1. Read docs/records/referee-rulings-*.md first.\n2. Verify the facts yourself: read spec/ and src/, run the code. Take neither side's word.\n3. Rule: assign the tag to the implementer, the reviewer, or a third option.\n4. Bound the next round: state the exact action that must now happen, so the loop cannot repeat.\n\nTag your comment \`[Researcher]\`.`,
      );
    },
  });

  // ---- run-observation --------------------------------------------------

  pi.registerCommand("mission-status", {
    description: "Show mission state (plan_resume.py + status.py)",
    handler: async (args, ctx) => {
      const n = args.trim().replace(/^#/, "");
      if (!/^\d+$/.test(n)) {
        ctx.ui.notify("Usage: /mission-status <tracking-issue-number>", "error");
        return;
      }
      const cmds = [
        `python3 tools/agent-workflow/plan_resume.py ${n}`,
        `python3 tools/agent-workflow/status.py`,
      ];
      let all = "";
      for (const cmd of cmds) {
        const r = await run(cmd, undefined);
        all += `\n$ ${cmd}\n${r.output}`;
      }
      ctx.ui.notify(tail(all, STATUS_OUTPUT_CAP), "info");
    },
  });

  // ---- engine -----------------------------------------------------------

  // Editing a file after a green gate invalidates both the gate and any prior
  // done declaration, so "done" cannot be stale relative to the tree.
  pi.on("tool_call", async (event) => {
    if (!autonomy.mode) return;
    if (event.toolName === "edit" || event.toolName === "write") {
      autonomy.lastGatePassed = false;
      autonomy.declaredDone = false;
    }
  });

  pi.on("turn_end", async () => {
    const mode = autonomy.mode;
    if (!mode) return;
    autonomy.iterations += 1;
    const gateGreen = autonomy.lastGatePassed === true;
    const finished = mode === "loop" ? gateGreen : gateGreen && autonomy.declaredDone;

    if (finished) {
      autonomy.mode = null;
      persist();
      return;
    }

    const budget = BUDGET[mode];
    if (autonomy.iterations >= budget) {
      autonomy.mode = null;
      persist();
      return {
        entries: [
          {
            type: "custom_message",
            customType: "mode-limit",
            content: `${mode} mode stopped after ${budget} iterations. Continue by hand or re-run /${mode}.`,
            display: true,
          },
        ],
      };
    }

    const nudge = gateGreen
      ? `[${mode} mode] ${autonomy.iterations}/${budget}: the gate is green. Summarize what changed and call loxpp_done to finish.`
      : `[${mode} mode] ${autonomy.iterations}/${budget}: the gate is not green yet. Read the latest failure output, fix it, then call loxpp_gate again.`;
    return {
      entries: [
        { type: "custom_message", customType: "mode-continue", content: nudge, display: true },
      ],
      continue: true,
    };
  });

  // ---- restore on resume ------------------------------------------------

  pi.on("session_start", async (_event, ctx) => {
    const entries = ctx.sessionManager.getEntries();
    const last = entries
      .filter(
        (e: { type: string; customType?: string }) =>
          e.type === "custom" && e.customType === "loxpp-modes",
      )
      .pop() as { data?: AutonomyState } | undefined;
    if (last?.data) {
      autonomy = {
        mode: last.data.mode,
        iterations: last.data.iterations,
        lastGatePassed: last.data.lastGatePassed,
        declaredDone: last.data.declaredDone,
      };
    }
  });
}
