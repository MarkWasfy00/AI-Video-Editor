// Let Claude edit a video through the editor's MCP server (Claude Agent SDK, TypeScript).
//
//   npm install @anthropic-ai/claude-agent-sdk
//   npx tsx edit_with_claude.ts "Trim clip_a to 3 seconds, add a vignette, export to C:/clips/out.mp4"
//
// By default this drives a running VideoEditor window over HTTP (Tools > MCP Server),
// so you can watch the edits happen. Set VIDEO_EDITOR_MCP_EXE to the path of
// VideoEditorMCP.exe to run headless over stdio instead.
import { query } from "@anthropic-ai/claude-agent-sdk";

const task = process.argv.slice(2).join(" ") || "Describe the current timeline using project_get.";
const exe = process.env.VIDEO_EDITOR_MCP_EXE;

const videoEditor = exe
  ? { command: exe, args: [] as string[] }
  : { type: "http" as const, url: process.env.VIDEO_EDITOR_MCP_URL ?? "http://127.0.0.1:8765/mcp" };

for await (const message of query({
  prompt: task,
  options: {
    model: "claude-opus-5-5",
    systemPrompt:
      "You are a video editor operating a non-linear editor through the video-editor MCP tools. " +
      "Start with project_get. Times are in seconds; track 0 is the top layer. " +
      "Use render_frame to look at your edits before exporting. Save with project_save when done.",
    mcpServers: { "video-editor": videoEditor },
    allowedTools: ["mcp__video-editor__*"],
  },
})) {
  if (message.type === "system" && message.subtype === "init") {
    for (const s of message.mcp_servers) {
      if (s.status === "failed" || s.status === "needs-auth") console.warn(`MCP server ${s.name}: ${s.status}`);
    }
  }
  if (message.type === "assistant") {
    for (const block of message.message.content) {
      if (block.type === "tool_use" && block.name.startsWith("mcp__video-editor__")) {
        console.log("->", block.name.replace("mcp__video-editor__", ""));
      }
    }
  }
  if (message.type === "result") {
    console.log(message.subtype === "success" ? message.result : `Run ended: ${message.subtype}`);
  }
}
