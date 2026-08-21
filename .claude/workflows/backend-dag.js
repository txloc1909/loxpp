export const meta = {
  name: 'backend-dag',
  description: 'Drive a DAG of backend implementation nodes to green, one PR per node, via implementer/reviewer/researcher agents. See notes/multi-agent-playbook.md.',
  whenToUse: 'Building a new Lox++ compiler backend (or any DAG-shaped, node-by-node implementation effort) with a multi-agent implementer/reviewer/researcher loop.',
  phases: [
    // meta.phases must stay a literal, so it cannot be derived from
    // args.nodes. Replace this list with the running mission's real node
    // ids/titles so the progress-tree preview is accurate. A phase() call
    // for an id missing here still works; it just gets its own unlabeled
    // progress group instead of a named preview.
    { title: 'C-RT', detail: 'C# runtime library, its tests, and the CLR link smoke test' },
    { title: 'C-N4', detail: 'straight-line CIL emit, --target clr, and the run harness' },
    { title: 'C-N5', detail: 'control flow, labels, and IL legality' },
    { title: 'C-N6', detail: 'functions and calls' },
    { title: 'C-N7', detail: 'closures and upvalues [BUG GATE]' },
    { title: 'C-N8', detail: 'classes, methods, super, and the shared depth-0 authority' },
    { title: 'C-N9', detail: 'aggregates, slices, membership, and iterators' },
    { title: 'C-N10', detail: 'match and enum dispatch' },
    { title: 'C-N11', detail: 'differential corpus gate, native vs JVM vs CLR' },
    { title: 'C-N12', detail: 'self-hosted interpreter gate [MISSION GATE]' },
  ],
}

// ---------------------------------------------------------------------------
// Mission configuration — supplied by the caller via `args`, not hardcoded.
// See notes/multi-agent-playbook.md for what each of these means and for the
// node specification structure `missionDir/nodes/<id>.md` must follow.
// ---------------------------------------------------------------------------

const cfg = args || {}

if (!cfg.missionDir) {
  throw new Error(
    'backend-dag: args.missionDir is required — an absolute path to a durable ' +
    'directory holding brief.md and nodes/*.md. Never point this at /tmp: on ' +
    'some hosts it is a memory filesystem a service cleans, and an agent that ' +
    'loses its instructions this way tends not to notice, let alone report it.'
  )
}
if (!cfg.nodes || typeof cfg.nodes !== 'object') {
  throw new Error('backend-dag: args.nodes is required — a map of node id -> { branch, title }.')
}

const MISSION = cfg.missionDir
const BRIEF = MISSION + '/brief.md'
const REPO = cfg.repo || '/var/home/loctran/personal/loxpp'
const GH = cfg.githubRepo || 'txloc1909/loxpp'
const NODES = cfg.nodes
// Human-readable label for what makes an analysis node's knowledge "foreign"
// — e.g. 'JVM', 'CLR' — used only in the reviewer's checklist wording.
const TARGET_LABEL = cfg.targetLabel || 'target-specific'
const DAG_DOC = cfg.dagDoc || (REPO + '/notes/backend-implementation-dag.md')
const OPCODE_DOC = cfg.opcodeDoc || (REPO + '/notes/bytecode-translation-problems.md')

const MAX_REVIEW_ROUNDS = 8      // hard stop on the implementer/reviewer loop
const DISPUTE_LIMIT = 3          // rounds a tag may stay disputed before the referee decides
const MAX_UNBLOCKS = 3           // researcher unblock attempts per node

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['pr_open', 'blocked_surprise', 'abort_dependency', 'failed'] },
    pr: { type: 'integer', description: 'PR number, or 0 when no PR exists yet' },
    branch: { type: 'string' },
    summary: { type: 'string', description: 'Simplified Technical English. What you built and how you proved it.' },
    checkpoint_evidence: { type: 'string', description: 'The real command output that proves the node checkpoint.' },
    head_sha: { type: 'string', description: 'The commit SHA now at the tip of the pushed branch on origin. Read it back with `git rev-parse origin/<branch>` after you push; do not report a local-only commit.' },
    blocker: { type: 'string', description: 'Only when status is blocked_surprise or abort_dependency. State the exact problem and what you already tried.' },
  },
  required: ['status', 'summary'],
}

const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    approved: { type: 'boolean', description: 'True only when you independently reproduced the checkpoint and no blocking finding is open.' },
    verified_independently: { type: 'boolean', description: 'True when you ran the checkpoint yourself in your own worktree and container.' },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          tag: { type: 'string', description: 'Stable tag, for example R1.' },
          severity: { type: 'string', enum: ['blocking', 'nit'] },
          summary: { type: 'string' },
          state: { type: 'string', enum: ['new', 'open', 'resolved', 'disputed'] },
          file: { type: 'string', description: 'Repo-relative path the finding anchors to. Give it for every finding.' },
        },
        required: ['tag', 'severity', 'summary', 'state'],
      },
    },
    disputed_tags: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
  },
  required: ['approved', 'verified_independently', 'summary'],
}

