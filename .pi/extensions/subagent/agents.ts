/**
 * Subagent definitions.
 *
 * An agent is a markdown file with YAML frontmatter:
 *
 *     ---
 *     name: scout
 *     description: Fast codebase reconnaissance
 *     tools: read, grep, find, ls, bash
 *     model: anthropic/claude-haiku-4-5
 *     ---
 *     system prompt body
 *
 * `tools` and `model` are optional. A `model` left out means the subagent
 * inherits the dispatching session's model.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { CONFIG_DIR_NAME, getAgentDir, parseFrontmatter } from "@earendil-works/pi-coding-agent";

export type AgentScope = "user" | "project" | "both";
export type AgentSource = "user" | "project";

export interface AgentConfig {
  name: string;
  description: string;
  tools?: string[];
  model?: string;
  systemPrompt: string;
  source: AgentSource;
  filePath: string;
}

export interface AgentDiscovery {
  agents: AgentConfig[];
  projectAgentsDir: string | null;
}

export function discoverAgents(cwd: string, scope: AgentScope): AgentDiscovery {
  const projectAgentsDir = findProjectAgentsDir(cwd);
  const userAgents = scope === "project" ? [] : loadAgents(path.join(getAgentDir(), "agents"), "user");
  const projectAgents = scope === "user" || !projectAgentsDir ? [] : loadAgents(projectAgentsDir, "project");

  // Project agents override user agents of the same name, because a project
  // that defines an agent expects its own definition to run.
  const byName = new Map<string, AgentConfig>();
  for (const agent of userAgents) byName.set(agent.name, agent);
  for (const agent of projectAgents) byName.set(agent.name, agent);

  return { agents: Array.from(byName.values()), projectAgentsDir };
}

export function formatAgentList(agents: AgentConfig[]): string {
  if (agents.length === 0) return "none";
  return agents.map((a) => `${a.name} (${a.source}): ${a.description}`).join("; ");
}

function loadAgents(dir: string, source: AgentSource): AgentConfig[] {
  let entries: fs.Dirent[];
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return [];
  }

  const agents: AgentConfig[] = [];
  for (const entry of entries) {
    if (!entry.name.endsWith(".md")) continue;
    if (!entry.isFile() && !entry.isSymbolicLink()) continue;

    const filePath = path.join(dir, entry.name);
    let content: string;
    try {
      content = fs.readFileSync(filePath, "utf-8");
    } catch {
      continue;
    }

    const { frontmatter, body } = parseFrontmatter<Record<string, unknown>>(content);
    if (typeof frontmatter.name !== "string" || typeof frontmatter.description !== "string") continue;

    agents.push({
      name: frontmatter.name,
      description: frontmatter.description,
      tools: parseToolList(frontmatter.tools),
      model: typeof frontmatter.model === "string" ? frontmatter.model : undefined,
      systemPrompt: body,
      source,
      filePath,
    });
  }
  return agents;
}

/**
 * `tools` is valid YAML written either as a list or as a comma-separated
 * string. A bad value yields no tool restriction rather than an error: one
 * broken agent file must not disable discovery of the others.
 */
function parseToolList(value: unknown): string[] | undefined {
  const raw = Array.isArray(value) ? value : typeof value === "string" ? value.split(",") : [];
  const tools = raw.filter((t): t is string => typeof t === "string").map((t) => t.trim()).filter(Boolean);
  return tools.length > 0 ? tools : undefined;
}

function findProjectAgentsDir(cwd: string): string | null {
  let dir = path.resolve(cwd);
  for (;;) {
    const candidate = path.join(dir, CONFIG_DIR_NAME, "agents");
    if (isDirectory(candidate)) return candidate;
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

function isDirectory(candidate: string): boolean {
  try {
    return fs.statSync(candidate).isDirectory();
  } catch {
    return false;
  }
}
