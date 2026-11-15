"""Voice / music separation through the MCP server (audio_separate).

Skips (exit 0) when the separation environment is not installed. Otherwise it
separates testdata/separation/mix.flac (Windows speech + synthetic music) and
checks the timeline edit and the quality of the stems against the originals
(scored with the separation environment's numpy/soundfile).

usage: separation_test.py --exe VideoEditorMCP --data testdata
"""
import argparse
import json
import os
import subprocess
import sys

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


class Stdio:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "separation-test", "version": "1"}})

    def rpc(self, method, params):
        self.n += 1
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n").encode())
        self.p.stdin.flush()
        while True:
            msg = json.loads(self.p.stdout.readline())
            if msg.get("id") == self.n:
                return msg

    def call(self, name, args=None):
        r = self.rpc("tools/call", {"name": name, "arguments": args or {}})["result"]
        if r.get("isError"):
            raise RuntimeError(r["content"][0]["text"])
        return r["structuredContent"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(10)


SCORE = r"""
import sys, json, numpy as np, soundfile as sf
def load(p):
    a, sr = sf.read(p, dtype='float32', always_2d=True); return a.mean(1)
v, m, rv, rm, mix = (load(p) for p in sys.argv[1:6])
n = min(map(len, (v, m, rv, rm, mix)))
v, m, rv, rm, mix = (x[:n] for x in (v, m, rv, rm, mix))
sdr = lambda ref, est: float(10*np.log10(np.sum(ref**2) / (np.sum((ref-est)**2) + 1e-12)))
print(json.dumps({"voice": sdr(rv, v), "music": sdr(rm, m), "voiceMix": sdr(rv, mix), "sum": sdr(mix, v + m)}))
"""


def main():
    try:  # Arabic output on a non-UTF-8 console
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--data", required=True)
    args = ap.parse_args()
    data = os.path.abspath(os.path.join(args.data, "separation"))

    s = Stdio(args.exe)
    status = s.call("project_get")  # warm-up
    tools = [t["name"] for t in s.rpc("tools/list", {})["result"]["tools"]]
    check("audio_separate" in tools, "audio_separate is advertised")
    try:
        imp = s.call("media_import", {"paths": [os.path.join(data, "mix.flac")]})
        mix = s.call("clip_add", {"mediaId": imp["imported"][0]["id"], "start": 1})
        try:
            r = s.call("audio_separate", {"clipId": mix["id"]})
        except RuntimeError as e:
            if "not installed" in str(e):
                print("SKIP: voice/music separation is not installed on this machine")
                return 0
            raise
        print(f"  separated on {r.get('device', 'cache')} in {r.get('seconds', 0)} s (cached: {r['cached']})")
        v, m = r["voice"], r["music"]
        check(v["trackKind"] == "audio" and m["trackKind"] == "audio" and v["track"] != m["track"],
              "voice and music on separate audio tracks")
        check(v["start"] == 1 and m["start"] == 1 and v["duration"] == mix["duration"], "stems keep the clip's timing")
        check(v["linkedClips"] == [m["id"]], "voice and music are linked to each other")
        check(all(c["id"] != mix["id"] for c in s.call("clip_list")["clips"]), "the original sound was replaced")
        s.call("edit_undo")
        check(any(c["id"] == mix["id"] for c in s.call("clip_list")["clips"]), "undo brings the original back")
        r2 = s.call("audio_separate", {"clipId": mix["id"], "keep": "music"})
        check(r2["cached"] and "music" in r2 and "voice" not in r2, "keep=music reuses the cached stems")
        try:
            s.call("audio_separate", {"clipId": mix["id"], "keep": "drums"})
            check(False, "invalid keep rejected")
        except RuntimeError:
            check(True, "invalid keep rejected")

        # Quality against the original voice and music (SDR, dB).
        st = s.call("project_get")
        env_python = None
        for c in ("Scripts/python.exe", "bin/python"):
            p = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~/.local/share")),
                             "VideoEditor", "separator", ".venv", c)
            if os.path.exists(p):
                env_python = p
        if env_python:
            out = subprocess.run([env_python, "-I", "-c", SCORE, r["voiceFile"], r["musicFile"],
                                  os.path.join(data, "voice.flac"), os.path.join(data, "music.flac"),
                                  os.path.join(data, "mix.flac")], capture_output=True, text=True)
            q = json.loads(out.stdout)
            print(f"  SDR voice {q['voice']:.1f} dB (unseparated {q['voiceMix']:.1f}), music {q['music']:.1f} dB, "
                  f"voice+music vs mix {q['sum']:.1f} dB")
            check(q["voice"] > 8 and q["music"] > 8, "voice and music are cleanly separated (SDR > 8 dB)")
            check(q["sum"] > 20, "voice + music add back up to the original")
        s.call("edit_undo")  # back to the original mix clip
        # Transcription of the same song (voice separated from the music first).
        try:
            t = s.call("transcript_create", {"clipId": mix["id"]})
        except RuntimeError as e:
            if "not installed" in str(e):
                print("SKIP transcription: Whisper is not installed")
                t = None
            else:
                raise
        if t:
            text = " ".join(l["text"] for l in t["lines"]).lower()
            print(f"  transcribed in {t['seconds']} s on {t['device']} (voice isolated: {t['isolatedVoice']}): {text[:80]}...")
            check(all(w in text for w in ("welcome", "video editor", "voice", "music")), "transcript has the spoken words")
            check(t["lines"][0]["start"] >= 1 and t["lines"][0]["words"], "lines on the timeline with word timings")
            cap = s.call("captions_set", {"preset": "classic"})
            check(cap["captionCount"] >= 4, f"captions from the transcript ({cap['captionCount']})")
            first = cap["firstCaptions"][0]
            frame = s.call("render_frame", {"time": (first["start"] + first["end"]) / 2})
            check(frame["layers"] >= 1, "caption drawn over the frame")
            mid = t["transcripts"][0]["mediaId"]
            # Egyptian Arabic: a song (Egyptian speech + music), dialect mode, Arabic captions.
            arpath = os.path.join(os.path.abspath(args.data), "arabic", "egyptian_song.flac")
            if os.path.exists(arpath):
                ar_media = s.call("media_import", {"paths": [arpath]})["imported"][0]["id"]
                ar_clip = s.call("clip_add", {"mediaId": ar_media, "start": 30})
                at = s.call("transcript_create", {"clipId": ar_clip["id"], "language": "ar-EG"})
                artext = " ".join(l["text"] for l in at["lines"])
                print(f"  Egyptian Arabic ({at['seconds']} s): {artext}")
                check(at["transcripts"][0]["language"] == "ar-EG", "transcript marked as Egyptian Arabic")
                hits = [w for w in ("إزيك", "صاحبي", "النهارده", "فيديو", "الموسيقى", "الشاشة") if w in artext]
                check(len(hits) >= 5, f"Egyptian words recognised ({len(hits)}/6: {' '.join(hits)})")
                ac = s.call("captions_set", {"preset": "arabic"})
                arcaps = [c for c in ac["firstCaptions"] if c["start"] >= 30]
                check(len(arcaps) >= 3 or ac["captionCount"] >= 8, "Arabic captions created")
            u = s.call("transcript_update", {"mediaId": mid, "segment": 0, "text": "Edited first line"})
            check(u["lines"][0]["text"] == "Edited first line", "edit a transcript line")
    finally:
        s.close()
    print(f"\n{len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