const FIX_SCHEMA = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['pushed', 'blocked_surprise', 'abort_dependency', 'failed'] },
    addressed_tags: { type: 'array', items: { type: 'string' } },
    rebutted_tags: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
    head_sha: { type: 'string', description: 'The commit SHA now at the tip of the pushed branch on origin. Read it back with `git rev-parse origin/<branch>` after you push; do not report a local-only commit.' },
    blocker: { type: 'string' },
  },
  required: ['status', 'summary'],
}

const RESEARCH_SCHEMA = {
  type: 'object',
  properties: {
    resolved: { type: 'boolean' },
    mission_status: { type: 'string', enum: ['continue', 'abort'] },
    guidance: { type: 'string', description: 'A concrete way forward for the implementer. Simplified Technical English.' },
    report: { type: 'string', description: 'Only when mission_status is abort. A detailed failure report and a list of promising directions.' },
  },
  required: ['resolved', 'mission_status', 'guidance'],
}

const REFEREE_SCHEMA = {
  type: 'object',
  properties: {
    decisions: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          tag: { type: 'string' },
          ruling: { type: 'string', enum: ['implementer', 'reviewer', 'other'] },
          instruction: { type: 'string', description: 'What must now happen. Binding on both agents.' },
        },
        required: ['tag', 'ruling', 'instruction'],
      },
    },
    mission_status: { type: 'string', enum: ['continue', 'abort'] },
    report: { type: 'string' },
  },
  required: ['decisions', 'mission_status'],
}

const MERGE_SCHEMA = {
  type: 'object',
  properties: {
    merged: { type: 'boolean' },
    ci_status: { type: 'string' },
    merge_commit: { type: 'string' },
    summary: { type: 'string' },
  },
  required: ['merged', 'summary'],
}

// ---------------------------------------------------------------------------
// Prompt builders
// ---------------------------------------------------------------------------

function common(id) {
  const n = NODES[id]
  return [
    'MISSION: build a Lox++ compiler backend, node by node.',
    'YOUR NODE: ' + id + ' - ' + n.title,
    'BRANCH: ' + n.branch,
    'REPO ROOT (host): ' + REPO,
    'GITHUB REPO: ' + GH,
    '',
    'Read these files before you act, in this order:',
    '  1. ' + BRIEF + '  (the mission rules; they are binding)',
    '  2. ' + MISSION + '/nodes/' + id + '.md  (your node specification)',
    '  3. ' + DAG_DOC,
    '  4. ' + OPCODE_DOC + '  (authoritative opcode semantics)',
    '  5. ' + REPO + '/AGENTS.md',
    '  6. ' + REPO + '/notes/multi-agent-playbook.md',
    '',
    'If any of these files is missing or unreadable, STOP immediately and report',
    'status "blocked_surprise" with the exact path and error. Do not continue as',
    'if a missing file were optional — an agent working from a partial brief is',
    'indistinguishable from one working correctly until its output is wrong.',
    '',
    'Hard rules:',
    '  - Write every GitHub message and every returned string in ASD-STE100 Simplified Technical English.',
    '  - Start every GitHub message with your role tag.',
    '  - You work alone and without supervision. Never ask a question and wait. Decide, act, and record the decision.',
    '  - Use `gh` as user txloc1909. It is already authenticated with ADMIN rights.',
    '  - Run builds and tests inside the `loxpp-dev-env-managed` container. Never use `-it`.',
    '  - Never touch another agent worktree, and never touch the human `loxpp-dev` distrobox container.',
    '',
  ].join('\n')
}

