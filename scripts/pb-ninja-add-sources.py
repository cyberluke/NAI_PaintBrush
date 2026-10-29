#!/usr/bin/env python3
"""Add new NAI Paintbrush sources to an existing win64-meson/build.ninja.

`meson setup --reconfigure` is broken on this tree (see HANDOFF.md), so the
ninja file is patched directly by cloning the edges meson generated for a
template source (pb_export.cpp / pb_export.hpp). The edit is idempotent: files
already present are skipped. meson.build stays the authoritative source list
(it is edited separately) so a future working regen produces the same graph.

usage: python scripts/pb-ninja-add-sources.py [--check]
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NINJA = os.path.join(ROOT, "win64-meson", "build.ninja")

# (relative path under modules/gui/qt/, needs moc)
SOURCES = [
    # listed in meson.build before this work but missing from the stale graph
    ("paintbrush/pb_capability.cpp", None),
    ("paintbrush/pb_sticker.cpp", None),
    ("paintbrush/pb_layer_model.cpp", "paintbrush/pb_layer_model.hpp"),
    # Paintbrush 2.0
    ("paintbrush/pb_harvest.cpp", "paintbrush/pb_harvest.hpp"),
    ("paintbrush/pb_boost.cpp", None),
    ("paintbrush/pb_controller_art.cpp", None),
    ("paintbrush/art/runtime/material_registry.cpp", None),
    ("paintbrush/art/surface/paper_stock.cpp", None),
    ("paintbrush/art/surface/paper_renderer.cpp", None),
    ("paintbrush/art/stroke/bristle_brush.cpp", None),
    ("paintbrush/art/materials/glitter/glitter_material.cpp", None),
    ("paintbrush/art/finish/effects.cpp", None),
    ("paintbrush/art/style/mood.cpp", None),
    ("paintbrush/art/style/typography_style.cpp", None),
    ("paintbrush/art/style/badges.cpp", None),
    # Paintbrush 2.0 Showroom: GPU Art Runtime (QRhi)
    ("paintbrush/art/gpu/pb_gpu_runtime.cpp", None),
    # Paintbrush Motion Toys: 2D puppet runtime (mask -> mesh -> ARAP)
    ("paintbrush/motion/pb_puppet.cpp", None),
    # Paintbrush Living Harvest: rig once + auto motion window + asset store
    ("paintbrush/motion/pb_living.cpp", None),
]
# QML files of the VLC.Paintbrush module to add to its qrc/qmldir generation
QML = [
    "paintbrush/qml/PaintbrushHarvestTray.qml",
    "paintbrush/qml/PaintbrushBoostBar.qml",
    "paintbrush/qml/PaintbrushMoodPicker.qml",
    "paintbrush/qml/PaintbrushTextEditorBar.qml",
]

TEMPLATE_SRC = "paintbrush/pb_export.cpp"
TEMPLATE_HDR = "paintbrush/pb_export.hpp"


def obj_name(rel):
    # meson: modules/libqt_plugin.dll.p/gui_qt_<path with / -> _>.obj
    return "modules/libqt_plugin.dll.p/gui_qt_" + rel.replace("/", "_") + ".obj"


def moc_cpp(rel_hdr):
    base = os.path.splitext(os.path.basename(rel_hdr))[0]
    return "modules/libqt_plugin.dll.p/moc_%s.cpp" % base


def moc_obj(rel_hdr):
    base = os.path.splitext(os.path.basename(rel_hdr))[0]
    return "modules/libqt_plugin.dll.p/meson-generated_moc_%s.cpp.obj" % base


def find_block(lines, head_prefix):
    """Return (start, end) of the build block whose first line starts with
    head_prefix; `end` is exclusive (first line of the next block)."""
    for i, line in enumerate(lines):
        if line.startswith(head_prefix):
            j = i + 1
            while j < len(lines) and lines[j].startswith(" "):
                j += 1
            return i, j
    raise SystemExit("template block not found: " + head_prefix)


def main():
    check = "--check" in sys.argv
    with open(NINJA, "r", encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")

    tmpl_obj = obj_name(TEMPLATE_SRC)
    tmpl_moc_cpp = moc_cpp(TEMPLATE_HDR)
    tmpl_moc_obj = moc_obj(TEMPLATE_HDR)

    added = []
    skipped = []

    def clone(block_lines, old, new):
        out = []
        for l in block_lines:
            out.append(l.replace(old, new))
        return out

    # 1) compile edges for sources
    for rel, hdr in SOURCES:
        o = obj_name(rel)
        if any(l.startswith("build " + o + ":") for l in lines):
            skipped.append(rel)
        else:
            s, e = find_block(lines, "build " + tmpl_obj + ":")
            block = lines[s:e]
            new = clone(block, "../modules/gui/qt/" + TEMPLATE_SRC, "../modules/gui/qt/" + rel)
            new = clone(new, tmpl_obj, o)
            new = clone(new, tmpl_obj.replace("/", "\\"), o.replace("/", "\\"))
            lines[e:e] = new
            added.append(rel)
        if hdr:
            mc = moc_cpp(hdr)
            mo = moc_obj(hdr)
            if not any(l.startswith("build " + mc + ":") for l in lines):
                s, e = find_block(lines, "build " + tmpl_moc_cpp + ":")
                block = lines[s:e]
                new = clone(block, "../modules/gui/qt/" + TEMPLATE_HDR, "../modules/gui/qt/" + hdr)
                new = clone(new, tmpl_moc_cpp, mc)
                new = clone(new, tmpl_moc_cpp.replace("/", "\\"), mc.replace("/", "\\"))
                lines[e:e] = new
                added.append(hdr + " (moc)")
            if not any(l.startswith("build " + mo + ":") for l in lines):
                s, e = find_block(lines, "build " + tmpl_moc_obj + ":")
                block = lines[s:e]
                new = clone(block, tmpl_moc_cpp, mc)
                new = clone(new, tmpl_moc_obj, mo)
                new = clone(new, tmpl_moc_obj.replace("/", "\\"), mo.replace("/", "\\"))
                lines[e:e] = new
                added.append(hdr + " (moc obj)")

    # 2) link inputs: add objects to the libqt_plugin.dll link edge
    for i, line in enumerate(lines):
        if line.startswith("build modules/libqt_plugin.dll: "):
            objs = []
            for rel, hdr in SOURCES:
                objs.append(obj_name(rel))
                if hdr:
                    objs.append(moc_obj(hdr))
            missing = [o for o in objs if (" " + o + " ") not in line + " " and not line.endswith(" " + o)]
            if missing:
                # insert right after the template object
                anchor = " " + tmpl_obj
                idx = line.find(anchor)
                if idx < 0:
                    raise SystemExit("link anchor not found")
                idx += len(anchor)
                line = line[:idx] + " " + " ".join(missing) + line[idx:]
                lines[i] = line
                added.append("link: %d object(s)" % len(missing))
            break

    # 3) QML: the rcc edge for paintbrush_assets.cpp lists every QML file as an
    #    implicit dependency (absolute D$:/ paths). Add the new files there and
    #    to the generated res.qrc / qmldir (plain text inputs of that edge).
    qml_anchor = "D$:/_SATIN_AI/NAI-OS/vlc/modules/gui/qt/paintbrush/qml/PaintbrushAssistantDock.qml"
    for i, line in enumerate(lines):
        if line.startswith("build modules/gui/qt/paintbrush_assets.cpp:") and qml_anchor in line:
            missing = ["D$:/_SATIN_AI/NAI-OS/vlc/modules/gui/qt/" + q for q in QML
                       if ("D$:/_SATIN_AI/NAI-OS/vlc/modules/gui/qt/" + q) not in line]
            if missing:
                idx = line.find(qml_anchor) + len(qml_anchor)
                lines[i] = line[:idx] + " " + " ".join(missing) + line[idx:]
                added.append("qml rcc deps @%d" % (i + 1))
            break
    qrc = os.path.join(ROOT, "win64-meson", "modules", "gui", "qt", "VLC", "Paintbrush", "res.qrc")
    qmldir = os.path.join(ROOT, "win64-meson", "modules", "gui", "qt", "VLC", "Paintbrush", "qmldir")
    if not check and os.path.exists(qrc) and os.path.exists(qmldir):
        with open(qrc, "r", encoding="utf-8") as f:
            qrc_text = f.read()
        with open(qmldir, "r", encoding="utf-8") as f:
            qmldir_text = f.read()
        for q in QML:
            name = os.path.basename(q)
            if name not in qrc_text:
                entry = '    <file alias="%s">%s</file>\n' % (
                    name, os.path.join(ROOT, "modules", "gui", "qt", q).replace("/", "\\"))
                qrc_text = qrc_text.replace("  </qresource>", entry + "  </qresource>")
                added.append("res.qrc: " + name)
            typ = os.path.splitext(name)[0]
            if (typ + " 1.0") not in qmldir_text:
                if not qmldir_text.endswith("\n"):
                    qmldir_text += "\n"
                qmldir_text += "%s 1.0 %s\n" % (typ, name)
                added.append("qmldir: " + typ)
        with open(qrc, "w", encoding="utf-8", newline="\n") as f:
            f.write(qrc_text)
        with open(qmldir, "w", encoding="utf-8", newline="\n") as f:
            f.write(qmldir_text)

    if check:
        print("would add:", *added, sep="\n  ")
        print("skipped:", *skipped, sep="\n  ")
        return 0
    if added:
        shutil.copyfile(NINJA, NINJA + ".pb2bak")
        with open(NINJA, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
    print("added:", *added, sep="\n  ")
    print("skipped (already present):", *skipped, sep="\n  ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
