"""Speech / lyrics transcription with OpenAI Whisper. Used by the Video Editor.

    python transcribe.py INPUT OUT.json [--model turbo] [--language auto|en|...] [--device auto|cuda|cpu]
    python transcribe.py --download-only [--model turbo]

INPUT: a WAV/FLAC file (any rate; resampled to 16 kHz mono here).
OUT.json: {"language": "en", "model": "turbo", "segments": [{"start": s, "end": s, "text": "...",
          "words": [{"start": s, "end": s, "word": "..."}]}]}  (times in seconds of INPUT)
Progress on stdout, one JSON object per line:
    {"type": "progress", "value": 0.42, "stage": "..."}
    {"type": "done", "path": "...", "segments": 12, "language": "en", "device": "cuda", "seconds": 9.1}
    {"type": "error", "message": "..."}
"""
import argparse
import json
import os
import sys
import time

MODEL_DIR = os.path.join(os.environ.get("TORCH_HOME", os.path.dirname(os.path.abspath(__file__))), "..", "whisper")


# Whisper has one "ar" language; a short prompt in the dialect steers the vocabulary
# and spelling of its transcript (e.g. Egyptian "إزيك", "دلوقتي", "عشان" instead of
# Modern Standard Arabic forms).
DIALECT_PROMPTS = {
    # Common Egyptian markers (future "هـ", دلوقتي, عشان, إيه, أوي, ده / دي / دول, التانية ...).
    # The demonstratives stop Whisper writing "ده" as "ذهو" and "التاني" as MSA "الثاني".
    # On the Egyptian test clips: 31% word error rate with no prompt, 11% with this one.
    "ar-EG": "النهارده هنتكلم بالمصري. إزيك؟ عامل إيه؟ أنا كويس أوي. الموضوع ده مهم، والحاجة دي حلوة، "
             "والناس دول طيبين. دلوقتي هنروح، وبعدين هنشوف المرة التانية. "
             "عشان كده لازم نعمل حاجة حلوة قوي. مش عارف ليه، بس تمام.",
    "ar": "مرحبا، هذا نص باللغة العربية.",
}

# Words (normalized: no diacritics, ا for أإآ, ي for ى, ه for ة) that mark Egyptian Arabic in
# Whisper's plain "ar" output, and words that mark other dialects. Used to pick the Egyptian
# prompt when the language is "auto". Words shared with MSA (قوي, حاجة, جامد, حلو) are left out.
EGYPTIAN_WORDS = {
    "ده", "دي", "دا", "دول", "كده", "كدا", "ازاي", "ازيك", "ازايك", "ازيكو", "ايه", "مش", "مفيش", "مافيش",
    "عايز", "عايزه", "عايزين", "عاوز", "عاوزه", "عاوزين", "بتاع", "بتاعه", "بتاعت", "بتاعي", "بتاعك", "بتاعنا",
    "اوي", "دلوقتي", "دلوقت", "عشان", "علشان", "فين", "امتى", "ليه", "كويس", "كويسه", "كويسين", "النهارده",
    "انهارده", "امبارح", "لسه", "برضه", "برضو", "خالص", "اهو", "اهي", "مستني", "مستنيه", "التاني", "التانيه",
    "تاني", "تانيه", "بعدين", "زي", "اومال", "ماشي", "ازيكم", "بقولك", "بص", "شويه", "حاجات", "يابني",
}
EGYPTIAN_PREFIXES = ("دلوقت", "بعدين", "النهارده", "امبارح", "عشان", "علشان", "ازاي", "ازيك", "بتاع")
OTHER_DIALECT_WORDS = {
    # Levantine / Gulf / Maghrebi
    "هيك", "شو", "هلق", "هلا", "منيح", "منيحه", "بدي", "بدك", "بدنا", "هيدا", "هيدي", "هاد", "هاي", "ليش", "وين",
    "شلون", "وايد", "الحين", "هالحين", "ابغى", "ابغي", "ابي", "بزاف", "واش", "دابا", "كيفاش", "ديال", "مزيان",
    "برشا", "توا", "شنو", "اشنو", "علاش",
}
_DIACRITICS = None


def normalize_ar(word):
    import re

    global _DIACRITICS
    if _DIACRITICS is None:
        _DIACRITICS = re.compile(r"[ؐ-ًؚ-ٰٟـ]|[^\w]")
    w = _DIACRITICS.sub("", word)
    for a in "أإآٱ":
        w = w.replace(a, "ا")
    return w.replace("ى", "ي").replace("ة", "ه")


def egyptian_score(text):
    """(Egyptian marker count, other-dialect marker count, word count) for Arabic text."""
    words = [normalize_ar(w) for w in text.split()]
    words = [w for w in words if w]
    egy = other = 0
    prev = ""
    for w in words:
        forms = {w, w[1:]} if len(w) > 3 and w[0] in "وف" else {w}   # و / ف "and" glued on
        if forms & EGYPTIAN_WORDS or any(f.startswith(EGYPTIAN_PREFIXES) for f in forms):
            egy += 1
        elif forms & OTHER_DIALECT_WORDS:
            other += 1
        elif w.endswith("ش") and (prev == "ما" or (w.startswith("ما") and len(w) >= 5)):
            egy += 1                                       # negation ما ... ش (ما تنساش, ماعرفش)
        prev = w
    return egy, other, len(words)


