"""AI video upscaling (super-resolution) for the Video Editor.

Models (see src/media/AiModels.cpp for the hardware each one needs):
    flashvsr-full        FlashVSR v1.1, Wan VAE decoder       best quality, NVIDIA RTX, most VRAM
    flashvsr-tiny        FlashVSR v1.1, tiny decoder          near-full quality, faster, less VRAM
    flashvsr-tiny-long   FlashVSR v1.1, tiny decoder, streamed lowest FlashVSR VRAM (long videos)
    realesrgan-x4plus    Real-ESRGAN x4plus (per frame)       any NVIDIA GPU, or CPU (slow)
    realesrgan-general   Real-ESRGAN general v3 (compact)     fast, runs on any computer

    python upscale.py INPUT OUTPUT --model ID --scale 2|4 [--start S] [--duration D]
                      [--device auto|cuda:N|cpu] [--precision bf16|fp16|fp32]
                      [--tile-dit] [--tile-vae] [--unload-dit] [--tile-size 256] [--chunk-frames N]
    python upscale.py --download-only --model ID

FlashVSR (https://github.com/OpenImagingLab/FlashVSR, Apache-2.0) runs through the low-VRAM
implementation of ComfyUI-FlashVSR_Ultra_Fast (https://github.com/lihaoyun6/ComfyUI-FlashVSR_Ultra_Fast,
GPL-3.0). Its "sparse sage" attention needs no compiled CUDA extension, so it also works on Windows.
That code and the model weights are downloaded on first use (pinned versions, checked) into
$VE_UPSCALER_HOME; nothing of it ships with the editor.

Progress on stdout, one JSON object per line:
    {"type": "progress", "value": 0.42, "stage": "upscaling", "frame": 120, "frames": 300}
    {"type": "done", "path": "...", "width": 3840, "height": 2160, "frames": 300, "device": "cuda:0", "seconds": 81.2}
    {"type": "error", "message": "..."}
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
import threading
import time
import types
import urllib.request
import zipfile

HOME = os.environ.get("VE_UPSCALER_HOME") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "upscaler-data")
MODELS_DIR = os.path.join(HOME, "models")
CODE_DIR = os.path.join(HOME, "flashvsr_code")
CODE_PACKAGE = "flashvsr_ultra"

# Pinned FlashVSR runtime code (commit) and weights (Hugging Face revision).
FLASHVSR_CODE_COMMIT = "4820b3f02347bddcbbb9a5a85ab7638fe976366e"
FLASHVSR_CODE_URL = ("https://github.com/lihaoyun6/ComfyUI-FlashVSR_Ultra_Fast/archive/"
                     + FLASHVSR_CODE_COMMIT + ".zip")
# sha256 over the sorted "path:sha256(file)" lines of the extracted files (see code_manifest()).
FLASHVSR_CODE_MANIFEST = "3150b63d21638cdb08df7d89e43d4d555d832498675bb98106b6647a88a9ac7e"
FLASHVSR_REPO = "JunhaoZhuang/FlashVSR-v1.1"
FLASHVSR_REVISION = "2aa7e1f3ab50e743537d1eca243fb0bff442b336"
FLASHVSR_DIR_NAME = "FlashVSR-v1.1"
FLASHVSR_FILES = {
    "diffusion_pytorch_model_streaming_dmd.safetensors": 5676070392,
    "LQ_proj_in.ckpt": 575694948,
    "TCDecoder.ckpt": 189018333,
    "Wan2.1_VAE.pth": 507609880,   # needed by every mode (the pipeline loader checks for it)
}

ESRGAN = {
    "realesrgan-x4plus": ("RealESRGAN_x4plus.pth",
                          "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
                          "4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1"),
    "realesrgan-general": ("realesr-general-x4v3.pth",
                           "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesr-general-x4v3.pth",
                           "8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292"),
}
FLASHVSR_MODES = {"flashvsr-full": "full", "flashvsr-tiny": "tiny", "flashvsr-tiny-long": "tiny-long"}
MODELS = list(FLASHVSR_MODES) + list(ESRGAN)


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def progress(value, stage, **extra):
    emit(dict({"type": "progress", "value": round(max(0.0, min(1.0, value)), 4), "stage": stage}, **extra))


# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, dest, sha256=None, stage="downloading", lo=0.0, hi=1.0):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "VideoEditor-upscaler"})
    with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        last = 0.0
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            f.write(block)
            done += len(block)
            if total and time.time() - last > 0.5:
                last = time.time()
                progress(lo + (hi - lo) * done / total, stage, bytes=done, total=total)
    if sha256 and sha256_file(tmp) != sha256:
        os.remove(tmp)
        raise RuntimeError(f"Download of {os.path.basename(dest)} is corrupt (checksum mismatch)")
    os.replace(tmp, dest)


def code_manifest(root):
    lines = []
    for dirpath, _, files in os.walk(root):
        for name in files:
            if "__pycache__" in dirpath or name.endswith(".pyc"):
                continue
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            lines.append(f"{rel}:{sha256_file(p)}")
    return hashlib.sha256("\n".join(sorted(lines)).encode()).hexdigest()


def install_flashvsr_code():
    target = os.path.join(CODE_DIR, CODE_PACKAGE)
    if os.path.isfile(os.path.join(target, "nodes.py")) and os.path.isfile(os.path.join(CODE_DIR, "ok")):
        return
    progress(0.0, "downloading FlashVSR code")
    os.makedirs(CODE_DIR, exist_ok=True)
    zpath = os.path.join(CODE_DIR, "code.zip")
    download(FLASHVSR_CODE_URL, zpath, stage="downloading FlashVSR code", hi=0.02)
    staging = target + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    with zipfile.ZipFile(zpath) as z:
        prefix = z.namelist()[0].split("/")[0] + "/"
        for info in z.infolist():
            rel = info.filename[len(prefix):]
            if info.is_dir() or not rel:
                continue
            keep = rel in ("__init__.py", "nodes.py", "posi_prompt.pth", "LICENSE") or rel.startswith("src/")
            if not keep or ".." in rel.split("/"):
                continue
            out = os.path.join(staging, *rel.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
    os.remove(zpath)
    if code_manifest(staging) != FLASHVSR_CODE_MANIFEST:
        shutil.rmtree(staging, ignore_errors=True)
        raise RuntimeError("The downloaded FlashVSR code does not match the pinned version")
    shutil.rmtree(target, ignore_errors=True)
    os.replace(staging, target)
    with open(os.path.join(CODE_DIR, "ok"), "w") as f:
        f.write(FLASHVSR_CODE_COMMIT)


def install_flashvsr_weights():
    from huggingface_hub import hf_hub_download

    local = os.path.join(MODELS_DIR, FLASHVSR_DIR_NAME)
    os.makedirs(local, exist_ok=True)
    total = sum(FLASHVSR_FILES.values())
    stop = threading.Event()

    def watch():  # report the bytes on disk while huggingface_hub downloads
        while not stop.wait(1.0):
            have = 0
            for dirpath, _, files in os.walk(local):
                for n in files:
                    try:
                        have += os.path.getsize(os.path.join(dirpath, n))
                    except OSError:
                        pass
            progress(0.02 + 0.98 * min(1.0, have / total), "downloading FlashVSR weights", bytes=have, total=total)

    t = threading.Thread(target=watch, daemon=True)
    t.start()
    try:
        for name, size in FLASHVSR_FILES.items():
            path = os.path.join(local, name)
            if os.path.isfile(path) and os.path.getsize(path) == size:
                continue
            hf_hub_download(FLASHVSR_REPO, name, revision=FLASHVSR_REVISION, local_dir=local)
            if os.path.getsize(path) != size:
                raise RuntimeError(f"{name} has the wrong size after download")
    finally:
        stop.set()
        t.join()


def download_model(model):
    if model in ESRGAN:
        name, url, sha = ESRGAN[model]
        path = os.path.join(MODELS_DIR, "realesrgan", name)
        if not (os.path.isfile(path) and sha256_file(path) == sha):
            download(url, path, sha, stage="downloading " + name)
    else:
        install_flashvsr_code()
        install_flashvsr_weights()
        import_flashvsr()   # fails early if the environment cannot run it
    os.makedirs(MODELS_DIR, exist_ok=True)
    with open(os.path.join(MODELS_DIR, model + ".ok"), "w") as f:
        f.write("ok")


# ---------------------------------------------------------------------------
# Video I/O (ffmpeg from imageio-ffmpeg)
# ---------------------------------------------------------------------------
class Reader:
    def __init__(self, path, start, duration):
        import imageio_ffmpeg

        inp = ["-ss", f"{start:.6f}"] if start > 0 else []
        out = ["-t", f"{duration:.6f}"] if duration > 0 else []
        self.gen = imageio_ffmpeg.read_frames(path, pix_fmt="rgb24", input_params=inp, output_params=out)
        meta = next(self.gen)
        self.width, self.height = meta["size"]
        self.fps = float(meta.get("fps") or 30.0)
        length = duration if duration > 0 else max(0.0, float(meta.get("duration") or 0.0) - start)
        self.frames = max(1, int(round(length * self.fps)))
        self.done = False

    def read(self, n):
        import numpy as np

        out = []
        while len(out) < n and not self.done:
            try:
                buf = next(self.gen)
            except StopIteration:
                self.done = True
                break
            out.append(np.frombuffer(buf, dtype=np.uint8).reshape(self.height, self.width, 3))
        return out

    def close(self):
        try:
            self.gen.close()
        except Exception:
            pass


class Writer:
    def __init__(self, path, width, height, fps):
        import imageio_ffmpeg

        self.gen = imageio_ffmpeg.write_frames(
            path, (width, height), fps=fps, codec="libx264", pix_fmt_in="rgb24", pix_fmt_out="yuv420p",
            quality=None, macro_block_size=2, ffmpeg_log_level="error",
            output_params=["-crf", "16", "-preset", "medium"])
        self.gen.send(None)

    def write(self, frame):
        self.gen.send(frame.tobytes())

    def close(self):
        self.gen.close()


def mux_audio(video, source, start, duration, output):
    import imageio_ffmpeg

    cmd = [imageio_ffmpeg.get_ffmpeg_exe(), "-y", "-loglevel", "error", "-i", video]
    if start > 0:
        cmd += ["-ss", f"{start:.6f}"]
    if duration > 0:
        cmd += ["-t", f"{duration:.6f}"]
    cmd += ["-i", source, "-map", "0:v:0", "-map", "1:a:0?", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
            "-shortest", "-movflags", "+faststart", output]
    flags = 0x08000000 if os.name == "nt" else 0   # no console window
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=flags)
    if r.returncode != 0:   # keep the video without sound rather than failing
        shutil.move(video, output)
        return False
    os.remove(video)
    return True


def pick_device(name):
    import torch

    if name == "auto":
        if torch.cuda.is_available():
            return "cuda:0"
        if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
            return "mps"
        return "cpu"
    return name


# ---------------------------------------------------------------------------
# Real-ESRGAN (spandrel), frame by frame with tiles
# ---------------------------------------------------------------------------
def esrgan_infer(model, img, scale, tile, pad=16):
    import torch

    _, _, h, w = img.shape
    if tile <= 0 or (h <= tile and w <= tile):
        return model(img)
    out = torch.zeros((1, 3, h * scale, w * scale), dtype=img.dtype, device=img.device)
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            y0, x0 = max(0, y - pad), max(0, x - pad)
            y1, x1 = min(h, y + tile + pad), min(w, x + tile + pad)
            piece = model(img[:, :, y0:y1, x0:x1])
            ty, tx = (y - y0) * scale, (x - x0) * scale
            th, tw = (min(h, y + tile) - y) * scale, (min(w, x + tile) - x) * scale
            out[:, :, y * scale:y * scale + th, x * scale:x * scale + tw] = piece[:, :, ty:ty + th, tx:tx + tw]
    return out


def run_esrgan(args, reader, writer, device):
    import torch
    import torch.nn.functional as F
    from spandrel import ModelLoader

    progress(0.0, "loading model")
    path = os.path.join(MODELS_DIR, "realesrgan", ESRGAN[args.model][0])
    desc = ModelLoader().load_from_file(path)
    half = device.startswith("cuda") and desc.supports_half and args.precision != "fp32"
    model = desc.model.eval().to(device)
    if half:
        model = model.half()
    model_scale = desc.scale
    tile = args.tile_size if args.tile_size > 0 else (512 if device != "cpu" else 384)
    done = 0
    with torch.inference_mode():
        while True:
            frames = reader.read(1)
            if not frames:
                break
            t = torch.from_numpy(frames[0].copy()).to(device).permute(2, 0, 1)[None]
            t = t.half() / 255.0 if half else t.float() / 255.0
            out = esrgan_infer(model, t, model_scale, tile)
            if model_scale != args.scale:
                out = F.interpolate(out.float(), size=(reader.height * args.scale, reader.width * args.scale),
                                    mode="bicubic", antialias=True, align_corners=False)
            img = (out[0].float().clamp(0, 1) * 255.0).round().byte().permute(1, 2, 0).cpu().numpy()
            writer.write(img)
            done += 1
            progress(done / reader.frames, "upscaling", frame=done, frames=reader.frames)
    return done


# ---------------------------------------------------------------------------
# FlashVSR
# ---------------------------------------------------------------------------
_inner = {"cb": None}


def import_flashvsr():
    """Imports the FlashVSR implementation with stand-ins for the two ComfyUI modules it uses."""
    if CODE_DIR not in sys.path:
        sys.path.insert(0, CODE_DIR)
    if "folder_paths" not in sys.modules:
        fp = types.ModuleType("folder_paths")
        fp.models_dir = MODELS_DIR
        fp.get_filename_list = lambda kind: []

        def get_full_path_or_raise(kind, name):
            raise RuntimeError(f"{kind} '{name}' is not available")

        fp.get_full_path_or_raise = get_full_path_or_raise
        sys.modules["folder_paths"] = fp

        comfy = types.ModuleType("comfy")
        utils = types.ModuleType("comfy.utils")

        class ProgressBar:   # forwards the pipeline's own progress (tiles / blocks) to the editor
            def __init__(self, total):
                self.total = max(1, int(total))
                self.current = 0

            def update(self, n):
                self.current += n
                if _inner["cb"]:
                    _inner["cb"](min(1.0, self.current / self.total))

            def update_absolute(self, value, total=None, preview=None):
                if total:
                    self.total = max(1, int(total))
                self.current = value
                self.update(0)

        utils.ProgressBar = ProgressBar
        comfy.utils = utils
        sys.modules["comfy"] = comfy
        sys.modules["comfy.utils"] = utils
    import importlib

    nodes = importlib.import_module(CODE_PACKAGE + ".nodes")
    dit = importlib.import_module(CODE_PACKAGE + ".src.models.wan_video_dit")
    dit.USE_BLOCK_ATTN = False   # sparse sage attention (Triton): no compiled CUDA extension needed
    return nodes


def run_flashvsr(args, reader, writer, device):
    import numpy as np
    import torch
    import torch.nn.functional as F

    if not device.startswith("cuda"):
        raise RuntimeError("FlashVSR needs an NVIDIA GPU (CUDA). Choose a Real-ESRGAN model on this computer.")
    progress(0.0, "loading model")
    nodes = import_flashvsr()
    torch.cuda.set_device(device)
    dtype = {"bf16": torch.bfloat16, "fp16": torch.float16, "fp32": torch.float32}[args.precision]
    pipe = nodes.init_pipeline(FLASHVSR_DIR_NAME, FLASHVSR_MODES[args.model], device, dtype)

    s = args.scale
    # FlashVSR's output must be a multiple of 128 px: pad the input (edge pixels) and crop afterwards,
    # instead of cropping the picture.
    mult = 128 // s
    W, H = reader.width, reader.height
    PW, PH = math.ceil(W / mult) * mult, math.ceil(H / mult) * mult
    if args.tile_dit:
        PW, PH = max(PW, args.tile_size), max(PH, args.tile_size)
    # Frames per pass, bounded by memory: the upscaled input and the output canvas live in RAM.
    per_frame = PW * s * PH * s * 3 * 2 * 3
    chunk = args.chunk_frames or int(max(21, min(121, (3 << 30) // max(1, per_frame))))
    context = 8   # frames of the previous pass repeated so each pass starts "warmed up"

    done = 0
    prev_tail = []
    while True:
        new = reader.read(chunk)
        if not new:
            break
        batch = prev_tail + new
        t = torch.from_numpy(np.stack(batch)).float() / 255.0          # N H W C
        if PW != W or PH != H:
            t = F.pad(t.permute(0, 3, 1, 2), (0, PW - W, 0, PH - H), mode="replicate").permute(0, 2, 3, 1)
        base = done / reader.frames
        span = len(new) / reader.frames
        _inner["cb"] = lambda f, base=base, span=span: progress(base + span * f, "upscaling", frame=done,
                                                                 frames=reader.frames)
        out = nodes.flashvsr(pipe, t.contiguous(), s, True, args.tile_vae, args.tile_dit, args.tile_size, 24,
                             args.unload_dit, 2.0, 3.0, 11, 0, args.unload_dit)
        out = out[len(prev_tail):len(prev_tail) + len(new), :H * s, :W * s, :]
        for frame in out:
            writer.write((frame.float().clamp(0, 1) * 255.0).round().byte().numpy())
        done += len(new)
        progress(done / reader.frames, "upscaling", frame=done, frames=reader.frames)
        prev_tail = batch[-context:]
        del t, out
        nodes.clean_vram()
    _inner["cb"] = None
    return done


def upscale(args):
    t0 = time.time()
    import torch

    device = pick_device(args.device)
    if args.precision == "auto":
        # bfloat16 only where it is native (RTX 30 / Ampere and newer); RTX 20 emulates it slowly.
        bf16 = device.startswith("cuda") and torch.cuda.get_device_capability(device)[0] >= 8
        args.precision = "bf16" if bf16 else "fp16" if device != "cpu" else "fp32"
    reader = Reader(args.input, args.start, args.duration)
    ow, oh = reader.width * args.scale, reader.height * args.scale
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    silent = args.output + ".video.mp4"
    writer = Writer(silent, ow, oh, reader.fps)
    try:
        if args.model in ESRGAN:
            frames = run_esrgan(args, reader, writer, device)
        else:
            frames = run_flashvsr(args, reader, writer, device)
    finally:
        writer.close()
        reader.close()
    progress(1.0, "adding sound")
    audio = mux_audio(silent, args.input, args.start, args.duration, args.output)
    emit({"type": "done", "path": os.path.abspath(args.output), "width": ow, "height": oh, "frames": frames,
          "fps": reader.fps, "audio": audio, "device": device, "precision": args.precision,
          "seconds": round(time.time() - t0, 2)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--model", default="realesrgan-general", choices=MODELS)
    ap.add_argument("--scale", type=int, default=2, choices=[2, 4])
    ap.add_argument("--start", type=float, default=0.0, help="Seconds into INPUT")
    ap.add_argument("--duration", type=float, default=0.0, help="Seconds to upscale (0 = to the end)")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--precision", default="auto", choices=["auto", "bf16", "fp16", "fp32"])
    ap.add_argument("--tile-dit", action="store_true", help="FlashVSR: process in tiles (much less VRAM)")
    ap.add_argument("--tile-vae", action="store_true", help="FlashVSR: decode in tiles (less VRAM)")
    ap.add_argument("--unload-dit", action="store_true", help="FlashVSR: free the DiT before decoding (less VRAM)")
    ap.add_argument("--tile-size", type=int, default=256)
    ap.add_argument("--chunk-frames", type=int, default=0, help="Frames per FlashVSR pass (0 = from memory)")
    ap.add_argument("--download-only", action="store_true")
    ap.add_argument("--print-code-manifest", metavar="DIR")
    args = ap.parse_args()
    try:
        if args.print_code_manifest:
            print(code_manifest(args.print_code_manifest))
            return 0
        if args.download_only:
            download_model(args.model)
            emit({"type": "done", "model": args.model})
            return 0
        if not args.input or not args.output:
            ap.error("INPUT and OUTPUT are required")
        upscale(args)
        return 0
    except Exception as e:  # reported to the editor
        emit({"type": "error", "message": f"{type(e).__name__}: {e}"})
        return 1


if __name__ == "__main__":
    sys.exit(main())
