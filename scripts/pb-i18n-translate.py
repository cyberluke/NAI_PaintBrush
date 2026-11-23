#!/usr/bin/env python3
"""Translate the paintbrush gettext catalogs and compile the runtime trees.

The paintbrush QML is already part of po/POTFILES.in and qsTr() is routed
through VLC's gettext-backed QTranslator. This script fills the missing
paintbrush messages in the seven requested languages from the configured
OpenAI-compatible endpoint and appends them to po/<lang>.po. The --compile
step delegates to scripts/pb-i18n-compile.ps1, the single owner of the
runtime locale trees.

Usage:
  python scripts/pb-i18n-translate.py --languages cs sk vi fr de it es --apply --compile
  python scripts/pb-i18n-translate.py --languages cs --limit 6 --apply

The endpoint URL/model/key are read from the VLC config, never printed.
The script only appends missing msgids; existing translations are preserved.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
QML_DIR = os.path.join(ROOT, "modules", "gui", "qt", "paintbrush", "qml")
PO_DIR = os.path.join(ROOT, "po")
LANGUAGES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "pb-i18n-languages.json")
with open(LANGUAGES_FILE, encoding="utf-8") as _f:
    LANGUAGES = json.load(_f)


def read_config():
    cfg = os.path.join(os.environ.get("APPDATA", ""), "vlc", "vlcrc")
    values = {}
    if not os.path.exists(cfg):
        return values
    with open(cfg, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^(paintbrush-(?:assistant|image)-[a-z-]+)=(.*)$", line.strip())
            if m:
                values[m.group(1)] = m.group(2)
    return values


def decode_po_string(raw):
    try:
        return json.loads('"' + raw + '"')
    except Exception:
        return raw


def _unquote(line):
    s = line.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def parse_po(path):
    """Return {msgid: msgstr} for a .po/.pot file."""
    entries = {}
    src_raw = None
    dst_raw = None
    mode = None

    def finish():
        nonlocal src_raw, dst_raw
        if src_raw is None:
            return
        src = decode_po_string(src_raw)
        dst = decode_po_string(dst_raw or "")
        if src:
            entries[src] = dst
        src_raw = None
        dst_raw = None

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.rstrip("\n")
            if s.startswith("msgctxt "):
                continue
            if s.startswith("#~ msgid "):
                # obsolete entries still reserve the msgid; appending an
                # active copy would make msgfmt report a duplicate.
                obsolete = decode_po_string(_unquote(s[9:]))
                if obsolete:
                    entries.setdefault(obsolete, "")
                continue
            if s.startswith("msgid "):
                finish()
                src_raw = _unquote(s[6:])
                mode = "id"
                continue
            if s.startswith("msgid_plural "):
                mode = "plural"
                continue
            if s.startswith("msgstr"):
                dst_raw = _unquote(s[7:])
                mode = "str"
                continue
            if s.lstrip().startswith('"'):
                chunk = _unquote(s.strip())
                if mode == "id":
                    src_raw = (src_raw or "") + chunk
                elif mode == "str":
                    dst_raw = (dst_raw or "") + chunk
    finish()
    return entries


def extract_pot():
    xgettext = shutil.which("xgettext") or r"C:/msys64/usr/bin/xgettext.exe"
    files = sorted(glob.glob(os.path.join(QML_DIR, "*.qml")))
    pot = os.path.join(tempfile.gettempdir(), "pb-i18n.pot")
    cmd = [xgettext, "--language=C++", "--from-code=UTF-8",
           "--keyword=qsTr:1,1t", "--keyword=qsTr:1,2,3t",
           "-o", pot] + files
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    return parse_po(pot)


def endpoint_config():
    cfg = read_config()
    base = cfg.get("paintbrush-assistant-base-url",
                   "https://nai-ai-foundry-611086.services.ai.azure.com/openai/v1")
    model = cfg.get("paintbrush-assistant-model",
                    "deepseek-ai--deepseek-v41-flash-1017")
    key = cfg.get("paintbrush-assistant-api-key", "")
    return base.rstrip("/") + "/chat/completions", model, key


def translate_batch(items, language, url, model, key):
    system = (
        "You are a professional software localizer for NAI Paintbrush, a "
        "creative painting and collage application. Translate every UI string "
        "into " + language + ". Keep the tone friendly, concise and natural. "
        "Preserve placeholders such as %1 and %2, newlines, arrows, quotes, "
        "and product names (NAI Paintbrush, NAI OS, VLC, Neon, Buddy, Kids, "
        "Glitter Powder, SAM, CUDA, AI). Do not translate code identifiers, "
        "file names, URLs or keyboard keys (F1, Esc, Ctrl, Shift, Enter). "
        "Return ONLY a JSON object of the form "
        '{"items":[{"id":<id>,"text":"<translation>"}]}. '
        "Include every input id exactly once and no extra text."
    )
    body = {
        "model": model,
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": json.dumps(items, ensure_ascii=False)},
        ],
        "temperature": 0.15,
        "max_tokens": 8192,
        "stream": False,
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + key,
            "api-key": key,
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=240) as response:
        data = json.loads(response.read().decode("utf-8"))
    content = data["choices"][0]["message"].get("content") or ""
    content = content.strip()
    if content.startswith("```"):
        content = re.sub(r"^```[a-zA-Z]*\n?", "", content)
        content = re.sub(r"\n?```$", "", content)
    parsed = json.loads(content)
    result = {}
    for item in parsed.get("items", []):
        result[int(item["id"])] = str(item["text"])
    return result


def placeholders(text):
    return set(re.findall(r"%\d+", text))


def ensure_entries(po_path, translated, sources):
    """Append missing msgid/msgstr pairs at the end of the .po file."""
    existing = parse_po(po_path)
    appended = 0
    with open(po_path, "a", encoding="utf-8", newline="\n") as f:
        f.write("\n")
        for src in sources:
            if src in existing:
                continue
            dst = translated.get(src)
            if not dst:
                continue
            if placeholders(src) != placeholders(dst):
                continue
            f.write("#: modules/gui/qt/paintbrush (auto-translated)\n")
            f.write("msgid " + json.dumps(src, ensure_ascii=False) + "\n")
            f.write("msgstr " + json.dumps(dst, ensure_ascii=False) + "\n\n")
            appended += 1
    return appended


def compile_catalogs():
    """Compile every catalog into the runtime trees via the single build owner."""
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "pb-i18n-compile.ps1")
    shell = shutil.which("pwsh") or shutil.which("powershell")
    if not shell:
        raise RuntimeError("PowerShell not found; run scripts/pb-i18n-compile.ps1 manually")
    subprocess.run([shell, "-NoProfile", "-File", script], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--languages", nargs="+", default=list(LANGUAGES))
    ap.add_argument("--batch", type=int, default=40)
    ap.add_argument("--limit", type=int, default=0,
                    help="translate at most N missing strings per language (pilot)")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--compile", action="store_true")
    args = ap.parse_args()

    pot = extract_pot()
    print("paintbrush messages:", len(pot))
    url, model, key = endpoint_config()
    if not key:
        print("NO ASSISTANT API KEY IN CONFIG", file=sys.stderr)
        return 2

    for lang in args.languages:
        if lang not in LANGUAGES:
            continue
        po_path = os.path.join(PO_DIR, lang + ".po")
        if not os.path.exists(po_path):
            print("missing", po_path)
            continue
        existing = parse_po(po_path)
        missing = [s for s in pot if s and s not in existing]
        if args.limit:
            missing = missing[:args.limit]
        print(f"{lang}: missing {len(missing)}")
        translated = {}
        for i in range(0, len(missing), args.batch):
            batch = missing[i:i + args.batch]
            items = [{"id": j, "text": s} for j, s in enumerate(batch)]
            try:
                result = translate_batch(items, LANGUAGES[lang], url, model, key)
                for j, src in enumerate(batch):
                    dst = result.get(j)
                    if dst:
                        translated[src] = dst
                print(f"  {lang} {i + len(batch)}/{len(missing)}")
            except Exception as exc:
                print(f"  {lang} batch {i} failed: {exc}")
                # retry one-by-one for this batch
                for j, src in enumerate(batch):
                    try:
                        result = translate_batch(
                            [{"id": 0, "text": src}], LANGUAGES[lang],
                            url, model, key)
                        if 0 in result:
                            translated[src] = result[0]
                    except Exception:
                        pass
        if args.apply:
            count = ensure_entries(po_path, translated, missing)
            print(f"{lang}: appended {count}")
    if args.compile:
        print("compiling catalogs via pb-i18n-compile.ps1")
        compile_catalogs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
