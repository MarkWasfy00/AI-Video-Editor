// Checks that claude-bridge/bridge.mjs loads the Claude Agent SDK and answers
// the editor's protocol. Sends no message to Claude (no login or cost needed).
//
// usage: node claude_bridge_smoke.mjs <path to bridge.mjs>
import { spawn } from "node:child_process";
import readline from "node:readline";

const bridge = process.argv[2];
const p = spawn(process.execPath, [bridge], {
  env: { ...process.env, VE_MCP_URL: "http://127.0.0.1:1/mcp" },
  stdio: ["pipe", "pipe", "inherit"],
});
const seen = [];
const fail = (msg) => { console.error("FAIL", msg, "seen:", seen.join(",")); p.kill(); process.exit(1); };
const timer = setTimeout(() => fail("timeout"), 20000);

readline.createInterface({ input: p.stdout }).on("line", (line) => {
  let m;
  try { m = JSON.parse(line); } catch { return fail(`not JSON: ${line}`); }
  seen.push(m.type);
  if (m.type === "hello") {
    if (!m.model || !m.mcpUrl) fail("hello is missing fields");
    console.log(`ok   hello (node ${m.node}, bundled CLI: ${m.bundledCli})`);
    p.stdin.write(JSON.stringify({ type: "model", model: "claude-haiku-5-5" }) + "\n");
    p.stdin.write(JSON.stringify({ type: "bogus" }) + "\n");
    p.stdin.write(JSON.stringify({ type: "reset" }) + "\n");
  } else if (m.type === "reset") {
    if (!seen.includes("status") || !seen.includes("error")) fail("model/unknown-command replies missing");
    console.log("ok   model switch, unknown command error, reset");
    clearTimeout(timer);
    p.stdin.end();
  }
});
p.on("exit", (code) => {
  if (!seen.includes("reset")) fail(`bridge exited early (${code})`);
  console.log("ok   bridge exits when stdin closes");
  process.exit(0);
});
