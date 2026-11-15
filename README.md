# Video Editor

A basic non-linear video editor built with **Qt 6 + C++20**, **FFmpeg**, **Vulkan + OpenGL**
GPU compositing, **CMake + Ninja**, and **SQLite + JSON** project files.

## Features

- **Media bin**: import video, audio, and still images (drag & drop or *File > Import*),
  with thumbnails and stream info.
- **Multi-track timeline** with a **video section** (V tracks, thumbnails) above an **audio
  section** (A tracks, waveforms):
  - Drag clips in from the bin, move them between tracks of the same kind, trim either edge,
    split at the playhead (`S`), and delete (`Del`).
  - Clips snap to the playhead and to clip edges. Placing a clip over another one overwrites
    it, as in most editors.
  - Video tracks can be hidden (H); audio tracks can be muted (M) or soloed (S). `Ctrl` + wheel
    zooms the timeline.
- **Video and its sound as separate, linked clips**: when you add a video that has sound, the
  picture goes on a video track and the sound becomes its own clip on an audio track, with a
  waveform.
  - The two are linked (marked ⚭): moving, trimming, splitting or deleting one does the same
    to the other.
  - **Unlink the audio from the footage**: select the clip and press `Ctrl+L` (*Edit > Unlink
    Audio and Video*), click **Unlink** in the Inspector, or right-click the clip. Press
    `Ctrl+L` again, or click **Link**, to rejoin them.
  - Turn off **Linked Selection** (the toggle in the transport bar, or `Ctrl+Shift+L`) to edit
    every clip on its own without unlinking anything. Holding `Alt` while dragging does the same
    for a single drag.
  - Turn off *Edit > Link Audio to Video When Adding Clips* to add videos with their sound
    already unlinked.
  - Right-click a clip to delete only that clip and keep its linked partner.
  - Picture settings belong to the video clip; volume belongs to the audio clip.
  - Projects saved by earlier versions are converted automatically when opened.
- **Editing tools** (*Edit* and *Clip* menus, the clip right-click menu, the Inspector, and MCP):

  | Feature | How |
  | --- | --- |
  | Import media | *File > Import* (`Ctrl+I`), drag & drop: video, images, audio |
  | Timeline editing | Multitrack video section + audio section, linked audio/video |
  | Trim | Drag a clip edge (removes the beginning or ending; follows the clip speed) |
  | Split | `S` at the playhead |
  | Cut | Mark a range with `I` / `O`, then `Ctrl+X` (*Cut Marked Range*) removes it from every track and closes the gap; `Ctrl+Shift+X` leaves the gap |
  | Crop | Inspector > *Crop* (left / right / top / bottom) |
  | Resize | *Clip > Resize Canvas / Aspect Ratio* (16:9, 9:16, 1:1, 4:5, 4:3, 21:9 presets), Inspector *Fit* / *Fill*, Scale |
  | Rotate | Inspector *Rotation*, `Ctrl+]` / `Ctrl+[` rotate 90° |
  | Reverse | `Ctrl+R` (picture and its sound play backwards) |
  | Freeze frame | `Shift+F` holds the frame at the playhead for 2 s (trim the still to change it) |
  | Duplicate | `Ctrl+D` (with the linked audio, placed right after) |
  | Speed control | *Clip > Speed* (0.25x - 8x, custom 0.05x - 16x), Inspector *Speed* |
  | Speed curves | *Clip > Speed Curve*: ease in, ease out, slow-fast-slow, bullet, hero (or custom points via MCP) |
  | Ripple editing | `Shift+Del` ripple delete, `Ctrl+G` / right-click *Close Gap* |
  | Undo / Redo | `Ctrl+Z` / `Ctrl+Y` for every edit |
  | Transcript / lyrics | *Clip > Transcribe Speech / Lyrics...* or right-click: Whisper turns speech or singing into timed text in the **Transcript** panel (`Ctrl+Shift+T`) |
  | Captions on the video | *Clip > Auto Captions* (or right-click): CapCut-style text from the transcript, with the spoken word highlighted; style it in the **Captions** panel (`Ctrl+Shift+K`) |
  | Separate voice and music | *Clip > Separate Voice and Music...* or right-click: AI separation (Demucs) puts the voice and the music on separate audio tracks, or keeps only one of them |

  When a clip's speed changes, it keeps the same footage: its length changes and later clips
  on its tracks move so nothing is overwritten. The audio pitch follows the speed, as on tape
  (there is no pitch correction).
