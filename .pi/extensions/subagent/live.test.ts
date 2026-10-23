/**
 * Live kitty test: needs a real kitty with remote control over a socket. Run
 * it from a shell inside a kitty window, so KITTY_LISTEN_ON is inherited:
 *
 *     LOXPP_SUBAGENT_LIVE=1 node --test .pi/extensions/subagent/live.test.ts
 *
 * The socket is required: without it `kitten @` writes to the terminal, where
 * pi reads its keys. The address kitty reports may differ from the `listen_on`
 * text, because kitty appends -<pid> when the line has no {kitty_pid}.
 *
 * The unit tests cover the plumbing with a fake `kitten`; this one proves the
 * real flags work, that the child lands in its own window, and that the window
 * is gone when the run is over.
 */

import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { closeWindowArgv, listWindowsArgv, windowIds } from "./kitty.ts";
import { finalOutput } from "./stream.ts";
import { runInKittyWindow, runProcess } from "./window.ts";

const live = process.env.LOXPP_SUBAGENT_LIVE === "1";
const listenOn = process.env.KITTY_LISTEN_ON ?? "";

const listWindows = (): Promise<{ code: number; stdout: string; stderr: string }> =>
  runProcess(listWindowsArgv(listenOn));

/** Tab id that holds a window, for placement assertions. */
function tabOf(listing: string, windowId: string): number | null {
  const osWindows = JSON.parse(listing) as Array<{ id: number; tabs: Array<{ id: number; windows: Array<{ id: number }> }> }>;
  for (const osWindow of osWindows) {
    for (const tab of osWindow.tabs) {
      for (const win of tab.windows) if (String(win.id) === windowId) return tab.id;
    }
  }
  return null;
}

test("a real kitty window runs the child and closes afterwards", { skip: !live && "set LOXPP_SUBAGENT_LIVE=1" }, async () => {
  const listing = await listWindows();
  assert.equal(listing.code, 0, listing.stderr);
  const parentWindow = windowIds(listing.stdout)[0];
  assert.ok(parentWindow, "the kitty instance has at least one window");

  const cwd = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-live-"));
  try {
    const run = await runInKittyWindow({
      session: { windowId: parentWindow, listenOn },
      cwd,
      title: "pi subagent: live test",
      header: ["pi subagent: live test"],
      // The child reports the window it runs in, so the assertion needs no race.
      command: [
        "/bin/sh",
        "-c",
        `printf '{"type":"message_end","message":{"role":"assistant","content":[{"type":"text","text":"%s"}],"usage":{"input":1,"output":1}}}\\n' "$KITTY_WINDOW_ID"; sleep 0.6; exit 5`,
      ],
      env: {},
      hold: false,
      keepFocus: true,
      signal: undefined,
      onProgress: () => {},
    });

    assert.equal(run.exitCode, 5);
    assert.equal(run.closedByUser, false);
    assert.match(run.windowId, /^\d+$/);
    assert.equal(finalOutput(run.state), run.windowId, "the child ran in the window kitty reported");

    const after = await listWindows();
    assert.equal(after.code, 0, after.stderr);
    assert.equal(windowIds(after.stdout).includes(run.windowId), false, "the window closed when the subagent exited");
  } finally {
    await fs.promises.rm(cwd, { recursive: true, force: true });
  }
});

test("the subagent window is a split in the parent's tab, not a new OS window", { skip: !live && "set LOXPP_SUBAGENT_LIVE=1" }, async () => {
  const listing = await listWindows();
  const parentWindow = windowIds(listing.stdout)[0];
  const osWindowsBefore = JSON.parse(listing.stdout).length;

  const cwd = await fs.promises.mkdtemp(path.join(os.tmpdir(), "loxpp-subagent-live-"));
  try {
    // hold: the window must survive the child so its placement can be read.
    const run = await runInKittyWindow({
      session: { windowId: parentWindow, listenOn },
      cwd,
      title: "pi subagent: placement test",
      header: ["pi subagent: placement test"],
      command: ["/bin/sh", "-c", "sleep 0.2; exit 0"],
      env: {},
      hold: true,
      keepFocus: true,
      signal: undefined,
      onProgress: () => {},
    });

    const after = await listWindows();
    assert.equal(after.code, 0, after.stderr);
    assert.equal(JSON.parse(after.stdout).length, osWindowsBefore, "no new OS window");
    assert.equal(tabOf(after.stdout, run.windowId), tabOf(after.stdout, parentWindow), "the child is in the parent's tab");

    await runProcess(closeWindowArgv(run.windowId, listenOn));
  } finally {
    await fs.promises.rm(cwd, { recursive: true, force: true });
  }
});
