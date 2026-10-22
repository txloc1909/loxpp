/**
 * Integration test for the autonomous-mode turn policy, driven by a fake model.
 *
 *   node tools/test_pi_modes.mjs             # run every scenario
 *   node tools/test_pi_modes.mjs reading     # run one scenario
 *   node tools/test_pi_modes.mjs --dump      # print what each session produced
 *   node tools/test_pi_modes.mjs --modes <path>  # test another checkout's modes.ts
 *
 * A scenario scripts the fake model's turns and asserts what the mode did: the
 * messages it injected, and the counters in its last persisted state. Each
 * scenario is one guard of the policy, so a regression names the guard it broke.
 *
 * The run is a real `pi --mode rpc` session with the extension loaded from this
 * checkout. It needs the pi binary, git, and node. It does not need the Lox++
 * build: a `podman` stand-in on PATH answers the gate.
 */

import { execFileSync, spawn } from "node:child_process";
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import http from "node:http";
import { tmpdir } from "node:os";
import * as path from "node:path";

const argv = process.argv.slice(2);
const dump = argv.includes("--dump");
const modesArg = argv.indexOf("--modes");
const MODES = path.resolve(
  modesArg === -1 ? path.join(import.meta.dirname, "../.pi/extensions/modes.ts") : argv[modesArg + 1],
);
const only = argv.find((a, i) => !a.startsWith("--") && argv[i - 1] !== "--modes");
const PI = process.env.PI_BIN ?? "pi";
const STOP_TIMEOUT_MS = 120_000;

// ---------------------------------------------------------------- fake model

/**
 * `step(turn)` returns the assistant reply for turn number `turn`, counted from
 * the assistant messages already in the request. Turn 0 is the first answer
 * after /loop.
 */
const SCENARIOS = {
  // Reading only, up to the read-phase ceiling: no turn before the first change
  // may be billed against the idle cap.
  reading: {
    turns: 40,
    step: (turn) => ({ text: `reading turn ${turn}` }),
    expect: { stop: "no file changed in 40 turns", started: false, minContinues: 39 },
  },
  // One write, then silence: the mode must notice the worktree stopped moving.
  idle: {
    step: (turn) =>
      turn === 0
        ? { tool: { name: "write", args: { path: "work.txt", content: "v0" } } }
        : { text: `idle turn ${turn}` },
    expect: { stop: "changed no file and produced no new gate result", idleTurns: 10 },
  },
  // A write on every turn: the worktree moves, so the mode must keep going even
  // past the old 12-turn budget. The harness ends the run, not the mode.
  progress: {
    turns: 25,
    step: (turn) => ({ tool: { name: "write", args: { path: "work.txt", content: `v${turn}` } } }),
    expect: { stop: false, started: true, gateRuns: 0 },
  },
  // One write, then a gate run on every second turn, always failing in a new
  // way. The mode must stop on the gate-run budget, not on the turn count.
  gates: {
    step: (turn) =>
      turn === 0
        ? { tool: { name: "write", args: { path: "work.txt", content: "v0" } } }
        : turn % 2 === 1
          ? { text: `thinking ${turn}` }
          : { tool: { name: "loxpp_gate", args: { scope: "native" } } },
    gate: "fail-each",
    expect: { stop: "after 12 gate runs", gateRuns: 12, minTurns: 24 },
  },
  // The same, but the last allowed gate run passes: one grace turn tells the
  // agent to call loxpp_done.
  green: {
    step: (turn) =>
      turn === 0
        ? { tool: { name: "write", args: { path: "work.txt", content: "v0" } } }
        : turn % 2 === 1
          ? { text: `thinking ${turn}` }
          : { tool: { name: "loxpp_gate", args: { scope: "native" } } },
    gate: "fail-then-pass",
    expect: { stop: "green, but loxpp_done was not called", gateRuns: 12, graceSeen: true },
  },
};

function startProvider(scenario) {
  const server = http.createServer((req, res) => {
    let body = "";
    req.on("data", (d) => (body += d));
    req.on("end", () => {
      if (!req.url.includes("/chat/completions")) {
        res.writeHead(404).end();
        return;
      }
      const parsed = JSON.parse(body);
      const turn = (parsed.messages ?? []).filter((m) => m.role === "assistant").length;
      const reply = scenario.step(turn);
      res.writeHead(200, { "content-type": "text/event-stream", connection: "keep-alive" });
      const base = { id: "chatcmpl-1", object: "chat.completion.chunk", created: 0, model: "dummy" };
      const send = (o) => res.write(`data: ${JSON.stringify({ ...base, ...o })}\n\n`);
      send({ choices: [{ index: 0, delta: { role: "assistant", content: reply.text ?? null }, finish_reason: null }] });
      if (reply.tool) {
        send({
          choices: [
            {
              index: 0,
              delta: {
                tool_calls: [
                  {
                    index: 0,
                    id: `call_${turn}`,
                    type: "function",
                    function: { name: reply.tool.name, arguments: JSON.stringify(reply.tool.args) },
                  },
                ],
              },
              finish_reason: null,
            },
          ],
        });
        send({ choices: [{ index: 0, delta: {}, finish_reason: "tool_calls" }] });
      } else {
        send({ choices: [{ index: 0, delta: {}, finish_reason: "stop" }] });
      }
      res.write("data: [DONE]\n\n");
      res.end();
    });
  });
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve({ server, port: server.address().port }));
  });
}