- **Voice / music separation** with the open-source **Demucs** model (Meta, `htdemucs`). It runs
  on this computer, on the GPU when PyTorch has CUDA (about 4 s for a 15 s clip on an RTX 2060
  SUPER).
  - Choose *Keep both* (voice and music become separate, linked clips you can mix, mute or
    delete), *Keep only the voice* (removes the music) or *Keep only the music* (removes the
    voice).
  - The first use runs a one-time setup with [`uv`](https://docs.astral.sh/uv/): a private
    Python 3.12 environment with PyTorch (the CUDA build when an NVIDIA GPU is present) and
    `demucs`, plus the model (~80 MB). It lives in `%LOCALAPPDATA%\VideoEditor\separator`.
  - Separated files are cached in `%LOCALAPPDATA%\VideoEditor\separated`, so separating the
    same file again is instant.
- **Transcripts and lyrics** with OpenAI's open-source **Whisper** (`turbo` model, on the GPU):
  - Select a clip with speech or singing, then *Transcribe Selected Clip* in the **Transcript**
    panel (or right-click the clip). For songs, *Voice only* separates the voice from the music
    first, which gives much better lyrics.
  - The panel shows the line being heard karaoke-style: sung words white, the current word gold.
    Lines are listed with their times; click a time to jump there, and double-click a line to
    edit its text. Right-click a line to delete it or add a new line at the playhead.
  - The lyrics also appear along the audio clip in the timeline. They follow the clip when you
    move, trim, speed up or reverse it, and they are saved with the project.
  - *Export* writes subtitles (`.srt`, `.vtt`), synced lyrics (`.lrc`) or plain text.
  - The first use downloads Whisper and its model (~1.6 GB) into the same private environment
    as the voice/music separator.
- **Arabic, including Egyptian Arabic:**
  - Choose *Arabic (Egyptian)* in the Transcript panel's language list. Whisper has a single Arabic
    model, so the editor gives it Egyptian dialect hints (دلوقتي، عشان، إزيك ...). On Egyptian
    speech the Arabic transcript became much more accurate: word errors dropped from 58% to 21%
    on a test sentence. *Arabic (Standard)* is for Modern Standard Arabic.
  - The *Hints* box adds names, slang or song words Whisper should expect.
  - Arabic captions run right to left with joined letters, and break at ، ؟ ؛. They use an Arabic
    font: automatic (Segoe UI when the main font has no Arabic letters), any installed Arabic
    font (Sakkal Majalla, Traditional Arabic, Simplified Arabic, Arabic Typesetting, Tahoma ...),
    or a font file. Popular caption fonts such as Cairo, Tajawal, Almarai and Lalezar can be added
    with *Add Arabic Font File...*. The *Arabic* preset is tuned for Arabic captions.
  - The Transcript panel, the karaoke view and the timeline show Arabic right to left.
    SRT / VTT / LRC exports are UTF-8.
- **Captions on the video (CapCut-style)**, generated from the transcript:
  - *Auto Captions* (Clip menu or right-click) transcribes the clip if needed and turns captions
    on. You can also tick *Captions on video* in the Transcript panel.
  - Presets: **Classic** (white, black outline, yellow word), **Bold** (big caps in the middle),
    **Box**, **Neon**, **Minimal** (whole line) and **Karaoke** (coloured box behind the word).
  - **Fonts:** pick any installed font from the font list, where each font is shown in its own
    style. *Add Font File...* uses a downloaded `.ttf` / `.otf` without installing it; the file is
    remembered in the project and used in the preview and in export.
  - The **Captions** panel also sets bold / italic / ALL CAPS, size, colours (text, spoken
    word, outline, box), outline thickness, shadow, background box, position (bottom / middle /
    top and distance from the edge), how many words show at a time, max lines, and the animation
    (pop in, fade, none).
  - The spoken word is highlighted as it is said, using Whisper's word timings.
  - Edit the words in the Transcript panel and the captions follow. Captions show in the preview
    and are burned into exported video (and `render_frame`). Use *Export > .srt* for separate
    subtitle files.
- **GPU preview** with two backends you can switch at runtime (*View > Preview Renderer*):
  - **OpenGL 3.3 core** (`QOpenGLWidget`)
  - **Vulkan** (`QVulkanWindow`, with SPIR-V compiled at build time by `glslc`)
- **Per-clip effects** (fragment shaders, identical on both backends): brightness,
  contrast, saturation, hue, blur, vignette, grayscale, invert, opacity, fade in/out.
- **Per-clip transform**: position, scale, rotation.
- **Audio**: timeline mixing with per-clip volume and fades, played through Qt Multimedia.
- **Audio mixer** (*View > Audio Mixer*, `Ctrl+Shift+M`): one channel strip per track with a
  fader (-inf to +12 dB), pan knob, mute and solo, plus a master bus with fader, mute and a
  clip indicator. Stereo peak meters with peak hold follow the audio as it plays. Double-click
  a fader or pan knob to reset it. Mixer settings apply to playback and export, are saved in
  the project, and every change can be undone.
- **Playback** decodes on a worker thread. Frame requests are coalesced, so scrubbing
  never builds up a backlog.
- **Export**: the timeline is rendered offscreen with the same OpenGL compositor, then
  encoded with FFmpeg. Video codecs: H.264, H.265, VP9, MPEG-4, ProRes. Audio: AAC/Opus.
  Resolution and frame rate are selectable.
- **Projects**:
  - `.veproj` is an SQLite database (`meta`, `media`, `tracks`, `clips` tables; clip
    properties are stored as JSON columns).
  - `.json` is a plain JSON interchange format.
- **Panel layout**: the timeline fills the bottom half of the window, across its full width. The top
  half holds the Media panel (left), the preview (centre), and the Inspector and Audio Mixer
  side by side (right).
  *View > Inspector & Mixer Layout* switches between side by side, stacked, and tabbed, floats
  the mixer, or resets all panels. Any panel can also be dragged by its title bar or tab to
  another spot. The layout is remembered between sessions.
- Undo/redo for every edit. Consecutive slider changes merge into one undo step.

## Graphics card (GPU) support

The GPU is used at every stage of the pipeline:

| Stage | GPU path | Fallback |
| --- | --- | --- |
| Decoding | FFmpeg hardware decoding: D3D11VA, NVDEC/CUDA, DXVA2, D3D12VA, Vulkan Video (Windows); VA-API/CUDA/Vulkan (Linux); VideoToolbox (macOS). Frames are downloaded for compositing. | CPU decoding, per stream (e.g. for profiles the GPU can't decode) |
| Compositing + effects | OpenGL 3.3 or Vulkan shaders | none (a GPU is required) |
| Preview | OpenGL widget or Vulkan window; you choose the Vulkan device (auto prefers a discrete GPU) | OpenGL |
| Export rendering | Offscreen OpenGL framebuffer | none |
| Encoding | NVIDIA NVENC (H.264/HEVC/AV1), AMD AMF, Intel Quick Sync, Apple VideoToolbox | x264/x265/VP9/MPEG-4/ProRes on the CPU |

- **Edit > GPU & Hardware Acceleration...** sets the decode mode (Automatic / Off / a specific API) and
  the Vulkan GPU, and lists the detected hardware.
- The status bar shows whether the visible layers were decoded on the GPU, e.g. `Decode: GPU (d3d11va)`.
- The Export dialog lists only the encoders that actually open on this machine. Each hardware
  encoder is test-opened once, so for example AV1 NVENC is hidden on RTX 20-series cards.
  GPU H.264 is preselected when available.
- On hybrid-graphics laptops, the executables export `NvOptimusEnablement` and
  `AmdPowerXpressRequestHighPerformance` so that Windows runs them on the discrete GPU.

Results on an RTX 2060 SUPER: all five Windows decoders produce the same image as the CPU decoder
(mean difference < 1/255), and NVENC exports run about 3x faster than real time at 1080p.

## Controlling the editor with Claude (MCP)

Every editor operation is exposed as a [Model Context Protocol](https://modelcontextprotocol.io)
tool, so Claude (Agent SDK, Claude Code, Claude Desktop) can drive the editor: import media,
build and trim the timeline, apply effects, look at rendered frames, and export.

All features go through one API layer (`src/api/EditorApi`). The GUI, the MCP tools, and the
tests use that same code. Two transports are available:

| | Endpoint | Use it for |
| --- | --- | --- |
| **GUI app** (live) | `http://127.0.0.1:8765/mcp` (Streamable HTTP, localhost only) | Watching Claude edit in the window. Toggle under *Tools > Enable MCP Server*; `--mcp-port N` / `--no-mcp` on the command line. |
| **Headless** | `VideoEditorMCP.exe` (stdio) or `VideoEditorMCP --http 8765` | Automation and CI. No window; the SDK launches it for you. |

### Tools (56)

| Area | Tools |
| --- | --- |
| Project | `project_get`, `project_new`, `project_open`, `project_save`, `project_set_settings` |
| Media | `media_import`, `media_list`, `media_remove`, `media_probe` |
| Tracks | `track_list`, `track_add`, `track_remove`, `track_update` |
| Audio mixer | `mixer_get` (faders, pan, mute/solo, master, live meters while playing), `mixer_set_track`, `mixer_set_master`, `mixer_analyze` (offline peak/RMS per track and master, clipping count, gain advice) |
| Audio AI | `audio_separate` (voice / music separation; `keep`: both, voice, music), `transcript_create` (Whisper speech / lyrics with word timings; `language: "ar-EG"` for Egyptian Arabic, `hints`), `transcript_get`, `transcript_update` (fix text or timing, insert, delete), `transcript_export` (srt, vtt, lrc, txt), `captions_get`, `captions_set` (CapCut-style captions: preset, font or `fontFile`, `arabicFont` / `arabicFontFile`, colours, position, words per caption, animation) |
| Edit tools | `clip_set_speed` (speed + curves), `clip_reverse`, `clip_freeze_frame`, `clip_duplicate`, `clip_ripple_delete`, `timeline_close_gap`, `timeline_cut_range`; crop and fit/fill go through `clip_update`, aspect ratio through `project_set_settings` |
| Clips | `clip_list`, `clip_get`, `clip_add` (a video with sound returns its linked audio clip too; `link: false` adds the sound unlinked), `clip_move`, `clip_trim`, `clip_split`, `clip_remove` (these act on linked clips unless `linked: false`), `clip_link` (two ids, or one id to re-link a video with its own sound), `clip_unlink`, `clip_update` (transform, effects, volume, fades) |
| History | `edit_undo`, `edit_redo`, `edit_history` |
| Playback | `playback_state`, `playback_seek`, `playback_play`, `playback_pause`, `selection_get`, `selection_set` |
| Render / export | `render_frame` (returns the GPU-rendered frame as an image Claude can see), `export_video`, `export_encoders` |
| GPU | `gpu_info`, `gpu_set_decode_mode`, `preview_set_renderer` |

Times are in seconds. Edits are recorded on the undo stack, and every tool carries MCP
annotations (`readOnlyHint`, `destructiveHint`, ...). `project_new` and `project_open` refuse to
discard unsaved work unless the call passes `discardChanges: true`.

### Claude panel (chat inside the editor)

The **Claude** panel sits in the top-right corner, tabbed with the Inspector (*View > Claude
Assistant*, `Ctrl+Shift+C`). Type what you want, for example "split the selected clip at the
playhead and fade it out", and Claude does it in the open project while you watch:

- Each message includes the playhead position, the selected clip and the track names, so
  "this clip" and "here" work.
- Every tool call appears in the chat with a check mark or an error. Frames Claude renders to
  check its work are shown as thumbnails.
- Everything Claude changes can be undone. **Stop** (or `Esc`) interrupts it, and **New Chat**
  starts a fresh conversation without changing the project.
- You can choose Opus 5.5 (the default), Sonnet 5.5 or Haiku 5.5.

How it works: the panel starts `claude-bridge/bridge.mjs` with Node. The bridge uses the
**Claude Agent SDK** and connects Claude to this window's own MCP endpoint. Claude can use **only
the editor's tools**: shell and file tools are turned off, and your Claude Code settings, hooks
and `CLAUDE.md` are not loaded.

Requirements:
- Node.js 18 or newer.
- Claude Code installed and signed in (`claude` in a terminal once), or `ANTHROPIC_API_KEY` set.

The packaged `dist` already contains the bridge. From a source checkout, run this once:

```bash
cd claude-bridge && npm install
```

### Claude Agent SDK

**Python** (`pip install claude-agent-sdk`):

```python
from claude_agent_sdk import query, ClaudeAgentOptions

options = ClaudeAgentOptions(
    model="claude-opus-5-5",
    mcp_servers={
        # headless: the SDK launches the server over stdio
        "video-editor": {"command": "C:/path/to/VideoEditorMCP.exe", "args": []},
        # or the live GUI window:
        # "video-editor": {"type": "http", "url": "http://127.0.0.1:8765/mcp"},
    },
    allowed_tools=["mcp__video-editor__*"],
)
async for message in query(prompt="Import C:/clips/a.mp4, make it black and white, export to C:/clips/out.mp4",
                           options=options):
    ...
```

**TypeScript** (`npm install @anthropic-ai/claude-agent-sdk`):

```ts
import { query } from "@anthropic-ai/claude-agent-sdk";

for await (const message of query({
  prompt: "Split the first clip at 2 seconds and add a 1 second fade-out",
  options: {
    model: "claude-opus-5-5",
    mcpServers: { "video-editor": { type: "http", url: "http://127.0.0.1:8765/mcp" } },
    allowedTools: ["mcp__video-editor__*"],
  },
})) { /* ... */ }
```

Complete examples are in [examples/agent_sdk/](examples/agent_sdk/). An `.mcp.json` snippet is in
[examples/mcp.json](examples/mcp.json).

**Claude Code:**

```bash
claude mcp add --transport http video-editor http://127.0.0.1:8765/mcp
```

**Security:** the HTTP endpoint listens on 127.0.0.1 only. Requests carrying a non-local
`Origin` header are rejected with 403, which protects against DNS rebinding from web pages.
Disable the endpoint under *Tools > Enable MCP Server* when you don't need it.

## Architecture

```
src/
  core/    Project model (tracks, clips, effects), undo, SQLite/JSON persistence
  media/   StemSeparator (Demucs voice/music separation via separator/separate.py),
           FFmpeg probing, video/audio decoders (GPU hwaccel), AudioMixer (gain/pan/
           solo/master, metering, offline analysis), Waveform (timeline peaks), FrameSource,
           HwAccel (decode devices + NVENC/AMF/QSV encoder probing),
           (timeline -> layer stack), Exporter (offscreen GL + FFmpeg encoder)
  render/  CaptionRenderer (CapCut-style caption layer), CompositeFrame + shared shader params, GLCompositor, OpenGL & Vulkan
           preview views, PreviewEngine (decode worker thread)
  audio/   AudioPlayer (Qt Multimedia QAudioSink, pull mode)
  api/     EditorApi (every operation, seconds-based JSON), McpServer (56 MCP
           tools), MCP transports (HTTP + stdio)
  app/     MainWindow, MediaBin, TimelineWidget, Inspector, MixerPanel, ClaudePanel, ExportDialog,
           GpuSettingsDialog, PlaybackController
  mcp_main.cpp  VideoEditorMCP - headless MCP server
shaders/gl  GLSL 330 (loaded from Qt resources)
shaders/vk  GLSL 450 -> SPIR-V via glslc, embedded as uint32 arrays
claude-bridge/  bridge.mjs - Node bridge from the Claude panel to the Claude Agent SDK
tests/      smoke_test (C++: decode incl. every GPU decoder, editing, persistence, GPU
            compositing, CPU + GPU export) and mcp_e2e.py (drives all 56 MCP tools), separation_test.py (voice/music quality and lyrics transcription, skipped when the AI tools are not installed)
```

Data flow for one frame:

```
Project --snapshot--> PreviewEngine (worker thread)
          FrameSource: decode each visible clip -> RGBA layers -> CompositeFrame
      --> GL or Vulkan preview: upload textures, draw one quad per layer
          (transform + effects in the shader)
```

## Building

The requirements are CMake ≥ 3.21, Ninja, a C++20 compiler, Qt ≥ 6.4 (Widgets, OpenGL,
OpenGLWidgets; Multimedia is optional), FFmpeg dev libraries (avformat, avcodec, avutil,
swscale, swresample), and SQLite3. For the Vulkan backend you also need Vulkan headers and
`glslc` or `glslangValidator`. If any of those are missing, the editor builds with OpenGL only.

### Windows (MSYS2 UCRT64): tested

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,qt6-multimedia,ffmpeg,vulkan-headers,vulkan-loader,shaderc,sqlite3}
cmake --preset release
cmake --build build/release
ctest --test-dir build/release
scripts/deploy-windows.sh build/release   # -> dist/VideoEditor (runs without MSYS2)
```

### Windows (MSVC + vcpkg)

Run this from a "Developer PowerShell for VS", with `VCPKG_ROOT` set. `vcpkg.json` installs
the dependencies.

```bash
cmake --preset msvc-vcpkg
cmake --build build/msvc-vcpkg
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install cmake ninja-build pkg-config qt6-base-dev qt6-multimedia-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libsqlite3-dev libvulkan-dev glslc
cmake --preset release && cmake --build build/release
```

### macOS (Homebrew)

```bash
brew install cmake ninja pkg-config qt ffmpeg sqlite
cmake --preset release && cmake --build build/release
```

The Vulkan backend on macOS needs MoltenVK and a Qt build with Vulkan enabled. Without
them, CMake turns it off automatically.

CMake options: `-DVE_ENABLE_VULKAN=OFF`, `-DVE_ENABLE_AUDIO=OFF`, `-DVE_BUILD_TESTS=OFF`.

## Usage

```
VideoEditor [--renderer opengl|vulkan] [project.veproj | media files...]
```

| Key | Action |
| --- | --- |
| Space | Play / pause |
| Left / Right | Step one frame (Shift: one second) |
| Home / End | Go to start / end |
| S | Split at playhead (selected clip, or all clips under the playhead) |
| Del | Delete selected clip |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+I / Ctrl+E | Import media / export video |
| I / O, Alt+X | Mark in / mark out / clear marks |
| Ctrl+X / Ctrl+Shift+X | Cut the marked range (close gap) / delete it (leave gap) |
| Shift+Del, Ctrl+G | Ripple delete / close gaps at the playhead |
| Ctrl+D | Duplicate clip |
| Shift+F | Freeze frame at the playhead |
| Ctrl+R, Ctrl+Shift+R | Reverse clip / custom speed |
| Ctrl+] / Ctrl+[ | Rotate 90° clockwise / counter-clockwise |
| Ctrl+L | Unlink / link the selected clip's audio and video |
| Ctrl+Shift+L | Linked Selection on/off |
| Alt + drag | Move or trim one clip without its linked partner |
| Ctrl+Shift+M | Show the audio mixer |
| Ctrl+Shift+T | Show the transcript / lyrics |
| Ctrl+Shift+K | Show the caption style |
| Ctrl+Shift+C | Talk to Claude (Claude panel) |
| Ctrl + wheel, Ctrl +/- | Zoom timeline |

## Known limitations

- GPU-decoded frames are copied to system memory before compositing; there is no zero-copy
  D3D11/CUDA-to-GL/Vulkan interop yet. Heavy 4K multi-layer timelines can still drop preview
  frames. The preview skips frames rather than lagging.
- Export runs on the UI thread, behind a modal progress dialog. Over MCP, `export_video`
  blocks until the file is written and sends no progress notifications.
- There are no keyframes, transitions beyond fades, text titles, or clip speed changes yet.
- Only the MSYS2 build has been compiled and tested. The Linux, macOS, and MSVC paths are
  set up but untested.