function implPrompt(id) {
  return common(id) + [
    'ROLE: IMPLEMENTER. Tag every GitHub message with "[Implementer]".',
    '',
    'TASK: deliver the first working version of this node and open its pull request.',
    '',
    'Steps:',
    '  0. RESUME CHECK FIRST. A previous attempt may have been interrupted by a reboot.',
    '     Run `git fetch origin` and `git worktree list`, and check',
    '     `git ls-remote --heads origin ' + NODES[id].branch + '` and',
    '     `git ls-remote --heads origin wip/' + NODES[id].branch + '`.',
    '     - If the worktree `.claude/worktrees/loxpp-<branch-with-dashes>` already exists, use it.',
    '     - If the branch or a `wip/` twin already exists on origin, check it out and CONTINUE that work.',
    '       Read the existing commits first. Do not start again from nothing.',
    '     - A `wip/` branch holds an interrupted commit. Take its content, then delete the `wip/` branch',
    '       from origin once the real branch carries the work.',
    '     Only when nothing exists, go to step 1.',
    '  1. `cd ' + REPO + ' && git fetch origin`. Create your worktree from `origin/main`:',
    '     `git worktree add .claude/worktrees/loxpp-<branch-with-dashes> -b ' + NODES[id].branch + ' origin/main`',
    '  2. Read the existing code you must fit into. Match its style and naming. Match the style of code',
    '     that predates this mission — do not copy a defect or a mission-scoped habit (like citing a',
    '     review-round number) from a recent, mission-authored change just because it is nearby.',
    '  3. Write the code. Commit atomically with Conventional Commits. Every commit must build.',
    '     Commit as soon as the code compiles, and push after each commit. Do not hold a large',
    '     unit of work uncommitted; an agent that reaches its step limit before it commits loses',
    '     the work and costs a full review round.',
    '     Write each code comment for a maintainer who cannot see this pull request, this review',
    '     thread, or this mission. A comment that needs the thread to make sense is in the wrong',
    '     place: the invariant or trade-off goes in the code, the reason the code changed at this',
    '     time goes in the commit message body and the pull request reply.',
    '  4. Verify your node checkpoint inside the container. Capture the real command output.',
    '  5. Format and lint: clang-format on src and test, clang-tidy on src.',
    '  6. Push the branch, then handle the PR:',
    '     - Check first: `gh pr list --repo ' + GH + ' --head ' + NODES[id].branch + ' --state open`.',
    '     - If a PR already exists, do **NOT** open another. Push to the branch, read the whole',
    '       thread including every `[Orchestrator]` and `[Researcher] REFEREE DECISION` comment,',
    '       obey those decisions, resolve every open finding, and post one comment that lists each',
    '       finding tag and its state. Then return that PR number.',
    '     - Only when no PR exists, open one against `main`.',
    '',
    'The PR body must contain, in this order:',
    '  - The line "[Implementer] Node ' + id + ' - ' + NODES[id].title + '".',
    '  - The checkpoint, copied from your node specification.',
    '  - The evidence: the real command output, in a fenced block. Do not paraphrase it.',
    '  - The design choices you made and why.',
    '  - Anything you decided to leave for a later node.',
    '',
    'Do NOT merge the PR. A reviewer must approve it first.',
    'Do NOT wait for CI at this step. Return as soon as the PR is open.',
    'The reviewer starts when you return, and CI runs at the same time. A later step watches CI',
    'and requires it to be green before the merge. Time you spend to poll CI here delays the review.',
    '',
    'Escalation:',
    '  - If you meet a problem the plan did not anticipate, and you cannot solve it inside this node,',
    '    set status to "blocked_surprise". State the exact problem, the evidence, and everything you tried.',
    '  - If your work proves a NEW dependency between two DAG nodes that makes the plan invalid,',
    '    set status to "abort_dependency".',
    '  - Do not use these for ordinary difficulty. Solve ordinary problems yourself.',
    '',
    'Return status "pr_open" with the PR number when the PR is open.',
  ].join('\n')
}

function resumePrompt(id, blocker, guidance) {
  return common(id) + [
    'ROLE: IMPLEMENTER. Tag every GitHub message with "[Implementer]".',
    '',
    'You reported this blocker earlier:',
    '---',
    blocker || '(none recorded)',
    '---',
    '',
    'The researcher investigated. This guidance is binding:',
    '---',
    guidance,
    '---',
    '',
    'TASK: apply the guidance, finish the node, and open its pull request (or update it if it already exists).',
    'Follow the same steps and the same PR body format as the first attempt.',
    'Post a comment on the PR that records the blocker and the resolution, so the record is public.',
    'Set status "blocked_surprise" again only if the guidance does not work, and say exactly why.',
  ].join('\n')
}

