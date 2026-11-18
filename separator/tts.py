"""Text to speech for the Video Editor.

    python tts.py TEXT.txt OUT.(wav|mp3) --engine piper|edge --voice VOICE [--rate 1.0]
    python tts.py --download-only --engine piper --voice en_US-lessac-medium

piper: offline neural voices (rhasspy/piper-voices), stored in $PIPER_VOICES.
edge:  Microsoft Edge online neural voices (the text is sent to Microsoft), MP3 output.
Progress on stdout, one JSON object per line:
    {"type": "progress", "value": 0.5, "stage": "..."}
    {"type": "done", "path": "...", "seconds": 1.2}
    {"type": "error", "message": "..."}
"""
import argparse
import json
import os
import sys
import time
import urllib.request

VOICES_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main/"


def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)


def voices_dir():
    d = os.environ.get("PIPER_VOICES") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "piper")
    os.makedirs(d, exist_ok=True)
    return d


def piper_url(voice):
    # en_US-lessac-medium -> en/en_US/lessac/medium/en_US-lessac-medium.onnx
    locale, name, quality = voice.split("-", 2)
    family = locale.split("_")[0]
    return f"{VOICES_URL}{family}/{locale}/{name}/{quality}/{voice}.onnx"


def download_piper(voice):
    d = voices_dir()
    model = os.path.join(d, voice + ".onnx")
    if os.path.exists(model) and os.path.exists(model + ".json"):
        return model
    url = piper_url(voice)
    for suffix, part in ((".json", 0.02), ("", 0.98)):
        dest = model + suffix
        tmp = dest + ".part"
        with urllib.request.urlopen(url + suffix, timeout=60) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if total and part > 0.5:
                    emit(type="progress", value=round(got / total, 3), stage="downloading")
        os.replace(tmp, dest)
    return model


def speak_piper(text, out, voice, rate):
    import wave

    from piper import PiperVoice

    model = download_piper(voice)
    pv = PiperVoice.load(model)
    length_scale = 1.0 / max(0.5, min(2.0, rate))
    with wave.open(out, "wb") as wf:
        try:  # piper-tts >= 1.3
            from piper import SynthesisConfig

            pv.synthesize_wav(text, wf, syn_config=SynthesisConfig(length_scale=length_scale))
        except ImportError:  # older API
            pv.synthesize(text, wf, length_scale=length_scale)


def speak_edge(text, out, voice, rate):
    import asyncio

    import edge_tts

    pct = int(round((max(0.5, min(2.0, rate)) - 1.0) * 100))
    comm = edge_tts.Communicate(text, voice, rate=f"{pct:+d}%")
    asyncio.run(comm.save(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--engine", default="piper", choices=["piper", "edge"])
    ap.add_argument("--voice", default="en_US-lessac-medium")
    ap.add_argument("--rate", type=float, default=1.0)
    ap.add_argument("--download-only", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    try:
        if a.download_only:
            if a.engine == "piper":
                download_piper(a.voice)
            emit(type="done", path="", seconds=round(time.time() - t0, 2))
            return 0
        with open(a.text, encoding="utf-8") as f:
            text = f.read().strip()
        if not text:
            raise ValueError("no text to speak")
        emit(type="progress", value=-1, stage="speaking")
        if a.engine == "piper":
            speak_piper(text, a.out, a.voice, a.rate)
        else:
            speak_edge(text, a.out, a.voice, a.rate)
        if not os.path.exists(a.out) or os.path.getsize(a.out) < 256:
            raise RuntimeError("no audio was produced")
        emit(type="done", path=a.out, seconds=round(time.time() - t0, 2))
        return 0
    except Exception as e:  # reported to the editor
        emit(type="error", message=f"{type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