// ------------------------------------------------------------------- fixtures

/**
 * A `podman` stand-in on PATH, so loxpp_gate runs its real code path without a
 * container. Each `podman run` appends its exit code to a counter file.
 */
function writePodmanShim(dir, mode) {
  const counter = path.join(dir, "gate-count");
  writeFileSync(
    path.join(dir, "podman"),
    `#!/bin/sh
case "$1" in
  --version) echo "podman version 5.0.0"; exit 0 ;;
  image) exit 0 ;;
  run)
    n=$(cat "${counter}" 2>/dev/null || echo 0)
    n=$((n + 1))
    echo "$n" > "${counter}"
    case "${mode}" in
      fail-each) echo "failure number $n"; exit 1 ;;
      fail-then-pass) if [ "$n" -ge 12 ]; then echo "ok"; exit 0; else echo "failure number $n"; exit 1; fi ;;
      *) exit 0 ;;
    esac ;;
esac
exit 0
`,
    { mode: 0o755 },
  );
  chmodSync(path.join(dir, "podman"), 0o755);
  return counter;
}

function writeRepo(dir) {
  execFileSync("git", ["init", "-q", dir]);
  writeFileSync(path.join(dir, "README.md"), "fixture\n");
  execFileSync("git", ["-C", dir, "add", "."]);
  execFileSync("git", ["-C", dir, "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "init"]);
}

// ------------------------------------------------------------- rpc session

/** Run `pi --mode rpc` until the mode settles, then return the session entries. */
function runSession({ repo, agentDir, bin, prompt, turnLimit }) {
  return new Promise((resolve, reject) => {
    const child = spawn(PI, ["--mode", "rpc", "--approve", "--extension", MODES, "--model", "fake/dummy"], {
      cwd: repo,
      env: {
        ...process.env,
        PI_CODING_AGENT_DIR: agentDir,
        PATH: `${bin}:${process.env.PATH}`,
        PI_NO_UPDATE_CHECK: "1",
      },
      stdio: ["pipe", "pipe", "pipe"],
    });

    let buffer = "";
    let stderr = "";
    let settled = false;
    let turns = 0;
    let entries;
    let nextId = 1;
    const send = (command) => child.stdin.write(`${JSON.stringify({ id: `req-${nextId++}`, ...command })}\n`);
    const timer = setTimeout(() => {
      child.kill("SIGTERM");
      reject(new Error(`the mode did not settle in ${STOP_TIMEOUT_MS} ms`));
    }, STOP_TIMEOUT_MS);

    const finish = () => {
      clearTimeout(timer);
      child.stdin.end();
      setTimeout(() => child.kill("SIGKILL"), 2000).unref();
    };

    child.stderr.on("data", (d) => (stderr += d));
    child.on("error", (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.on("close", () => {
      if (entries) resolve({ entries, stderr });
      else {
        clearTimeout(timer);
        reject(new Error(`pi exited before returning entries\n${stderr}`));
      }
    });

    child.stdout.on("data", (chunk) => {
      buffer += chunk;
      let newline;
      while ((newline = buffer.indexOf("\n")) !== -1) {
        const line = buffer.slice(0, newline).trim();
        buffer = buffer.slice(newline + 1);
        if (!line) continue;
        let event;
        try {
          event = JSON.parse(line);
        } catch {
          continue;
        }
        if (dump && event.type !== "message_update") console.log("event", event.type, event.command ?? "");
        if (event.type === "turn_end") turns += 1;
        if (event.type === "response" && event.command === "get_entries") {
          entries = event.data.entries;
          finish();
        } else if (
          (event.type === "agent_settled" && !settled) ||
          (turnLimit !== undefined && turns >= turnLimit && !entries)
        ) {
          settled = true;
          // A command that starts a run answers the prompt first, so the run it
          // starts is not the run the prompt response awaits. Settle, or reach
          // the scenario's turn limit, then ask for the entries the mode saved.
          send({ type: "get_entries" });
        }
      }
    });

    send({ type: "prompt", message: prompt });
  });
}

// -------------------------------------------------------------------- checks

function collect(entries) {
  const messages = entries
    .filter((e) => e.type === "custom_message")
    .map((e) => ({ kind: e.customType, text: String(e.content ?? "") }));
  const states = entries
    .filter((e) => e.type === "custom" && e.customType === "loxpp-modes")
    .map((e) => e.data?.autonomy?.policy)
    .filter(Boolean);
  return { messages, state: states.at(-1) };
}

async function run(name) {
  const scenario = SCENARIOS[name];
  const root = mkdtempSync(path.join(tmpdir(), `pi-modes-${name}-`));
  const repo = path.join(root, "repo");
  const bin = path.join(root, "bin");
  const agentDir = path.join(root, "agent");
  for (const dir of [repo, bin, agentDir]) mkdirSync(dir, { recursive: true });
  writeRepo(repo);
  const gateCounter = writePodmanShim(bin, scenario.gate ?? "always-pass");

  const { server, port } = await startProvider(scenario);
  writeFileSync(
    path.join(agentDir, "models.json"),
    JSON.stringify({
      providers: {
        fake: {
          baseUrl: `http://127.0.0.1:${port}/v1`,
          api: "openai-completions",
          apiKey: "fake",
          models: [{ id: "dummy", input: ["text"], contextWindow: 200000, maxTokens: 8192 }],
        },
      },
    }),
  );

  try {
    const { entries, stderr } = await runSession({
      repo,
      agentDir,
      bin,
      prompt: "/loop test goal",
      turnLimit: scenario.turns,
    });
    const { messages, state } = collect(entries);
    const gateRuns = existsSync(gateCounter) ? Number(readFileSync(gateCounter, "utf8")) : 0;
    const turns = entries.filter((e) => e.type === "message" && e.message?.role === "assistant").length;
    return { messages, state, gateRuns, turns, stderr };
  } finally {
    server.close();
  }
}

let failed = 0;
for (const name of only ? [only] : Object.keys(SCENARIOS)) {
  const scenario = SCENARIOS[name];
  let result;
  try {
    result = await run(name);
  } catch (error) {
    console.error(`FAIL ${name}: ${error?.message ?? error}`);
    failed += 1;
    continue;
  }

  const texts = result.messages.map((m) => m.text);
  if (dump) {
    console.log(
      `--- ${name}: ${texts.length} messages, ${result.turns} turns, ${result.gateRuns} gate runs, state ${JSON.stringify(result.state)}`,
    );
    for (const m of result.messages) console.log(`   [${m.kind}] ${m.text.replace(/\n/g, " | ")}`);
  }

  const stops = result.messages.filter((m) => m.kind !== "mode-continue");
  const continues = result.messages.filter((m) => m.kind === "mode-continue");
  const expect = scenario.expect;
  const failures = [];

  if (expect.stop === false) {
    if (stops.length > 0) failures.push(`expected no stop, got: ${stops[0].text}`);
  } else if (!stops.some((m) => m.text.includes(expect.stop))) {
    failures.push(`expected a stop matching "${expect.stop}", got: ${stops[0]?.text ?? "(none)"}`);
  }
  if (expect.minContinues !== undefined && continues.length < expect.minContinues) {
    failures.push(`expected at least ${expect.minContinues} continue turns, got ${continues.length}`);
  }
  if (expect.minTurns !== undefined && result.turns < expect.minTurns) {
    failures.push(`expected at least ${expect.minTurns} assistant turns, got ${result.turns}`);
  }
  if (expect.gateRuns !== undefined) {
    if (result.gateRuns !== expect.gateRuns) failures.push(`expected ${expect.gateRuns} gate runs, got ${result.gateRuns}`);
    if (result.state?.gateRuns !== expect.gateRuns) {
      failures.push(`expected the persisted gateRuns to be ${expect.gateRuns}, got ${result.state?.gateRuns}`);
    }
  }
  if (expect.idleTurns !== undefined && result.state?.idleTurns !== expect.idleTurns) {
    failures.push(`expected the persisted idleTurns to be ${expect.idleTurns}, got ${result.state?.idleTurns}`);
  }
  if (expect.started !== undefined && result.state?.started !== expect.started) {
    failures.push(`expected persisted started=${expect.started}, got ${result.state?.started}`);
  }
  if (expect.graceSeen && !texts.some((t) => t.includes("Call loxpp_done now"))) {
    failures.push("expected a grace turn telling the agent to call loxpp_done");
  }
  if (expect.started === false && continues.some((m) => /gate runs:/.test(m.text))) {
    failures.push("a pre-mutation turn was billed against the attempt budget");
  }
  if (SCENARIOS[name].turns !== undefined && result.turns !== SCENARIOS[name].turns) {
    failures.push(`expected the session to stop after ${SCENARIOS[name].turns} turns, got ${result.turns}`);
  }

  if (failures.length > 0) {
    failed += 1;
    console.error(`FAIL ${name}`);
    for (const f of failures) console.error(`  - ${f}`);
    if (result.stderr.trim()) console.error(`  stderr: ${result.stderr.trim().split("\n").slice(-3).join(" | ")}`);
  } else {
    console.log(`ok   ${name} (${result.turns} turns, ${result.gateRuns} gate runs)`);
  }
}

if (failed > 0) {
  console.error(`${failed} scenario(s) failed`);
  process.exit(1);
}