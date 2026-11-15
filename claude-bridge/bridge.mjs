// Bridge between the Video Editor's Claude panel and the Claude Agent SDK.
//
// The editor starts this script with Node and talks to it with one JSON
// object per line:
//
//   editor -> bridge (stdin)
//     {"type":"user","text":"..."}        send a message (starts the session)
//     {"type":"interrupt"}                stop the current turn
//     {"type":"reset"}                    forget the conversation
//     {"type":"model","model":"..."}      switch model
//
//   bridge -> editor (stdout)
//     ready / text_delta / assistant_text / tool_use / tool_result / result /
//     status / error / reset
//
// Claude reaches the editor through its own MCP endpoint (VE_MCP_URL) and is
// limited to the editor's tools: every built-in Claude Code tool is disabled.
import { query } from "@anthropic-ai/claude-agent-sdk";
import { execFileSync } from "node:child_process";
import { existsSync, mkdirSync } from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";
import readline from "node:readline";

const MCP_URL = process.env.VE_MCP_URL || "http://127.0.0.1:8765/mcp";
const CWD = process.env.VE_CLAUDE_CWD || path.join(os.homedir(), ".video-editor-claude");
let model = process.env.VE_CLAUDE_MODEL || "claude-opus-5-5";

const SYSTEM_PROMPT = `You are Claude, the assistant built into a desktop non-linear video editor.
You edit the user's open project live through the video-editor MCP tools while they watch the window.

How the editor works:
- Times are in seconds. Video tracks come first (index 0 is the top layer of the picture) and are silent;
  audio tracks follow. Adding a video with sound creates a video clip plus a linked audio clip on an
  audio track. Linked clips move, trim, split and delete together (pass linked=false to edit one side;
  clip_unlink separates the audio from the footage, clip_link {clipId} re-links it; clip_add link=false
  adds a video with its sound already unlinked). Picture settings go on the video clip, volume on the audio clip.
- Each user message may start with an [Editor state: ...] line describing the playhead and selected clip; "this clip" or "here" refers to those.
- Call project_get (or clip_list / mixer_get) when you need the current state; don't guess ids.
- Every edit is undoable (edit_undo). Prefer small, precise edits.
- Editing tools: clip_trim, clip_split, timeline_cut_range (remove a time range), clip_set_speed (speed and
  speed curves), clip_reverse, clip_freeze_frame, clip_duplicate, clip_ripple_delete, timeline_close_gap;
  crop / fit / rotate with clip_update; resize or change aspect ratio with project_set_settings.
- transcript_create turns speech or song lyrics into timed text (Whisper); use language "ar-EG" for Egyptian
  Arabic and "ar" for Modern Standard Arabic; hints = names/slang to expect. Arabic captions are right to left
  automatically; set arabicFont (see captions_get suggestedArabicFonts) or arabicFontFile for the Arabic face; transcript_get reads it,
  transcript_update fixes words / timing, transcript_export writes srt, vtt, lrc or txt. Only pass install=true if the
  user agreed to the one-time download.
- captions_set shows the transcript as CapCut-style text over the video (presets classic, bold, box, neon, minimal,
  karaoke; any style field can be overridden; font = an installed font from captions_get suggestedFonts, or
  fontFile = a .ttf/.otf path). Check the look with render_frame.
- audio_separate splits a clip's sound into voice and music (keep both, only voice, or only music). Only pass
  install=true if the user agreed to the one-time download.
- After visual changes, check your work with render_frame. For audio levels use mixer_analyze, since you cannot listen.
- Never pass discardChanges=true to project_new / project_open unless the user explicitly asked to throw away unsaved work.
- Before export_video to an existing path or a long render, say what you are about to do.

Reply briefly in plain language: what you changed and anything the user should check.`;

const send = (o) => process.stdout.write(JSON.stringify(o) + "\n");
const strip = (name) => (name || "").replace(/^mcp__video-editor__/, "");

// Use the SDK's bundled Claude Code if it was installed, otherwise the
// user's own `claude` CLI (the packaged editor ships without the bundle).
function claudeExecutable() {
  if (process.env.VE_CLAUDE_EXE) return process.env.VE_CLAUDE_EXE;
  try {
    const require = createRequire(import.meta.url);
    require.resolve(`@anthropic-ai/claude-agent-sdk-${process.platform}-${process.arch}/package.json`);
    return undefined; // bundled binary present
  } catch {}
  try {
    const cmd = process.platform === "win32" ? "where" : "which";
    const found = execFileSync(cmd, ["claude"], { encoding: "utf8" }).split(/\r?\n/).find(Boolean);
    if (found) return found.trim();
  } catch {}
  return undefined;
}

// Async queue of user messages: keeps one conversation open across turns.
class Inbox {
  constructor() { this.items = []; this.waiters = []; this.closed = false; }
  push(item) {
    const w = this.waiters.shift();
    if (w) w({ value: item, done: false });
    else this.items.push(item);
  }
  close() {
    this.closed = true;
    for (const w of this.waiters.splice(0)) w({ value: undefined, done: true });
  }
  [Symbol.asyncIterator]() {
    return {
      next: () => {
        if (this.items.length) return Promise.resolve({ value: this.items.shift(), done: false });
        if (this.closed) return Promise.resolve({ value: undefined, done: true });
        return new Promise((resolve) => this.waiters.push(resolve));
      },
    };
  }
}