function reviewPrompt(id, pr, round) {
  return common(id) + [
    'ROLE: REVIEWER. Tag every GitHub message with "[Reviewer]".',
    'REVIEW ROUND: ' + round + ' of at most ' + MAX_REVIEW_ROUNDS + '.',
    'PULL REQUEST: #' + pr,
    '',
    'TASK: review this PR adversarially. Assume it is wrong until you prove it is right.',
    '',
    'Steps:',
    '  1. Read the whole thread: `gh pr view ' + pr + ' --repo ' + GH + ' --comments`',
    '     and the diff: `gh pr diff ' + pr + ' --repo ' + GH + '`.',
    '  2. Make your OWN worktree from the PR branch and verify the checkpoint YOURSELF:',
    '     `cd ' + REPO + ' && git fetch origin && git worktree add .claude/worktrees/loxpp-review-' + id.toLowerCase() + ' ' + NODES[id].branch + '`',
    '     Run the checkpoint commands in the container. Do not trust the evidence in the PR body.',
    '     If a worktree for that path already exists, reuse it and `git pull` inside it.',
    '  3. Attack the change. Look for:',
    '     - A checkpoint that passes for the wrong reason, or a test that cannot fail.',
    '       Prove it: remove the fix, rerun the test, and confirm it fails before trusting that it can.',
    '     - Semantics that differ from `src/vm.cpp`, `src/compiler.cpp`, or `spec/`.',
    '     - Opcode operand widths that differ from `src/debug.cpp`.',
    '     - Non-determinism: pointer order, hash order, or map iteration order in generated output.',
    '     - Hard-coded probe-specific behaviour instead of general logic.',
    '     - ' + TARGET_LABEL + ' knowledge leaking into a target-independent analysis node.',
    '     - Missed edge cases from ' + OPCODE_DOC + '.',
    '     - Violations of the resolved design decisions in the brief.',
    '     - A new code comment that cites a PR number, a review round, or a finding tag (e.g. "R5",',
    '       "PR #109", "round 3") — that citation belongs in the commit message and the PR reply, not',
    '       in the source. Flag it as a finding.',
    '     - Code that will not scale to `bootstrap/loxpp_interpreter.lox`.',
    '     - A claim in the PR body or a reply that you did not personally reproduce (a tool result, a',
    '       "clean" report, a completeness claim) — run it yourself before trusting it.',
    '  4. Post each finding as an INLINE comment anchored to the code. Do not describe a location',
    '     in words when you can anchor to it. Section 7 of the brief has the exact commands.',
    '     Short form:',
    '       SHA=$(gh pr view ' + pr + ' --repo ' + GH + ' --json headRefOid -q .headRefOid)',
    '       gh api -X POST repos/' + GH + '/pulls/' + pr + '/comments \\',
    '         -f commit_id=$SHA -f path=<file> -F line=<line> -f side=RIGHT -f body=<text>',
    '     Use `-f subject_type=file` with no line when the finding is about a whole file, about',
    '     something that is ABSENT, or about a line outside the diff.',
    '     Give each finding a stable tag: R1, R2, R3, and so on. Keep the same tag for the same',
    '     finding across rounds. Start the body with the tag, for example',
    '     "[Reviewer] R1 (blocking) - ...". Mark each finding "blocking" or "nit".',
    '     For a blocking finding, state the failure case concretely: the input, and the wrong result.',
    '     Post ONE top-level summary comment per round with `gh pr comment`, and keep the detail',
    '     inline. Do not repeat the inline text in the summary.',
    '  5. When, and only when, you reproduced the checkpoint yourself AND no blocking finding is open,',
    '     post a comment whose FIRST LINE is exactly:',
    '     [Reviewer] APPROVED',
    '     Follow it with a short summary of what you verified.',
    '',
    'Do not approve on trust. Do not approve if you could not run the checkpoint.',
    'Do not merge. The implementer merges after your approval.',
    'Do not fix the code yourself. Report; the implementer fixes.',
    '',
    'If the implementer rebutted a finding and you still disagree, keep the tag and mark it "disputed".',
    'After ' + DISPUTE_LIMIT + ' rounds a researcher will referee it and the decision will be binding.',
    '',
    'Return the structured result. Set approved true only when you posted the APPROVED comment.',
  ].join('\n')
}

function fixPrompt(id, pr, round, reviewSummary) {
  return common(id) + [
    'ROLE: IMPLEMENTER. Tag every GitHub message with "[Implementer]".',
    'REVIEW ROUND: ' + round + '.',
    'PULL REQUEST: #' + pr,
    '',
    'The reviewer posted findings. Their summary:',
    '---',
    reviewSummary,
    '---',
    '',
    'TASK: resolve every blocking finding.',
    '',
    'Steps:',
    '  1. Read the full thread. The findings are INLINE comments, so list them with their ids:',
    '       gh api repos/' + GH + '/pulls/' + pr + '/comments --paginate \\',
    '         --jq \'.[] | "\\(.id)\\t\\(.path):\\(.line)\\t\\(.body[0:100])"\'',
    '     Also read the top-level summary: `gh pr view ' + pr + ' --repo ' + GH + ' --comments`.',
    '  2. Go back to your worktree: `' + REPO + '/.claude/worktrees/loxpp-<branch-with-dashes>`.',
    '     If it is gone, recreate it from the branch.',
    '  3. For each finding, do ONE of:',
    '     - Fix it. Commit atomically. Reply IN THAT FINDING THREAD with the commit hash.',
    '     - Rebut it with evidence. Reply IN THAT FINDING THREAD with the reason and the proof.',
    '       Rebut only when you are sure. Cite `src/`, `spec/`, or real command output.',
    '     Reply inside the thread, not as a new top-level comment:',
    '       gh api -X POST repos/' + GH + '/pulls/' + pr + '/comments/<comment-id>/replies \\',
    '         -f body=\'[Implementer] R1 - fixed in <sha>. <what changed and why>\'',
    '     The reader must see your answer next to the code it is about.',
    '  4. Re-run the checkpoint and the regression commands. Format and lint.',
    '  5. **Re-read the thread before you push.** A decision can arrive while you work. Run',
    '     `gh pr view ' + pr + ' --repo ' + GH + ' --comments` again, and look for a comment that starts',
    '     with "[Orchestrator]" or "[Researcher] REFEREE DECISION". Such a comment is BINDING and',
    '     it overrides the reviewer finding it answers. Also re-read section 8 of the brief, because',
    '     the orchestrator writes every ruling there too.',
    '     If a ruling arrived after you started, drop the work it forbids, even when that work is',
    '     finished and passes its tests. Say so in your reply. Working code that breaks a ruling',
    '     is still wrong.',
    '  6. Push. Then RETURN. Do **not** wait for CI, and never poll it in a loop.',
    '     The reviewer starts the moment you return, and it verifies the checkpoint itself.',
    '     CI is watched once, at the merge step, after the approval. Every step you spend to poll',
    '     CI here is a step the review is not happening. Do not run a placeholder command such as',
    '     `echo still-waiting` to pass time; each one costs a full model step.',
    '  7. Post one summary comment that lists every tag and its state.',
    '',
    'Do not merge. Escalate with "blocked_surprise" or "abort_dependency" only under the brief rules.',
  ].join('\n')
}

