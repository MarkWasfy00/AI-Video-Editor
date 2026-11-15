"""Separate voice and music with Demucs (htdemucs). Used by the Video Editor.

    python separate.py INPUT.wav OUTDIR [--model htdemucs] [--device auto|cuda|cpu]
    python separate.py --download-only

INPUT is a stereo WAV/FLAC (the editor writes 44.1 kHz float WAV). Writes
OUTDIR/voice.flac and OUTDIR/music.flac (music = drums + bass + other).
Progress is reported on stdout as one JSON object per line:
    {"type": "progress", "value": 0.42}
    {"type": "done", "voice": "...", "music": "...", "device": "cuda", "seconds": 12.3}
    {"type": "error", "message": "..."}
"""
import argparse
import json
import sys
import time


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def load_model(name):
    from demucs.pretrained import get_model

    model = get_model(name)
    model.eval()
    return model


def separate(args):
    import numpy as np
    import soundfile as sf
    import torch
    from demucs.apply import apply_model

    t0 = time.time()
    device = args.device
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"

    emit({"type": "progress", "value": 0.0, "stage": "loading model"})
    model = load_model(args.model)
    rate = model.samplerate

    audio, sr = sf.read(args.input, dtype="float32", always_2d=True)  # (frames, channels)
    if sr != rate:
        raise RuntimeError(f"input must be {rate} Hz (got {sr})")
    if audio.shape[1] == 1:
        audio = np.repeat(audio, 2, axis=1)
    audio = audio[:, :2]
    wav = torch.from_numpy(audio.T.copy())  # (2, frames)

    # Same normalisation as demucs' own CLI.
    ref = wav.mean(0)
    mean, std = ref.mean(), ref.std() + 1e-8
    wav = (wav - mean) / std

    names = list(model.sources)
    vocals_idx = names.index("vocals")
    frames = wav.shape[1]
    chunk = int(rate * args.chunk_seconds)
    overlap = int(rate * 1.0)
    voice = torch.zeros(2, frames)
    music = torch.zeros(2, frames)
    weight = torch.zeros(frames)

    pos = 0
    with torch.no_grad():
        while pos < frames:
            end = min(frames, pos + chunk)
            a = max(0, pos - overlap)
            piece = wav[:, a:end]
            out = apply_model(model, piece[None], device=device, shifts=1, split=True, overlap=0.25,
                              progress=False)[0].cpu()  # (sources, 2, n)
            v = out[vocals_idx]
            m = out.sum(0) - v
            n = end - a
            # Linear cross-fade over the overlap with the previous chunk.
            w = torch.ones(n)
            if a < pos:
                w[: pos - a] = torch.linspace(0.0, 1.0, pos - a)
            voice[:, a:end] += v * w
            music[:, a:end] += m * w
            weight[a:end] += w
            pos = end
            emit({"type": "progress", "value": round(pos / frames, 4), "stage": "separating"})

    weight = weight.clamp(min=1e-6)
    voice = voice / weight * std + mean / 2
    music = music / weight * std + mean / 2

    import os

    os.makedirs(args.outdir, exist_ok=True)
    vpath = os.path.join(args.outdir, "voice.flac")
    mpath = os.path.join(args.outdir, "music.flac")
    sf.write(vpath, voice.T.clamp(-1, 1).numpy(), rate, subtype="PCM_24")
    sf.write(mpath, music.T.clamp(-1, 1).numpy(), rate, subtype="PCM_24")
    emit({"type": "done", "voice": vpath, "music": mpath, "device": device,
          "seconds": round(time.time() - t0, 2), "sampleRate": rate})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--model", default="htdemucs")
    ap.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    ap.add_argument("--chunk-seconds", type=float, default=30.0)
    ap.add_argument("--download-only", action="store_true")
    args = ap.parse_args()
    try:
        if args.download_only:
            import torch

            load_model(args.model)
            emit({"type": "done", "model": args.model, "cuda": torch.cuda.is_available()})
            return 0
        if not args.input or not args.outdir:
            ap.error("INPUT and OUTDIR are required")
        separate(args)
        return 0
    except Exception as e:  # reported to the editor
        emit({"type": "error", "message": f"{type(e).__name__}: {e}"})
        return 1


if __name__ == "__main__":
    sys.exit(main())
