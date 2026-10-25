/**
 * Decoding of a child's `pi --mode json` event stream.
 *
 * Structural types only and no package imports: the parent reads a handful of
 * fields and re-serializes nothing, so the unit tests load this module with
 * plain `node --test`.
 */

export interface UsageStats {
  input: number;
  output: number;
  cacheRead: number;
  cacheWrite: number;
  cost: number;
  contextTokens: number;
  turns: number;
}

export interface StreamPart {
  type: string;
  text?: string;
  name?: string;
  arguments?: Record<string, unknown>;
}

export interface StreamMessage {
  role: string;
  content: StreamPart[];
  usage?: Record<string, unknown>;
  model?: string;
  stopReason?: string;
  errorMessage?: string;
}

export interface StreamState {
  messages: StreamMessage[];
  usage: UsageStats;
  model?: string;
  stopReason?: string;
  errorMessage?: string;
  /**
   * True between `agent_end` and the next `agent_start`. An interactive child
   * in this state waits at its prompt for the person at its keyboard.
   */
  idle: boolean;
}

export function emptyState(): StreamState {
  return {
    messages: [],
    usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, cost: 0, contextTokens: 0, turns: 0 },
    idle: false,
  };
}

/**
 * Incremental JSONL reader. A chunk boundary can fall inside a line, so bytes
 * stay buffered until the newline arrives; `end()` flushes the last line.
 */
export class EventStream {
  readonly state: StreamState = emptyState();
  private buffer = "";

  /** Feed a stdout chunk. Returns true when the state changed. */
  push(chunk: string): boolean {
    this.buffer += chunk;
    let changed = false;
    let index = this.buffer.indexOf("\n");
    while (index >= 0) {
      const line = this.buffer.slice(0, index);
      this.buffer = this.buffer.slice(index + 1);
      if (applyLine(this.state, line)) changed = true;
      index = this.buffer.indexOf("\n");
    }
    return changed;
  }

  end(): boolean {
    const rest = this.buffer;
    this.buffer = "";
    return applyLine(this.state, rest);
  }
}

function applyLine(state: StreamState, line: string): boolean {
  if (!line.trim()) return false;
  let event: unknown;
  try {
    event = JSON.parse(line);
  } catch {
    return false;
  }
  return applyEvent(state, event);
}

/** `message_end` carries a finished message; `tool_result_end` a tool result. */
function applyEvent(state: StreamState, event: unknown): boolean {
  const record = asRecord(event);
  // The run boundaries tell the parent whether the child is working or waits at
  // its prompt, which is what an interactive window is for.
  if (record.type === "agent_start" || record.type === "agent_end") {
    const idle = record.type === "agent_end";
    if (state.idle === idle) return false;
    state.idle = idle;
    return true;
  }
  if (record.type !== "message_end" && record.type !== "tool_result_end") return false;
  const message = asRecord(record.message);
  if (typeof message.role !== "string") return false;

  const entry: StreamMessage = { role: message.role, content: asParts(message.content) };
  if (message.model !== undefined) entry.model = String(message.model);
  if (typeof message.stopReason === "string") entry.stopReason = message.stopReason;
  if (typeof message.errorMessage === "string") entry.errorMessage = message.errorMessage;
  if (message.usage !== undefined) entry.usage = asRecord(message.usage);
  state.messages.push(entry);

  if (entry.role !== "assistant") return true;

  const usage = asRecord(message.usage);
  state.usage.turns++;
  state.usage.input += num(usage.input);
  state.usage.output += num(usage.output);
  state.usage.cacheRead += num(usage.cacheRead);
  state.usage.cacheWrite += num(usage.cacheWrite);
  state.usage.cost += num(asRecord(usage.cost).total);
  const contextTokens = num(usage.totalTokens);
  if (contextTokens > 0) state.usage.contextTokens = contextTokens;
  if (entry.model && !state.model) state.model = entry.model;
  if (entry.stopReason) state.stopReason = entry.stopReason;
  if (entry.errorMessage) state.errorMessage = entry.errorMessage;
  return true;
}

/** The child's answer: the last text of its last assistant message. */
export function finalOutput(state: Pick<StreamState, "messages">): string {
  for (let i = state.messages.length - 1; i >= 0; i--) {
    const message = state.messages[i];
    if (message.role !== "assistant") continue;
    for (const part of message.content) {
      if (part.type === "text" && typeof part.text === "string") return part.text;
    }
  }
  return "";
}

/** Last tool call the child made, for the parent's progress line. */
export function lastToolCall(state: Pick<StreamState, "messages">): { name: string; args: Record<string, unknown> } | null {
  for (let i = state.messages.length - 1; i >= 0; i--) {
    const message = state.messages[i];
    if (message.role !== "assistant") continue;
    for (let j = message.content.length - 1; j >= 0; j--) {
      const part = message.content[j];
      if (part.type === "toolCall" && typeof part.name === "string") {
        return { name: part.name, args: part.arguments ?? {} };
      }
    }
  }
  return null;
}

export function formatTokens(count: number): string {
  if (count < 1000) return count.toString();
  if (count < 10000) return `${(count / 1000).toFixed(1)}k`;
  if (count < 1000000) return `${Math.round(count / 1000)}k`;
  return `${(count / 1000000).toFixed(1)}M`;
}

export function formatUsage(usage: UsageStats, model?: string): string {
  const parts: string[] = [];
  if (usage.turns) parts.push(`${usage.turns} turn${usage.turns > 1 ? "s" : ""}`);
  if (usage.input) parts.push(`↑${formatTokens(usage.input)}`);
  if (usage.output) parts.push(`↓${formatTokens(usage.output)}`);
  if (usage.cacheRead) parts.push(`R${formatTokens(usage.cacheRead)}`);
  if (usage.cacheWrite) parts.push(`W${formatTokens(usage.cacheWrite)}`);
  if (usage.cost) parts.push(`$${usage.cost.toFixed(3)}`);
  if (usage.contextTokens) parts.push(`ctx:${formatTokens(usage.contextTokens)}`);
  if (model) parts.push(model);
  return parts.join(" ");
}

export function addUsage(total: UsageStats, next: UsageStats): UsageStats {
  return {
    input: total.input + next.input,
    output: total.output + next.output,
    cacheRead: total.cacheRead + next.cacheRead,
    cacheWrite: total.cacheWrite + next.cacheWrite,
    cost: total.cost + next.cost,
    contextTokens: Math.max(total.contextTokens, next.contextTokens),
    turns: total.turns + next.turns,
  };
}

function asParts(value: unknown): StreamPart[] {
  if (!Array.isArray(value)) return [];
  const parts: StreamPart[] = [];
  for (const item of value) {
    const record = asRecord(item);
    if (typeof record.type !== "string") continue;
    const part: StreamPart = { type: record.type };
    if (typeof record.text === "string") part.text = record.text;
    if (typeof record.name === "string") part.name = record.name;
    if (record.arguments !== undefined) part.arguments = asRecord(record.arguments);
    parts.push(part);
  }
  return parts;
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value) ? (value as Record<string, unknown>) : {};
}

function num(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}