function repushPrompt(id, pr, sha) {
  return common(id) + [
    'ROLE: IMPLEMENTER. Tag every GitHub message with "[Implementer]".',
    'PULL REQUEST: #' + pr,
    '',
    'The branch tip on origin is still ' + sha + '. That is the same commit the last review',
    'round already read. So one of these is true, and you must find out which:',
    '  a. You finished work in your worktree and it is not committed.',
    '  b. You committed and did not push.',
    '  c. You pushed to the wrong branch or the wrong remote.',
    '  d. The work is really pushed and the reported SHA was wrong.',
    '',
    'TASK: get the real state onto origin, then report the true SHA.',
    '',
    'Steps:',
    '  1. Go to your worktree: `' + REPO + '/.claude/worktrees/loxpp-<branch-with-dashes>`.',
    '     Run `git status`, `git log --oneline -5`, and `git log --oneline origin/' + NODES[id].branch + ' -3`.',
    '  2. If uncommitted work is present and it compiles, commit it now, atomically.',
    '     Do not wait to finish a larger unit of work first. That is how two full review',
    '     rounds were lost on the last mission.',
    '  3. Push to `origin ' + NODES[id].branch + '`.',
    '  4. Read the tip back: `git fetch origin && git rev-parse origin/' + NODES[id].branch + '`.',
    '     Return that value in head_sha. Do not report a local-only commit.',
    '  5. If case (d) is true and nothing needed a push, return the real SHA and say so.',
    '',
    'Return status "pushed" with the true head_sha.',
  ].join('\n')
}

function unblockPrompt(id, blocker) {
  return common(id) + [
    'ROLE: RESEARCHER. Tag every GitHub message with "[Researcher]".',
    '',
    'You are called for condition 2: the implementer found a problem the plan did not anticipate.',
    '',
    'The blocker:',
    '---',
    blocker,
    '---',
    '',
    'TASK: unblock the implementer with a concrete, verified way forward.',
    '',
    'Steps:',
    '  1. Reproduce the problem yourself. Make your own worktree if you need one.',
    '     Do not accept the description without proof.',
    '  2. Find the cause. Read `src/`, `spec/`, and the notes. Search the web for prior art if it helps.',
    '  3. Choose the smallest change that keeps the plan valid. Prefer a solution inside the node.',
    '  4. Prove it works with a minimal experiment. Paste the real output in your guidance.',
    '  5. Post your finding and your decision as a comment on the node PR, if a PR exists.',
    '',
    'State a CONSTRAINT the fix must satisfy, not a specific mechanism to implement it — a mechanism',
    'stated too early gets disproved and rewritten within one review round; a constraint survives that.',
    '',
    'Set mission_status to "abort" ONLY when both hold:',
    '  - The problem is a real gap in the planned solution, not ordinary difficulty.',
    '  - You found no alternative after a serious attempt.',
    'When you abort, write a detailed report for the human supervisor in the report field:',
    'the failure, the evidence, the options you tried, why each failed, and a ranked list of',
    'promising directions. Write it in Simplified Technical English.',
    '',
    'Otherwise set mission_status to "continue" and give binding guidance.',
  ].join('\n')
}

function refereePrompt(id, pr, tags, reason) {
  const why = reason === 'stagnation'
    ? [
        'You are called because the review does not converge. The implementer accepts every finding,',
        'so there is no dispute, but the SAME file produced a new blocking finding in ' + DISPUTE_LIMIT + ' rounds',
        'in a row. Each fix repaired one symptom, and the next round found the next symptom of the',
        'same design. The file is: ' + tags.join(', '),
        '',
        'Decide ONE of these, and say which:',
        '  a. The design of that code is wrong. State the redesign that closes the whole class of',
        '     defect, so a fourth round is not needed.',
        '  b. The remaining problems do not block this node. State which findings become follow-up',
        '     work, and instruct the reviewer to approve when the checkpoint holds.',
        'Do not simply agree with both agents. A decision that leaves the loop running is a failure.',
      ]
    : [
        'You are called for condition 1: the implementer and the reviewer did not agree after ' + DISPUTE_LIMIT + ' rounds.',
        'The disputed findings are: ' + tags.join(', '),
      ]
  return common(id) + [
    'ROLE: RESEARCHER, acting as REFEREE. Tag every GitHub message with "[Researcher]".',
    'PULL REQUEST: #' + pr,
    '',
    ...why,
    '',
    'TASK: decide each dispute. Your decision is final and binding on both agents.',
    '',
    'Steps:',
    '  1. Read the whole thread: `gh pr view ' + pr + ' --repo ' + GH + ' --comments`.',
    '  2. Read the diff: `gh pr diff ' + pr + ' --repo ' + GH + '`.',
    '  3. For each disputed tag, VERIFY THE FACTS YOURSELF FIRST — run the code, read `spec/` and',
    '     `src/`. Do not take either agent\'s word for it. The decision priority is spec > implementation > design notes.',
    '  4. THEN rule: assign each tag to the implementer, the reviewer, or a third option.',
    '  5. THEN bound the next round: post ONE comment on the PR giving, for each tag, the ruling, the',
    '     reason, and the exact action that must now happen — specific enough that a fourth round',
    '     cannot repeat with the same ambiguity. Start the comment with "[Researcher] REFEREE DECISION".',
    '',
    'Be decisive. A dispute that you leave open blocks the mission.',
    'Set mission_status "abort" only under the brief rules, and then write the full report.',
  ].join('\n')
}

