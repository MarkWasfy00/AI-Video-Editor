#!/usr/bin/env python3
"""End-to-end test of the video editor's MCP server.

Speaks real MCP JSON-RPC either over stdio (spawning VideoEditorMCP) or over
HTTP (a running VideoEditor GUI), runs a full editing workflow and checks that
EVERY tool advertised by tools/list was exercised.

  python mcp_e2e.py --exe build/debug/VideoEditorMCP.exe --data testdata --out build/debug/mcp-out
  python mcp_e2e.py --http http://127.0.0.1:8765/mcp --data testdata --out build/debug/mcp-out
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import urllib.request

failures = []
called = set()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


class StdioClient:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        self.next_id = 1

    def send(self, msg):
        self.p.stdin.write((json.dumps(msg) + "\n").encode())
        self.p.stdin.flush()

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("server closed stdout; stderr:\n" + self.p.stderr.read().decode(errors="replace"))
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg

    def notify(self, method):
        self.send({"jsonrpc": "2.0", "method": method})

    def close(self):
        self.p.stdin.close()
        return self.p.wait(timeout=20)


class HttpClient:
    def __init__(self, url):
        self.url = url
        self.next_id = 1
        self.session = None

    def _post(self, msg):
        req = urllib.request.Request(self.url, data=json.dumps(msg).encode(), method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Accept": "application/json, text/event-stream",
                                              **({"Mcp-Session-Id": self.session} if self.session else {})})
        with urllib.request.urlopen(req, timeout=600) as r:
            self.session = r.headers.get("Mcp-Session-Id", self.session)
            body = r.read()
            return r.status, (json.loads(body) if body else None)

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        return self._post({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})[1]

    def notify(self, method):
        status, _ = self._post({"jsonrpc": "2.0", "method": method})
        check(status == 202, "notification answered with 202 Accepted")

    def close(self):
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--http")
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    data = os.path.abspath(a.data)
    out = os.path.abspath(a.out)
    client = HttpClient(a.http) if a.http else StdioClient(a.exe)
    gui = bool(a.http)

    def call(name, args=None, expect_error=False):
        called.add(name)
        resp = client.request("tools/call", {"name": name, "arguments": args or {}})
        if "error" in resp:
            raise RuntimeError(f"{name}: JSON-RPC error {resp['error']}")
        res = resp["result"]
        if res.get("isError") != expect_error:
            texts = [c.get("text") for c in res["content"] if c["type"] == "text"]
            failures.append(f"{name} isError={res.get('isError')} (expected {expect_error}): {texts}")
            print(f"  FAIL {name}: {texts}")
        return res.get("structuredContent", {}), res

    print("== handshake")
    init = client.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "mcp_e2e", "version": "1"}})
    check(init["result"]["protocolVersion"] == "2025-06-18", "protocol version negotiated")
    check(init["result"]["serverInfo"]["name"] == "video-editor", "server info")
    check("tools" in init["result"]["capabilities"], "tools capability")
    client.notify("notifications/initialized")
    check("result" in client.request("ping"), "ping")

    tools = client.request("tools/list")["result"]["tools"]
    names = {t["name"] for t in tools}
    print(f"  {len(tools)} tools")
    check(len(tools) == 56, "56 tools advertised")
    check(all(t["inputSchema"]["type"] == "object" and "description" in t and "annotations" in t for t in tools),
          "every tool has a schema, description and annotations")
    unknown = client.request("tools/call", {"name": "does_not_exist", "arguments": {}})
    check("error" in unknown and unknown["error"]["code"] == -32602, "unknown tool -> JSON-RPC error")
    check(client.request("bogus/method")["error"]["code"] == -32601, "unknown method -> -32601")

    print("== GPU")
    info, _ = call("gpu_info")
    print("  opengl:", info["opengl"].get("renderer"))
    print("  vulkan:", [d["name"] for d in info["vulkan"].get("devices", [])])
    print("  decoders:", [d["name"] for d in info["decode"]["available"]])
    check(len(info["encoders"]) > 0, "encoders reported")
    enc, _ = call("export_encoders")
    check(any(not e["gpu"] for e in enc["encoders"]), "software encoders listed")
    r, _ = call("gpu_set_decode_mode", {"mode": "none"})
    check(r["decode"]["mode"] == "none", "decode mode -> none")
    call("gpu_set_decode_mode", {"mode": "warp-drive"}, expect_error=True)
    r, _ = call("gpu_set_decode_mode", {"mode": "auto"})
    check(r["decode"]["mode"] == "auto", "decode mode -> auto")
    if gui:
        r, _ = call("preview_set_renderer", {"renderer": "vulkan"})
        check(r["previewRenderer"].startswith("vulkan"), "GUI preview switched to Vulkan")
        call("preview_set_renderer", {"renderer": "opengl"})
    else:
        call("preview_set_renderer", {"renderer": "vulkan"}, expect_error=True)

    print("== project + media")
    call("project_new", {"discardChanges": True})
    r, _ = call("project_set_settings", {"width": 1280, "height": 720, "fps": 30, "background": "#101020", "name": "MCP test"})
    check(r["settings"]["width"] == 1280 and r["settings"]["name"] == "MCP test", "settings applied")
    probe, _ = call("media_probe", {"path": os.path.join(data, "clip_a.mp4")})
    check(probe["hasVideo"] and probe["hasAudio"] and abs(probe["duration"] - 4) < 0.1, "probe clip_a")
    imp, _ = call("media_import", {"paths": [os.path.join(data, f) for f in
                                            ("clip_a.mp4", "clip_b.mp4", "logo.png", "tone.mp3", "missing.mp4")]})
    check(len(imp["imported"]) == 4 and len(imp["errors"]) == 1, "4 imported, 1 error reported")
    ids = {os.path.basename(m["path"]): m["id"] for m in imp["imported"]}
    ml, _ = call("media_list")
    check(len(ml["media"]) == 4, "media_list")

    print("== tracks")
    tl, _ = call("track_list")
    check([t["name"] for t in tl["tracks"]] == ["V2", "V1", "A1", "A2"], "default tracks V2/V1/A1/A2")
    check([t["kind"] for t in tl["tracks"]] == ["video", "video", "audio", "audio"], "video tracks first, then audio")
    tl, _ = call("track_add", {"name": "FX"})
    check(tl["tracks"][0]["name"] == "FX" and tl["tracks"][0]["kind"] == "video", "track_add puts a video track on top")
    tl, _ = call("track_update", {"track": 0, "name": "Overlay", "hidden": True})
    check(tl["tracks"][0]["name"] == "Overlay" and tl["tracks"][0]["hidden"], "track_update rename + hide")
    tl, _ = call("track_remove", {"track": 0})
    check(len(tl["tracks"]) == 4, "track_remove")
    tl, _ = call("track_add", {"kind": "audio"})
    check(tl["tracks"][-1]["name"] == "A3" and tl["tracks"][-1]["kind"] == "audio", "track_add audio goes last")
    call("track_remove", {"track": 4})
    call("track_add", {"kind": "subtitles"}, expect_error=True)
    call("track_remove", {"track": 99}, expect_error=True)

    print("== clips (video + linked audio)")
    a, _ = call("clip_add", {"mediaId": ids["clip_a.mp4"], "track": 1, "start": 0})
    b, _ = call("clip_add", {"mediaId": ids["clip_b.mp4"], "track": 1, "start": 4})
    logo, _ = call("clip_add", {"mediaId": ids["logo.png"], "track": 0, "start": 1})
    tone, _ = call("clip_add", {"mediaId": ids["tone.mp3"]})  # auto track, at playhead (0)
    check(a["duration"] == 4 and b["start"] == 4 and logo["duration"] == 5, "clip_add timings")
    check(len(a["linked"]) == 1 and a["linked"][0]["track"] == 2 and a["linked"][0]["trackKind"] == "audio"
          and a["linked"][0]["start"] == 0 and a["linked"][0]["duration"] == 4, "video with sound -> linked audio clip on A1")
    check(b["linked"] == [] and logo["linked"] == [], "silent video / image: no audio clip")
    check(tone["track"] == 3 and tone["trackKind"] == "audio", "audio file -> first free audio track (A2)")
    sp, _ = call("clip_split", {"clipId": a["id"], "time": 2})
    left, right = sp["left"], sp["right"]
    check(left["duration"] == 2 and right["start"] == 2 and right["in"] == 2, "clip_split")
    right_audio = call("clip_get", {"clipId": right["linkedClips"][0]})[0] if right["linkedClips"] else {}
    check(right_audio.get("start") == 2 and right_audio.get("trackKind") == "audio"
          and left["linkedClips"] != right["linkedClips"], "split also split the audio; halves linked pairwise")
    call("clip_split", {"clipId": a["id"], "time": 30}, expect_error=True)
    up, _ = call("clip_update", {"clipId": right["id"], "name": "B&W part",
                                 "effects": {"grayscale": True, "blur": 2},
                                 "transform": {"scale": 0.8, "rotation": 5}, "fadeOut": 0.5})
    check(up["effects"]["grayscale"] and up["effects"]["blur"] == 2 and up["transform"]["scale"] == 0.8
          and up["fadeOut"] == 0.5 and up["name"] == "B&W part", "clip_update effects/transform/fades")
    call("clip_update", {"clipId": right_audio["id"], "volume": 0.5})
    call("clip_update", {"clipId": right["id"], "effects": {"blur": "lots"}}, expect_error=True)
    tr, _ = call("clip_trim", {"clipId": left["id"], "end": 1.5})
    la = call("clip_get", {"clipId": left["linkedClips"][0]})[0]
    check(abs(tr["duration"] - 1.5) < 1e-6 and abs(la["duration"] - 1.5) < 1e-6, "clip_trim end (audio follows)")
    tr, _ = call("clip_trim", {"clipId": logo["id"], "start": 1.5})
    check(abs(tr["start"] - 1.5) < 1e-6 and abs(tr["end"] - 6) < 1e-6, "clip_trim start keeps end")
    mv, _ = call("clip_move", {"clipId": b["id"], "start": 5})
    check(mv["start"] == 5 and mv["track"] == 1, "clip_move")
    call("clip_move", {"clipId": b["id"], "track": 2}, expect_error=True)  # video onto an audio track
    mv, _ = call("clip_move", {"clipId": right_audio["id"], "start": 2.5, "linked": False})
    check(mv["start"] == 2.5 and call("clip_get", {"clipId": right["id"]})[0]["start"] == 2, "clip_move linked=false")
    call("edit_undo")
    un, _ = call("clip_unlink", {"clipId": right["id"]})
    check(len(un["unlinked"]) == 2 and call("clip_get", {"clipId": right["id"]})[0]["linkedClips"] == [], "clip_unlink")
    lk, _ = call("clip_link", {"clipId": right["id"]})  # finds its own sound
    check(lk["linkedClips"] == [right_audio["id"]], "clip_link with one clip re-links its audio")
    call("clip_unlink", {"clipId": right["id"]})
    lk, _ = call("clip_link", {"clipIds": [right["id"], right_audio["id"]]})
    check(lk["linkedClips"] == [right_audio["id"]], "clip_link")
    call("clip_link", {"clipId": right["id"]}, expect_error=True)  # already linked
    call("clip_link", {"clipIds": [right["id"]]}, expect_error=True)
    g, _ = call("clip_get", {"clipId": right["id"]})
    check(g["effects"]["grayscale"], "clip_get")
    call("clip_get", {"clipId": "nope"}, expect_error=True)
    cl, _ = call("clip_list", {"track": 1})
    check(len(cl["clips"]) == 3, "clip_list on track 1")
    cl, _ = call("clip_list", {"track": 2})
    check(len(cl["clips"]) == 2, "clip_list on A1: the two halves of clip_a's sound")

    print("== history")
    h, _ = call("edit_history")
    n = h["index"]
    check(n >= 10, f"history has {n} steps")
    u, _ = call("edit_undo")
    check(u["history"]["index"] == n - 1, "edit_undo")
    r, _ = call("edit_redo")
    check(r["history"]["index"] == n, "edit_redo")
    check(call("clip_get", {"clipId": b["id"]})[0]["start"] == 5, "redo kept the move")
    tmp, _ = call("clip_add", {"mediaId": ids["clip_a.mp4"], "track": 0, "start": 9})
    rm, _ = call("clip_remove", {"clipId": tmp["id"]})
    check(len(rm["removed"]) == 2, "clip_remove takes the linked audio along")
    call("clip_get", {"clipId": tmp["linked"][0]["id"]}, expect_error=True)
    sep, _ = call("clip_add", {"mediaId": ids["clip_a.mp4"], "track": 0, "start": 9, "link": False})
    check(sep["linked"] == [] and sep["audioClip"]["trackKind"] == "audio" and sep["audioClip"]["linkedClips"] == [],
          "clip_add link=false keeps the audio separate")
    rm, _ = call("clip_remove", {"clipId": sep["id"]})
    check(len(rm["removed"]) == 1, "removing unlinked video leaves its audio")
    call("clip_remove", {"clipId": sep["audioClip"]["id"]})

    print("== speed, reverse, freeze, duplicate, ripple, crop, resize")
    sp2, _ = call("clip_set_speed", {"clipId": b["id"], "speed": 2})
    check(abs(sp2["duration"] - 1.5) < 0.02 and abs(sp2["sourceSpan"] - 3) < 0.02, "clip_set_speed 2x")
    ramp, _ = call("clip_set_speed", {"clipId": b["id"], "speed": 1, "curve": "ease-in"})
    check(ramp["curvePreset"] == "ease-in" and len(ramp["speedCurve"]) >= 2, "speed curve preset")
    cus, _ = call("clip_set_speed", {"clipId": b["id"], "curve": [[0, 1], [1, 3]]})
    check(cus["curvePreset"] == "custom", "custom speed curve")
    call("clip_set_speed", {"clipId": b["id"], "curve": "warp"}, expect_error=True)
    call("clip_set_speed", {"clipId": b["id"], "speed": 1, "curve": "none"})
    rv, _ = call("clip_reverse", {"clipId": right["id"]})
    check(rv["reverse"] and call("clip_get", {"clipId": right_audio["id"]})[0]["reverse"], "clip_reverse (with its audio)")
    call("clip_reverse", {"clipId": right["id"], "reverse": False})
    fz, _ = call("clip_freeze_frame", {"clipId": b["id"], "time": 5.5, "duration": 1})
    check(fz["freeze"] and fz["start"] == 5.5 and abs(fz["duration"] - 1) < 1e-6, "clip_freeze_frame")
    call("clip_freeze_frame", {"clipId": right_audio["id"], "time": 2.5}, expect_error=True)
    call("edit_undo")
    dp, _ = call("clip_duplicate", {"clipId": b["id"]})
    check(dp["start"] == call("clip_get", {"clipId": b["id"]})[0]["end"], "clip_duplicate placed after the original")
    rd, _ = call("clip_ripple_delete", {"clipId": dp["id"]})
    check(len(rd["removed"]) == 1, "clip_ripple_delete")
    call("clip_move", {"clipId": b["id"], "start": 6})
    gap_start = call("clip_get", {"clipId": right["id"]})[0]["end"]
    cg, _ = call("timeline_close_gap", {"track": 1, "time": 5.5})
    check(abs(cg["closed"] - (6 - gap_start)) < 1e-6 and call("clip_get", {"clipId": b["id"]})[0]["start"] == gap_start,
          "timeline_close_gap")
    call("clip_move", {"clipId": b["id"], "start": 5})  # back where the later sections expect it
    call("timeline_close_gap", {"track": 1, "time": 0.5}, expect_error=True)  # inside a clip
    before = call("playback_state")[0]["duration"]
    cr, _ = call("timeline_cut_range", {"start": 0.5, "end": 1.0})
    check(abs(cr["duration"] - (before - 0.5)) < 1e-3, "timeline_cut_range closes the gap")
    call("edit_undo")
    call("timeline_cut_range", {"start": 2, "end": 1}, expect_error=True)
    cp, _ = call("clip_update", {"clipId": right["id"], "transform": {"cropLeft": 0.1, "cropBottom": 0.2}})
    check(cp["transform"]["cropLeft"] == 0.1 and cp["transform"]["cropBottom"] == 0.2, "crop")
    call("clip_update", {"clipId": right["id"], "transform": {"cropLeft": 0.6, "cropRight": 0.6}}, expect_error=True)
    call("clip_update", {"clipId": right["id"], "transform": {"cropLeft": 0, "cropBottom": 0}})
    fl, _ = call("clip_update", {"clipId": logo["id"], "fit": "fill"})  # square logo on a wide canvas
    check(fl["transform"]["scale"] > 1.5, "fit=fill scales up")
    call("clip_update", {"clipId": logo["id"], "fit": "fit"})
    canvas = call("project_get")[0]["settings"]
    ps, _ = call("project_set_settings", {"aspectRatio": "9:16"})
    check(ps["settings"]["width"] == 1080 and ps["settings"]["height"] == 1920, "resize to 9:16")
    call("project_set_settings", {"width": canvas["width"], "height": canvas["height"]})
    call("project_set_settings", {"aspectRatio": "wide"}, expect_error=True)

    # Voice/music separation: covered in depth by separation_test.py. Here: argument checks only
    # (never triggers the one-time install).
    call("audio_separate", {"clipId": tone["id"], "keep": "drums"}, expect_error=True)
    call("audio_separate", {"clipId": "nope"}, expect_error=True)
    # Transcripts: transcription itself is tested in separation_test.py; here the editing tools.
    call("transcript_create", {"clipId": "nope"}, expect_error=True)
    tg, _ = call("transcript_get")
    check(tg["lines"] == [], "transcript_get: nothing transcribed yet")
    call("transcript_export", {"path": os.path.join(out, "none.srt")}, expect_error=True)
    tu, _ = call("transcript_update", {"mediaId": ids["tone.mp3"], "insert": {"start": 0.5, "end": 1.5, "text": "la la la"}})
    check(len(tu["lines"]) == 1 and tu["lines"][0]["text"] == "la la la", "transcript_update insert (shown on the timeline)")
    tu, _ = call("transcript_update", {"mediaId": ids["tone.mp3"], "segment": 0, "text": "na na na"})
    check(tu["transcripts"][0]["segments"][0]["edited"] and tu["lines"][0]["text"] == "na na na", "transcript_update text")
    cg, _ = call("captions_set", {"preset": "bold"})
    check(cg["style"]["enabled"] and cg["style"]["uppercase"] and cg["captionCount"] == 1, "captions_set preset turns captions on")
    cg, _ = call("captions_set", {"position": "top", "size": 0.08, "highlight": "#00FF88", "wordsPerCaption": 2})
    check(cg["style"]["position"] == "top" and cg["style"]["wordsPerCaption"] == 2 and cg["captionCount"] == 2,
          "captions_set overrides")
    rf_cap, _ = call("render_frame", {"time": 0.7})
    check(rf_cap["layers"] >= 1, "caption rendered as a layer")
    call("captions_set", {"position": "sideways"}, expect_error=True)
    call("captions_set", {"color": "not-a-color"}, expect_error=True)
    call("captions_set", {"preset": "comic"}, expect_error=True)
    fonts = call("captions_get")[0]["suggestedFonts"]
    if fonts:
        cf, _ = call("captions_set", {"font": fonts[0], "italic": True})
        check(cf["style"]["font"] == fonts[0] and cf["style"]["italic"] and cf["style"]["fontFile"] == "", "captions_set font")
    call("captions_set", {"font": "No Such Font 123"}, expect_error=True)
    call("captions_set", {"fontFile": os.path.join(data, "tone.mp3")}, expect_error=True)
    cga, _ = call("captions_get")
    check(len(cga["suggestedArabicFonts"]) > 0 and cga["arabicFontInUse"] != "", "Arabic fonts listed")
    if cga["suggestedArabicFonts"]:
        cf, _ = call("captions_set", {"arabicFont": cga["suggestedArabicFonts"][0]})
        check(cf["style"]["arabicFont"] == cga["suggestedArabicFonts"][0], "captions_set arabicFont")
    call("captions_set", {"arabicFont": "No Such Arabic Font"}, expect_error=True)
    tu, _ = call("transcript_update", {"mediaId": ids["tone.mp3"], "segment": 0, "text": "إزيك يا صاحبي"})
    check(tu["lines"][0]["text"] == "إزيك يا صاحبي", "Arabic transcript text round-trips")
    tu, _ = call("transcript_update", {"mediaId": ids["tone.mp3"], "segment": 0, "text": "na na na"})
    cg, _ = call("captions_get")
    check("bold" in cg["presets"] and cg["firstCaptions"][0]["text"] == "na na", "captions_get")
    call("captions_set", {"enabled": False})
    ex, _ = call("transcript_export", {"path": os.path.join(out, "lyrics.lrc")})
    check(ex["lines"] == 1 and open(os.path.join(out, "lyrics.lrc"), encoding="utf-8").read().strip().endswith("na na na"),
          "transcript_export lrc")
    call("transcript_export", {"path": os.path.join(out, "x.doc")}, expect_error=True)
    call("transcript_update", {"mediaId": ids["tone.mp3"], "segment": 5, "text": "x"}, expect_error=True)
    call("transcript_update", {"mediaId": ids["tone.mp3"], "segment": 0, "delete": True})

    print("== audio mixer")
    # Mixer = audio tracks only: A1 (track 2, clip_a's sound) and A2 (track 3, tone).
    mx, _ = call("mixer_get")
    check([t["index"] for t in mx["tracks"]] == [2, 3] and mx["master"]["gainDb"] == 0 and not mx["soloActive"],
          "mixer_get lists the audio tracks")
    check(mx["tracks"][0]["hasAudioClips"] and mx["tracks"][1]["hasAudioClips"], "mixer_get hasAudioClips")
    mx, _ = call("mixer_set_track", {"track": 3, "solo": True})
    check(mx["soloActive"] and not mx["tracks"][0]["audible"] and mx["tracks"][1]["audible"], "solo silences others")
    an0, _ = call("mixer_analyze", {"start": 0, "end": 1})
    p0 = an0["master"]["peakDb"][0]
    check(an0["tracks"][0]["peakDb"][0] == -60 and an0["tracks"][1]["peakDb"][0] > -40, f"analyze solo (peak {p0})")
    call("mixer_set_track", {"track": 3, "gainDb": -6})
    an1, _ = call("mixer_analyze", {"start": 0, "end": 1})
    check(abs(an1["master"]["peakDb"][0] - (p0 - 6)) < 0.3, f"-6 dB fader: {p0} -> {an1['master']['peakDb'][0]}")
    call("mixer_set_track", {"track": 3, "pan": -1})
    an2, _ = call("mixer_analyze", {"start": 0, "end": 1})
    check(an2["master"]["peakDb"][1] == -60 and an2["master"]["peakDb"][0] > -40, "pan hard left")
    mx, _ = call("mixer_set_track", {"track": 3, "pan": 0, "gainDb": -2, "solo": False})
    check(mx["tracks"][1]["gainDb"] == -2 and not mx["soloActive"], "mixer_set_track partial update")
    call("mixer_set_track", {"track": 3, "volume": 1}, expect_error=True)
    call("mixer_set_track", {"track": 99, "gainDb": 0}, expect_error=True)
    call("mixer_set_track", {"track": 1, "gainDb": 0}, expect_error=True)  # video track
    mx, _ = call("mixer_set_master", {"muted": True})
    an3, _ = call("mixer_analyze")
    check(an3["master"]["peakDb"] == [-60, -60] and "silent" in an3["advice"], "master mute -> silent")
    mx, _ = call("mixer_set_master", {"muted": False, "gainDb": -3})
    check(mx["master"]["gainDb"] == -3 and not mx["master"]["muted"], "mixer_set_master")
    call("mixer_set_master", {"gainDb": -10})
    u, _ = call("edit_undo")
    check(u["undone"] == "Mixer: Master" and call("mixer_get")[0]["master"]["gainDb"] == -3, "mixer edits are undoable")
    call("mixer_set_master", {"gainDb": 12})
    call("mixer_set_track", {"track": 3, "gainDb": 12})
    an4, _ = call("mixer_analyze")
    check(an4["master"]["clippedSamples"] > 0 and "clips" in an4["advice"],
          f"analyze detects clipping at +24 dB (peak {an4['master']['peakDb']})")
    call("edit_undo")
    call("edit_undo")
    tl, _ = call("track_update", {"track": 2, "gainDb": 1.5, "pan": 0.25})
    check(tl["tracks"][2]["gainDb"] == 1.5 and tl["tracks"][2]["pan"] == 0.25, "track_update mixer fields")
    call("mixer_analyze", {"start": 5, "end": 2}, expect_error=True)

    print("== playback + selection")
    st, _ = call("playback_seek", {"time": 2.5})
    check(abs(st["position"] - 2.5) < 0.02, "playback_seek")
    st, _ = call("playback_state")
    check(abs(st["duration"] - 8) < 0.01, f"duration {st['duration']}")
    if gui:
        st, _ = call("playback_play")
        check(st["playing"], "playback_play")
    else:
        call("playback_play", expect_error=True)
    st, _ = call("playback_pause")
    check(not st["playing"], "playback_pause")
    sel, _ = call("selection_set", {"clipId": right["id"]})
    check(sel["selected"]["id"] == right["id"], "selection_set")
    sel, _ = call("selection_get")
    check(sel["selected"]["id"] == right["id"], "selection_get")
    call("media_remove", {"mediaId": ids["clip_a.mp4"]}, expect_error=True)  # in use

    print("== render (GPU)")
    rf, res = call("render_frame", {"time": 3, "path": os.path.join(out, "mcp_frame.png"), "maxWidth": 640})
    images = [c for c in res["content"] if c["type"] == "image"]
    check(rf["layers"] == 2 and rf["width"] == 1280, "render_frame composited 2 layers at 1280x720")
    check(len(images) == 1 and images[0]["mimeType"] == "image/png", "render_frame returns a PNG image")
    if images:
        with open(os.path.join(out, "mcp_preview.png"), "wb") as f:
            f.write(base64.b64decode(images[0]["data"]))
    check(os.path.getsize(os.path.join(out, "mcp_frame.png")) > 1000, "full-size frame written")

    print("== save / open")
    sv, _ = call("project_save", {"path": os.path.join(out, "mcp_project.veproj")})
    check(sv["format"] == "sqlite", "project_save sqlite")
    sj, _ = call("project_save", {"path": os.path.join(out, "mcp_project.json")})
    check(sj["format"] == "json", "project_save json")
    call("project_new")  # saved -> allowed without discardChanges
    call("clip_add", {"mediaId": "x"}, expect_error=True)
    call("track_add")
    call("project_open", {"path": os.path.join(out, "mcp_project.veproj")}, expect_error=True)  # unsaved change
    op, _ = call("project_open", {"path": os.path.join(out, "mcp_project.veproj"), "discardChanges": True})
    check(op["clipCount"] == 7 and op["missingMedia"] == [], f"project_open restores 7 clips ({op['clipCount']})")
    pj, _ = call("project_get")
    restored = [c for t in pj["tracks"] for c in t["clips"] if c["id"] == right["id"]]
    check(restored and restored[0]["effects"]["grayscale"] and restored[0]["fadeOut"] == 0.5,
          "effects survived save/open")
    mx, _ = call("mixer_get")
    check(mx["master"]["gainDb"] == -3 and mx["tracks"][1]["gainDb"] == -2 and mx["tracks"][0]["pan"] == 0.25,
          "mixer settings survived save/open")
    unused, _ = call("media_import", {"paths": [os.path.join(data, "tone.mp3")]})
    call("media_remove", {"mediaId": unused["imported"][0]["id"]})

    print("== export (GPU)")
    ex, _ = call("export_video", {"path": os.path.join(out, "mcp_export.mp4"), "encoder": "auto",
                                  "width": 640, "height": 360, "end": 3})
    print(f"  encoder={ex['encoder']} gpu={ex['gpuEncoder']} {ex['bytes']} bytes in {ex['elapsedSeconds']}s")
    check(ex["bytes"] > 10000, "export_video wrote a file")
    call("export_video", {"path": os.path.join(out, "x.mp4"), "encoder": "no_such_encoder"}, expect_error=True)
    pr, _ = call("media_probe", {"path": ex["path"]})
    check(pr["width"] == 640 and pr["hasAudio"] and abs(pr["duration"] - 3) < 0.2, "exported file is valid")

    missing = names - called
    check(not missing, "every advertised tool was exercised" + (f" (missing: {sorted(missing)})" if missing else ""))
    rc = client.close()
    if not gui:
        check(rc == 0, "server exits cleanly on stdin EOF")

    print(f"\n{len(called)} tools exercised, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
