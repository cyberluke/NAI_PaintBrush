#!/usr/bin/env python3
"""Activate matching obsolete gettext translations for the paintbrush UI.

VLC's catalogs contain many obsolete (`#~`) entries for common words that the
paintbrush UI now reuses ("Brush", "Send", "You"...). gettext ignores `#~`
entries, so those strings stayed English. This script:

- extracts the current paintbrush qsTr messages,
- finds obsolete entries with the same msgid and a non-empty translation,
- removes the obsolete block and appends an ACTIVE entry,
- adds the few labels that only exist in C++ (Layer %1) or have no legacy
  entry (Boost),
- leaves every existing active translation untouched.

It does not call any network service.
"""

import importlib.util
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PO_DIR = os.path.join(ROOT, "po")

_translate_path = os.path.join(os.path.dirname(__file__), "pb-i18n-translate.py")
_spec = importlib.util.spec_from_file_location("pb_i18n_translate", _translate_path)
_tr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_tr)
LANGS = list(_tr.LANGUAGES)

MANUAL = {
    "Boost": {
        "cs": "Vylepšit", "sk": "Vylepšiť", "vi": "Tăng cường",
        "fr": "Renforcer", "de": "Verstärken", "it": "Potenziare",
        "es": "Mejorar",
    },
    "Layer %1": {
        "cs": "Vrstva %1", "sk": "Vrstva %1", "vi": "Lớp %1",
        "fr": "Calque %1", "de": "Ebene %1", "it": "Livello %1",
        "es": "Capa %1",
    },
}


def _content(line):
    s = line.strip()
    if s.startswith('"') and s.endswith('"'):
        return s[1:-1]
    return s


def parse_active_and_obsolete(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines(keepends=True)
    active = set()
    obsolete_srcs = set()
    obsolete_blocks = []  # (start, end, src, dst)

    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("msgid "):
            # active entry; collect continuation lines
            chunks = [_content(line[6:])]
            j = i + 1
            while j < len(lines) and lines[j].lstrip().startswith('"'):
                chunks.append(_content(lines[j]))
                j += 1
            src = _tr.decode_po_string("".join(chunks))
            if src:
                active.add(src)
            i = j
            continue
        if line.startswith("#~ msgid "):
            start = i
            j = i
            mode = None
            id_chunks = []
            str_chunks = []
            while j < len(lines):
                t = lines[j]
                if t.startswith("#~ "):
                    body = t[3:].strip()
                    if body.startswith("msgid "):
                        mode = "id"
                        id_chunks = [_content(body[6:])]
                    elif body.startswith("msgstr "):
                        mode = "str"
                        str_chunks = [_content(body[7:])]
                    elif body.startswith('"'):
                        if mode == "id":
                            id_chunks.append(_content(body))
                        elif mode == "str":
                            str_chunks.append(_content(body))
                    j += 1
                    continue
                if t.strip() == "":
                    break
                break
            src = _tr.decode_po_string("".join(id_chunks))
            dst = _tr.decode_po_string("".join(str_chunks))
            if src:
                obsolete_srcs.add(src)
                obsolete_blocks.append((start, j, src, dst))
            i = j
            continue
        i += 1
    return lines, active, obsolete_srcs, obsolete_blocks


def main():
    pot = _tr.extract_pot()
    print("paintbrush messages:", len(pot))
    total = 0
    for lang in LANGS:
        po_path = os.path.join(PO_DIR, lang + ".po")
        if not os.path.exists(po_path):
            continue
        lines, active, obsolete_srcs, blocks = parse_active_and_obsolete(po_path)
        remove = set()
        additions = []

        for start, end, src, dst in blocks:
            if start in remove:
                continue
            if src in active or src not in pot:
                continue
            if dst and dst != src:
                additions.append((src, dst))
                active.add(src)
                remove.add(start)

        for src, translations in MANUAL.items():
            if src in active:
                continue
            dst = translations.get(lang)
            if dst:
                additions.append((src, dst))
                active.add(src)

        if not additions and not remove:
            print(f"{lang}: nothing to activate")
            continue

        out = []
        i = 0
        while i < len(lines):
            if i in remove:
                # skip until the blank line after the block
                i += 1
                while i < len(lines) and lines[i].strip() != "":
                    i += 1
                if i < len(lines):
                    i += 1
                continue
            out.append(lines[i])
            i += 1

        if out and not out[-1].endswith("\n"):
            out[-1] += "\n"
        out.append("\n")
        for src, dst in additions:
            out.append("#: modules/gui/qt/paintbrush (activated legacy translation)\n")
            out.append("msgid " + json.dumps(src, ensure_ascii=False) + "\n")
            out.append("msgstr " + json.dumps(dst, ensure_ascii=False) + "\n\n")

        open(po_path, "w", encoding="utf-8", newline="\n").write("".join(out))
        total += len(additions)
        print(f"{lang}: activated {len(additions)}")

    print("total:", total)
    try:
        _tr.compile_catalogs()
    except Exception as exc:
        print("compile failed:", exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
