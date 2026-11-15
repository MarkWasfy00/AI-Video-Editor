"""Let Claude edit a video through the editor's MCP server (Claude Agent SDK, Python).

Two ways to connect:
  * headless (default): the SDK launches VideoEditorMCP over stdio - no window needed.
  * --gui: connect to a running VideoEditor window (Tools > MCP Server) over HTTP and
    watch Claude edit live.

    pip install claude-agent-sdk
    python edit_with_claude.py --mcp-exe ../../dist/VideoEditor/VideoEditorMCP.exe \
        "Import C:/clips/a.mp4 and C:/clips/b.mp4, cut the first 2 seconds of a.mp4,
         put b.mp4 after it with a 1 second fade, make b.mp4 black and white,
         check the result with render_frame, then export to C:/clips/out.mp4"
"""
import argparse
import asyncio

from claude_agent_sdk import (
    AssistantMessage,
    ClaudeAgentOptions,
    ResultMessage,
    SystemMessage,
    query,
)

SYSTEM_PROMPT = (
    "You are a video editor operating a non-linear editor through the video-editor MCP tools. "
    "Start with project_get to see the timeline. Times are in seconds; track 0 is the top layer. "
    "After making edits, use render_frame at a few key times to look at the result before exporting. "
    "Every edit is undoable with edit_undo. Save the project with project_save when you are done."
)


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("task", help="What Claude should do with the video")
    ap.add_argument("--mcp-exe", default="VideoEditorMCP.exe", help="Path to the headless MCP server")
    ap.add_argument("--gui", action="store_true", help="Drive a running VideoEditor window instead")
    ap.add_argument("--url", default="http://127.0.0.1:8765/mcp", help="GUI MCP endpoint")
    args = ap.parse_args()

    server = (
        {"type": "http", "url": args.url}
        if args.gui
        else {"command": args.mcp_exe, "args": []}
    )
    options = ClaudeAgentOptions(
        model="claude-opus-5-5",
        system_prompt=SYSTEM_PROMPT,
        mcp_servers={"video-editor": server},
        # Auto-approve every editor tool (and nothing else).
        allowed_tools=["mcp__video-editor__*"],
    )

    async for message in query(prompt=args.task, options=options):
        if isinstance(message, SystemMessage) and message.subtype == "init":
            for s in message.data.get("mcp_servers", []):
                if s.get("status") in ("failed", "needs-auth"):
                    print(f"MCP server {s.get('name')} is unavailable: {s.get('status')}")
        elif isinstance(message, AssistantMessage):
            for block in message.content:
                name = getattr(block, "name", "")
                if name.startswith("mcp__video-editor__"):
                    print("->", name.removeprefix("mcp__video-editor__"))
        elif isinstance(message, ResultMessage):
            print(message.result if message.subtype == "success" else f"Run ended: {message.subtype}")


if __name__ == "__main__":
    asyncio.run(main())