let session = null; // { q, inbox }

function startSession() {
  mkdirSync(CWD, { recursive: true });
  const inbox = new Inbox();
  const exe = claudeExecutable();
  const q = query({
    prompt: inbox,
    options: {
      model,
      cwd: CWD,
      systemPrompt: SYSTEM_PROMPT,
      tools: [], // no built-in Claude Code tools (no shell, no file access)
      mcpServers: { "video-editor": { type: "http", url: MCP_URL, timeout: 30 * 60 * 1000 } },
      allowedTools: ["mcp__video-editor__*"],
      permissionMode: "dontAsk", // anything not allowed above is denied
      settingSources: [], // ignore the user's Claude Code settings, hooks and CLAUDE.md
      includePartialMessages: true,
      maxTurns: 80,
      ...(exe ? { pathToClaudeCodeExecutable: exe } : {}),
      env: { ...process.env, CLAUDE_AGENT_SDK_CLIENT_APP: "video-editor/0.3.0" },
      stderr: (data) => {
        const line = String(data).trim();
        if (line) send({ type: "log", text: line.slice(0, 500) });
      },
    },
  });
  session = { q, inbox };
  pump(session);
}

function textOf(content) {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content.filter((b) => b.type === "text").map((b) => b.text).join("\n");
}

async function pump(s) {
  try {
    for await (const m of s.q) {
      if (session !== s) break; // reset while streaming
      handle(m);
    }
  } catch (e) {
    if (session === s) send({ type: "error", message: String(e?.message || e) });
  }
  if (session === s) {
    session = null;
    send({ type: "status", text: "Session ended" });
  }
}

function handle(m) {
  switch (m.type) {
    case "system":
      if (m.subtype === "init") {
        const ed = (m.mcp_servers || []).find((x) => x.name === "video-editor");
        send({ type: "ready", model: m.model, version: m.claude_code_version,
               apiKeySource: m.apiKeySource, editor: ed ? ed.status : "missing" });
      }
      break;
    case "stream_event": {
      if (m.parent_tool_use_id) break;
      const ev = m.event;
      if (ev?.type === "message_start") send({ type: "message_start" });
      else if (ev?.type === "content_block_delta" && ev.delta?.type === "text_delta")
        send({ type: "text_delta", text: ev.delta.text });
      break;
    }
    case "assistant": {
      if (m.parent_tool_use_id) break;
      for (const b of m.message?.content || []) {
        if (b.type === "text") send({ type: "assistant_text", text: b.text });
        else if (b.type === "tool_use") send({ type: "tool_use", id: b.id, name: strip(b.name), input: b.input });
      }
      if (m.error) send({ type: "error", message: `Claude API error: ${m.error}` });
      break;
    }
    case "user": {
      const content = m.message?.content;
      if (!Array.isArray(content)) break;
      for (const b of content) {
        if (b.type !== "tool_result") continue;
        const blocks = Array.isArray(b.content) ? b.content : [];
        const images = blocks
          .filter((x) => x.type === "image" && x.source?.type === "base64")
          .map((x) => ({ mediaType: x.source.media_type, data: x.source.data }));
        send({ type: "tool_result", id: b.tool_use_id, isError: !!b.is_error,
               text: textOf(b.content).slice(0, 2000), images });
      }
      break;
    }
    case "result":
      send({ type: "result", subtype: m.subtype, isError: !!m.is_error, text: m.result ?? "",
             costUsd: m.total_cost_usd ?? 0, turns: m.num_turns ?? 0, durationMs: m.duration_ms ?? 0 });
      break;
    default:
      break;
  }
}

async function onCommand(cmd) {
  switch (cmd.type) {
    case "user":
      if (!session) startSession();
      session.inbox.push({ type: "user", message: { role: "user", content: String(cmd.text || "") },
                           parent_tool_use_id: null });
      break;
    case "interrupt":
      if (session) await session.q.interrupt().catch((e) => send({ type: "error", message: String(e?.message || e) }));
      break;
    case "reset":
      if (session) {
        const s = session;
        session = null;
        s.inbox.close();
        try { s.q.close(); } catch {}
      }
      send({ type: "reset" });
      break;
    case "model":
      model = String(cmd.model || model);
      if (session) await session.q.setModel(model).catch((e) => send({ type: "error", message: String(e?.message || e) }));
      send({ type: "status", text: `Model: ${model}` });
      break;
    default:
      send({ type: "error", message: `Unknown command ${cmd.type}` });
  }
}

const rl = readline.createInterface({ input: process.stdin });
rl.on("line", (line) => {
  if (!line.trim()) return;
  let cmd;
  try { cmd = JSON.parse(line); } catch { return send({ type: "error", message: "Bad JSON from editor" }); }
  onCommand(cmd).catch((e) => send({ type: "error", message: String(e?.message || e) }));
});
rl.on("close", () => {
  if (session) { session.inbox.close(); try { session.q.close(); } catch {} }
  process.exit(0);
});

send({ type: "hello", mcpUrl: MCP_URL, model, bundledCli: claudeExecutable() === undefined,
       node: process.version, cwdExists: existsSync(CWD) });