function mergePrompt(id, pr) {
  return common(id) + [
    'ROLE: IMPLEMENTER. Tag every GitHub message with "[Implementer]".',
    'PULL REQUEST: #' + pr + ' - the reviewer approved it.',
    '',
    'TASK: land the PR.',
    '',
    'Steps:',
    '  1. Confirm the approval: `gh pr view ' + pr + ' --repo ' + GH + ' --comments`.',
    '     Look for a comment whose first line is exactly "[Reviewer] APPROVED".',
    '  2. Rebase on the latest `main` in your worktree: `git fetch origin && git rebase origin/main`.',
    '     Fix any conflict. Re-run the checkpoint after the rebase. Force-push with `--force-with-lease`.',
    '  3. Wait for CI. Your shell tool stops a command after 10 minutes, and CI needs longer,',
    '     so NEVER start one long wait. Use short waits and repeat them across separate calls:',
    '',
    '     ```',
    '     end=$((SECONDS+420))',
    '     while [ $SECONDS -lt $end ]; do',
    '       s=$(gh pr checks ' + pr + ' --repo ' + GH + ' 2>&1)',
    '       printf "%s\\n" "$s" | grep -qE "pending|in_progress" || { printf "%s\\n" "$s"; exit 0; }',
    '       sleep 45',
    '     done',
    '     printf "%s\\n" "$s"; echo "STILL PENDING"',
    '     ```',
    '',
    '     Each call takes at most 7 minutes and always returns output you can read. If it prints',
    '     STILL PENDING, call it again. Give CI up to 45 minutes in total, that is about 6 calls.',
    '     A `gh pr checks` exit status of 8 means checks are still pending, not an error.',
    '     If a check fails, read the log with',
    '     `gh run view <run-id> --log-failed --repo ' + GH + '`, fix the cause, push, and wait again.',
    '     Repeat until CI is green. A red CI is never acceptable.',
    '  4. Squash-merge, bypassing the review rule because one account cannot approve its own PR:',
    '     `gh pr merge ' + pr + ' --repo ' + GH + ' --squash --admin --delete-branch`',
    '  5. Clean up: `cd ' + REPO + ' && git worktree remove .claude/worktrees/loxpp-<branch-with-dashes> --force`',
    '     and remove the reviewer worktree `.claude/worktrees/loxpp-review-' + id.toLowerCase() + '` if it exists.',
    '     Then `git fetch origin && git branch -D ' + NODES[id].branch + '` if the local branch remains.',
    '  6. Verify `main` is green: `cd ' + REPO + ' && git fetch origin && git log --oneline -3 origin/main`.',
    '',
    'Return merged true only when the squash-merge succeeded.',
  ].join('\n')
}

// ---------------------------------------------------------------------------
// Node driver
// ---------------------------------------------------------------------------

function isAbort(r) {
  return r && (r.status === 'abort_dependency' || r.mission_status === 'abort')
}