def is_egyptian(text):
    egy, other, n = egyptian_score(text)
    return egy >= 2 and egy >= 2 * other and egy >= 0.04 * n


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def model_dir():
    d = os.environ.get("WHISPER_MODEL_DIR") or os.path.normpath(MODEL_DIR)
    os.makedirs(d, exist_ok=True)
    return d


def load_audio(path):
    import numpy as np
    import soundfile as sf

    audio, sr = sf.read(path, dtype="float32", always_2d=True)
    audio = audio.mean(axis=1)
    if sr != 16000:  # linear-phase resample with torchaudio (already installed)
        import torch
        import torchaudio.functional as F

        audio = F.resample(torch.from_numpy(audio), sr, 16000).numpy()
    return np.ascontiguousarray(audio, dtype=np.float32)


class ProgressBar:
    """Stands in for tqdm inside whisper.transcribe and reports progress as JSON."""

    def __init__(self, total=None, **_):
        self.total = max(1, total or 1)
        self.n = 0

    def update(self, n=1):
        self.n += n
        emit({"type": "progress", "value": round(min(1.0, self.n / self.total), 4), "stage": "transcribing"})

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def close(self):
        pass


def detect_language(model, audio, device):
    """Whisper's language guess on the first 30 s; for Arabic, also the dialect ("ar-EG" or "ar")
    from a quick prompt-free pass over the first minute."""
    import whisper

    emit({"type": "progress", "value": 0.0, "stage": "detecting language"})
    mel = whisper.log_mel_spectrogram(whisper.pad_or_trim(audio), model.dims.n_mels).to(device)
    _, probs = model.detect_language(mel)
    lang = max(probs, key=probs.get)
    if lang != "ar":
        return lang
    probe = model.transcribe(audio[: 16000 * 60], language="ar", temperature=0.0, condition_on_previous_text=False,
                             fp16=(device == "cuda"), verbose=None)
    text = " ".join(s.get("text", "") for s in probe.get("segments", []))
    return "ar-EG" if is_egyptian(text) else "ar"


def transcribe(args):
    import importlib

    import torch
    import whisper

    wt = importlib.import_module("whisper.transcribe")  # the module (whisper.transcribe is also a function)

    t0 = time.time()
    device = args.device
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"
    emit({"type": "progress", "value": 0.0, "stage": "loading model"})
    model = whisper.load_model(args.model, device=device, download_root=model_dir())
    audio = load_audio(args.input)
    requested = args.language
    if requested == "auto":
        requested = detect_language(model, audio, device)
    wt.tqdm.tqdm = ProgressBar  # progress per 30 s window
    language = requested
    prompt = DIALECT_PROMPTS.get(requested, "")
    if args.prompt.strip():                     # user hints (names, slang) come after the dialect prompt
        prompt = (prompt + " " + args.prompt.strip()).strip()
    prompt = prompt or None
    if language and "-" in language:          # ar-EG -> Whisper's "ar" + the dialect prompt
        language = language.split("-")[0]
    result = model.transcribe(
        audio,
        language=language,
        initial_prompt=prompt,
        # Keep the dialect prompt on every 30 s window, not just the first (with
        # condition_on_previous_text off Whisper otherwise drops it after 30 s).
        carry_initial_prompt=prompt is not None,
        word_timestamps=True,
        condition_on_previous_text=False,  # fewer repetition loops on songs
        fp16=(device == "cuda"),
        verbose=False,
    )
    segments = []
    for s in result.get("segments", []):
        text = s.get("text", "").strip()
        if not text:
            continue
        segments.append({
            "start": round(float(s["start"]), 3),
            "end": round(float(s["end"]), 3),
            "text": text,
            "words": [{"start": round(float(w["start"]), 3), "end": round(float(w["end"]), 3),
                       "word": w["word"].strip()} for w in s.get("words", []) if w.get("word", "").strip()],
        })
    lang_out = requested if "-" in requested else result.get("language", "") or requested
    out = {"language": lang_out, "model": args.model, "segments": segments}
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False)
    emit({"type": "done", "path": args.output, "segments": len(segments), "language": out["language"],
          "device": device, "seconds": round(time.time() - t0, 2)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--model", default="turbo")
    ap.add_argument("--language", default="auto")
    ap.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    ap.add_argument("--prompt", default="", help="Words / spelling hints (names, slang) for Whisper")
    ap.add_argument("--download-only", action="store_true")
    args = ap.parse_args()
    try:
        if args.download_only:
            import whisper

            whisper.load_model(args.model, device="cpu", download_root=model_dir())
            emit({"type": "done", "model": args.model})
            return 0
        if not args.input or not args.output:
            ap.error("INPUT and OUT.json are required")
        transcribe(args)
        return 0
    except Exception as e:
        emit({"type": "error", "message": f"{type(e).__name__}: {e}"})
        return 1


if __name__ == "__main__":
    sys.exit(main())
