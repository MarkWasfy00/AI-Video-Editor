"""Subtitle translation with Meta's NLLB-200 (offline). Used by the Video Editor.

    python translate.py IN.json OUT.json --model facebook/nllb-200-distilled-600M --source ar --target en
    python translate.py --download-only --model facebook/nllb-200-distilled-600M

IN.json / OUT.json: {"texts": ["line 1", "line 2", ...]} (same count and order).
Language codes: en, ar (Modern Standard), ar-EG (Egyptian), es, fr, ... or "auto" for the source.
Models are cached in $HF_HOME. Progress on stdout, one JSON object per line:
    {"type": "progress", "value": 0.4, "stage": "translating"}
    {"type": "done", "count": 12, "device": "cuda", "seconds": 3.1}
    {"type": "error", "message": "..."}
"""
import argparse
import json
import sys
import time

NLLB = {
    "en": "eng_Latn", "ar": "arb_Arab", "ar-EG": "arz_Arab", "es": "spa_Latn", "fr": "fra_Latn",
    "de": "deu_Latn", "it": "ita_Latn", "pt": "por_Latn", "ru": "rus_Cyrl", "tr": "tur_Latn",
    "hi": "hin_Deva", "ur": "urd_Arab", "fa": "pes_Arab", "id": "ind_Latn", "nl": "nld_Latn",
    "pl": "pol_Latn", "uk": "ukr_Cyrl", "ja": "jpn_Jpan", "ko": "kor_Hang", "zh": "zho_Hans",
    "he": "heb_Hebr", "sv": "swe_Latn", "el": "ell_Grek", "vi": "vie_Latn", "th": "tha_Thai",
    "bn": "ben_Beng", "sw": "swh_Latn",
}


def emit(**kw):
    print(json.dumps(kw, ensure_ascii=False), flush=True)


def guess_source(texts):
    """A rough script-based guess when the transcript has no language."""
    sample = "".join(texts)[:2000]
    counts = {"ar": 0, "ru": 0, "zh": 0, "ja": 0, "ko": 0, "hi": 0, "he": 0, "el": 0, "th": 0, "en": 0}
    for ch in sample:
        o = ord(ch)
        if 0x0600 <= o <= 0x06FF:
            counts["ar"] += 1
        elif 0x0400 <= o <= 0x04FF:
            counts["ru"] += 1
        elif 0x4E00 <= o <= 0x9FFF:
            counts["zh"] += 1
        elif 0x3040 <= o <= 0x30FF:
            counts["ja"] += 1
        elif 0xAC00 <= o <= 0xD7AF:
            counts["ko"] += 1
        elif 0x0900 <= o <= 0x097F:
            counts["hi"] += 1
        elif 0x0590 <= o <= 0x05FF:
            counts["he"] += 1
        elif 0x0370 <= o <= 0x03FF:
            counts["el"] += 1
        elif 0x0E00 <= o <= 0x0E7F:
            counts["th"] += 1
        elif ch.isalpha():
            counts["en"] += 1
    if counts["ja"] and counts["zh"]:
        return "ja"
    return max(counts, key=counts.get)


def load(model_name, device):
    from transformers import AutoModelForSeq2SeqLM, AutoTokenizer

    tok = AutoTokenizer.from_pretrained(model_name)
    model = AutoModelForSeq2SeqLM.from_pretrained(model_name)
    if device == "cuda":
        model = model.half()
    return tok, model.to(device).eval()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--model", default="facebook/nllb-200-distilled-600M")
    ap.add_argument("--source", default="auto")
    ap.add_argument("--target", default="en")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--download-only", action="store_true")
    a = ap.parse_args()
    t0 = time.time()
    try:
        if a.download_only:
            from huggingface_hub import snapshot_download

            snapshot_download(a.model, allow_patterns=["*.json", "*.model", "*.bin", "*.safetensors", "*.txt"],
                              ignore_patterns=["*.msgpack", "*.h5", "*.ot"])
            emit(type="done", count=0, device="", seconds=round(time.time() - t0, 2))
            return 0

        import torch

        with open(a.inp, encoding="utf-8") as f:
            texts = json.load(f)["texts"]
        src = a.source if a.source in NLLB else guess_source(texts)
        if a.target not in NLLB:
            raise ValueError(f"unsupported target language '{a.target}'")
        device = a.device
        if device == "auto":
            device = "cuda" if torch.cuda.is_available() else "cpu"
        emit(type="progress", value=0.0, stage="loading model")
        tok, model = load(a.model, device)
        tok.src_lang = NLLB[src]
        target_id = tok.convert_tokens_to_ids(NLLB[a.target])

        out = [""] * len(texts)
        todo = [(i, t.strip()) for i, t in enumerate(texts) if t and t.strip()]
        same = NLLB[src] == NLLB[a.target]
        batch = 8 if device == "cuda" else 4
        for b in range(0, len(todo), batch):
            part = todo[b:b + batch]
            if same:
                for i, t in part:
                    out[i] = t
            else:
                enc = tok([t for _, t in part], return_tensors="pt", padding=True, truncation=True, max_length=400)
                enc = {k: v.to(device) for k, v in enc.items()}
                with torch.inference_mode():
                    gen = model.generate(**enc, forced_bos_token_id=target_id, num_beams=4,
                                         max_new_tokens=min(400, int(enc["input_ids"].shape[1] * 2.2) + 16))
                for (i, _), txt in zip(part, tok.batch_decode(gen, skip_special_tokens=True)):
                    out[i] = txt.strip()
            emit(type="progress", value=round(min(1.0, (b + len(part)) / max(1, len(todo))), 3), stage="translating")

        with open(a.out, "w", encoding="utf-8") as f:
            json.dump({"texts": out, "source": src, "target": a.target}, f, ensure_ascii=False)
        emit(type="done", count=len(out), device=device, source=src, seconds=round(time.time() - t0, 2))
        return 0
    except Exception as e:  # reported to the editor
        emit(type="error", message=f"{type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