async function runNode(id, res) {
  const ph = id
  const result = { node: id, title: NODES[id].title, branch: NODES[id].branch, rounds: 0, pr: 0, status: 'unknown', log: [] }

  // --- resume: a previous session may already have landed or opened this node
  if (res && res.merged) {
    log('node ' + id + ': already merged as PR #' + (res.pr || '?') + '; skipped')
    result.pr = res.pr || 0
    result.status = 'merged'
    result.log.push('resume: already merged before this run')
    return result
  }

  let impl
  if (res && res.pr) {
    result.pr = res.pr
    impl = { status: 'pr_open', pr: res.pr, summary: 'resumed: PR #' + res.pr + ' was already open' }
    result.log.push('resume: entered at ' + (res.phase || 'review') + ' on PR #' + res.pr)
    log('node ' + id + ': resuming on open PR #' + res.pr)
    if (res.phase === 'merge') {
      const mg0 = await agent(mergePrompt(id, res.pr), {
        model: 'sonnet', label: 'merge:' + id, phase: ph, schema: MERGE_SCHEMA,
      })
      if (!mg0) { result.status = 'agent_died'; return result }
      result.log.push('merge(resumed): merged=' + mg0.merged + ' - ' + mg0.summary)
      result.status = mg0.merged ? 'merged' : 'merge_failed'
      return result
    }
  } else {
    log('node ' + id + ': implementer starts')
    impl = await agent(implPrompt(id), { model: 'sonnet', label: 'impl:' + id, phase: ph, schema: IMPL_SCHEMA })
    if (!impl) { result.status = 'agent_died'; return result }
    result.log.push('impl-1: ' + impl.status + ' - ' + impl.summary)
    if (impl.pr) result.pr = impl.pr
  }

  // --- unblock loop -------------------------------------------------------
  let unblocks = 0
  while (impl && impl.status === 'blocked_surprise' && unblocks < MAX_UNBLOCKS) {
    unblocks += 1
    log('node ' + id + ': researcher unblocks (attempt ' + unblocks + ')')
    const res = await agent(unblockPrompt(id, impl.blocker || impl.summary), {
      model: 'fable', label: 'research:' + id + ':' + unblocks, phase: ph, schema: RESEARCH_SCHEMA,
    })
    if (!res) { result.status = 'agent_died'; return result }
    result.log.push('research-' + unblocks + ': ' + res.mission_status + ' - ' + res.guidance)
    if (res.mission_status === 'abort') {
      result.status = 'abort'
      result.abort_report = res.report || res.guidance
      return result
    }
    impl = await agent(resumePrompt(id, impl.blocker, res.guidance), {
      model: 'sonnet', label: 'impl:' + id + ':resume' + unblocks, phase: ph, schema: IMPL_SCHEMA,
    })
    if (!impl) { result.status = 'agent_died'; return result }
    result.log.push('impl-resume-' + unblocks + ': ' + impl.status + ' - ' + impl.summary)
    if (impl.pr) result.pr = impl.pr
  }

  if (isAbort(impl)) {
    result.status = 'abort'
    result.abort_report = impl.blocker || impl.summary
    return result
  }
  if (!impl || impl.status !== 'pr_open' || !result.pr) {
    result.status = 'no_pr'
    result.detail = impl ? (impl.blocker || impl.summary) : 'implementer returned nothing'
    return result
  }

  const pr = result.pr
  log('node ' + id + ': PR #' + pr + ' is open; review starts')

  // --- review loop --------------------------------------------------------
  const disputeStreak = {}
  const fileStreak = {}   // consecutive rounds a file yielded a blocking finding
  let approved = false
  let reviewedSha = null  // branch tip the last review round actually read
  let lastFixSha = impl.head_sha || null

  for (let round = 1; round <= MAX_REVIEW_ROUNDS; round++) {
    result.rounds = round

    // A review round against an unchanged branch tip can only repeat its own
    // findings, and a review round is the most expensive step in this loop.
    // Three rounds were lost this way on the last mission, each time an
    // implementer reached its step limit after the code compiled but before it
    // could commit, push, or reply. Send the implementer back for the push
    // instead of paying for a review that reads the same commit twice.
    let repushes = 0
    while (round > 1 && lastFixSha && lastFixSha === reviewedSha && repushes < 2) {
      repushes += 1
      log('node ' + id + ': branch tip did not move; implementer re-pushes (attempt ' + repushes + ')')
      const rp = await agent(repushPrompt(id, pr, reviewedSha), {
        model: 'sonnet', label: 'impl:' + id + ':repush' + round + '-' + repushes, phase: ph, schema: FIX_SCHEMA,
      })
      if (!rp) { result.status = 'agent_died'; return result }
      result.log.push('repush-' + round + '-' + repushes + ': ' + rp.status + ' sha=' + (rp.head_sha || '?'))
      if (isAbort(rp)) {
        result.status = 'abort'
        result.abort_report = rp.blocker || rp.summary
        return result
      }
      if (rp.head_sha) lastFixSha = rp.head_sha
    }

    const rev = await agent(reviewPrompt(id, pr, round), {
      model: 'opus', label: 'review:' + id + ':r' + round, phase: ph, schema: REVIEW_SCHEMA,
    })
    if (!rev) { result.status = 'agent_died'; return result }
    reviewedSha = lastFixSha
    result.log.push('review-' + round + ': approved=' + rev.approved + ' verified=' + rev.verified_independently + ' - ' + rev.summary)

    if (rev.approved && rev.verified_independently) { approved = true; break }

    // track disputes
    const disputed = rev.disputed_tags || []
    const seen = {}
    for (const t of disputed) {
      seen[t] = true
      disputeStreak[t] = (disputeStreak[t] || 0) + 1
    }
    for (const t of Object.keys(disputeStreak)) {
      if (!seen[t]) disputeStreak[t] = 0
    }
    const stuck = Object.keys(disputeStreak).filter(t => disputeStreak[t] >= DISPUTE_LIMIT)

    // Stagnation: no dispute, but the same file keeps yielding a new blocking
    // finding. The implementer patches one symptom, the reviewer finds the next
    // symptom of the same design, and nothing escalates because both agree.
    const blockingFiles = {}
    for (const f of (rev.findings || [])) {
      if (f.severity === 'blocking' && f.file) blockingFiles[f.file] = true
    }
    for (const f of Object.keys(blockingFiles)) {
      fileStreak[f] = (fileStreak[f] || 0) + 1
    }
    for (const f of Object.keys(fileStreak)) {
      if (!blockingFiles[f]) fileStreak[f] = 0
    }
    const stagnant = Object.keys(fileStreak).filter(f => fileStreak[f] >= DISPUTE_LIMIT)

    if (stuck.length === 0 && stagnant.length > 0) {
      log('node ' + id + ': referee breaks stagnation on ' + stagnant.join(', '))
      const ref = await agent(refereePrompt(id, pr, stagnant, 'stagnation'), {
        model: 'fable', label: 'referee:' + id + ':stagnation' + round, phase: ph, schema: REFEREE_SCHEMA,
      })
      if (!ref) { result.status = 'agent_died'; return result }
      result.log.push('referee-stagnation-' + round + ': ' + JSON.stringify(ref.decisions || []))
      if (ref.mission_status === 'abort') {
        result.status = 'abort'
        result.abort_report = ref.report || 'referee aborted'
        return result
      }
      for (const f of stagnant) fileStreak[f] = 0
    }

    if (stuck.length > 0) {
      log('node ' + id + ': referee decides ' + stuck.join(', '))
      const ref = await agent(refereePrompt(id, pr, stuck, 'dispute'), {
        model: 'fable', label: 'referee:' + id + ':r' + round, phase: ph, schema: REFEREE_SCHEMA,
      })
      if (!ref) { result.status = 'agent_died'; return result }
      result.log.push('referee-' + round + ': ' + JSON.stringify(ref.decisions || []))
      if (ref.mission_status === 'abort') {
        result.status = 'abort'
        result.abort_report = ref.report || 'referee aborted'
        return result
      }
      for (const t of stuck) disputeStreak[t] = 0
    }

    let fix = await agent(fixPrompt(id, pr, round, rev.summary), {
      model: 'sonnet', label: 'impl:' + id + ':fix' + round, phase: ph, schema: FIX_SCHEMA,
    })
    if (!fix) { result.status = 'agent_died'; return result }
    result.log.push('fix-' + round + ': ' + fix.status + ' - ' + fix.summary)
    if (fix.head_sha) lastFixSha = fix.head_sha

    let fixUnblocks = 0
    while (fix && fix.status === 'blocked_surprise' && fixUnblocks < MAX_UNBLOCKS) {
      fixUnblocks += 1
      const res = await agent(unblockPrompt(id, fix.blocker || fix.summary), {
        model: 'fable', label: 'research:' + id + ':fix' + round + '-' + fixUnblocks, phase: ph, schema: RESEARCH_SCHEMA,
      })
      if (!res) { result.status = 'agent_died'; return result }
      result.log.push('research-fix-' + round + '-' + fixUnblocks + ': ' + res.mission_status)
      if (res.mission_status === 'abort') {
        result.status = 'abort'
        result.abort_report = res.report || res.guidance
        return result
      }
      fix = await agent(resumePrompt(id, fix.blocker, res.guidance), {
        model: 'sonnet', label: 'impl:' + id + ':fixresume' + round + '-' + fixUnblocks, phase: ph, schema: FIX_SCHEMA,
      })
      if (!fix) { result.status = 'agent_died'; return result }
      result.log.push('fix-resume-' + round + '-' + fixUnblocks + ': ' + fix.status)
      if (fix.head_sha) lastFixSha = fix.head_sha
    }

    if (isAbort(fix)) {
      result.status = 'abort'
      result.abort_report = fix.blocker || fix.summary
      return result
    }
  }

  if (!approved) {
    result.status = 'review_exhausted'
    return result
  }

  // --- merge --------------------------------------------------------------
  log('node ' + id + ': approved; merge starts')
  const mg = await agent(mergePrompt(id, pr), {
    model: 'sonnet', label: 'merge:' + id, phase: ph, schema: MERGE_SCHEMA,
  })
  if (!mg) { result.status = 'agent_died'; return result }
  result.log.push('merge: merged=' + mg.merged + ' ci=' + (mg.ci_status || '?') + ' - ' + mg.summary)
  result.status = mg.merged ? 'merged' : 'merge_failed'
  result.ci_status = mg.ci_status
  return result
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

const stages = cfg.stages || []
const resumeMap = cfg.resume || {}
const all = []

for (let s = 0; s < stages.length; s++) {
  const stage = stages[s]
  log('stage ' + (s + 1) + '/' + stages.length + ': ' + stage.join(', '))
  phase(stage[0])
  const results = await parallel(stage.map(id => () => runNode(id, resumeMap[id])))
  const clean = results.filter(Boolean)
  all.push(...clean)

  const aborted = clean.filter(r => r.status === 'abort')
  if (aborted.length > 0) {
    log('MISSION ABORT requested by node(s): ' + aborted.map(r => r.node).join(', '))
    return { stopped: true, reason: 'abort', stage: s + 1, results: all }
  }
  const bad = clean.filter(r => r.status !== 'merged')
  if (bad.length > 0) {
    log('stage ' + (s + 1) + ' did not land: ' + bad.map(r => r.node + '=' + r.status).join(', '))
    return { stopped: true, reason: 'stage_incomplete', stage: s + 1, results: all }
  }
  log('stage ' + (s + 1) + ' merged: ' + clean.map(r => r.node + ' #' + r.pr).join(', '))
}

return { stopped: false, reason: 'all_stages_merged', results: all }
