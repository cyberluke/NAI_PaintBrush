/*****************************************************************************
 * ProbeModelBinding.qml : NAI Paintbrush — isolated QML model-binding probe
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * Tracer bullet for the component harness. It answers one question with
 * evidence instead of guesswork:
 *
 *   When a ListView's `model` is a QVariantList of QVariantMap (i.e. a plain
 *   JS array of objects), which accessor resolves inside the delegate —
 *   `model.<role>`, `modelData.<role>`, or neither?
 *
 * PaintbrushMediaDrawer's thumbnail delegate uses `model.timeMs` /
 * `model.hasImage`, while the (working) track lane and layer list use
 * `modelData.*` / `required property`. If `model.*` does not resolve, the
 * thumbnail Image `source` is always "" and the timeline renders empty boxes
 * — exactly the reported bug.
 *
 * Run:
 *   C:\msys64\mingw64\bin\qml.exe ProbeModelBinding.qml
 * Exit code 0 = probe ran; the RESULT lines carry the verdict.
 *****************************************************************************/

import QtQuick

Item {
    id: root
    width: 200
    height: 60

    /* Mirrors PaintbrushController::thumbnailList(): a QVariantList whose
     * elements are QVariantMap with timeMs / hasImage / failed. */
    property var thumbList: [
        { timeMs: 0,    hasImage: true,  failed: false },
        { timeMs: 1000, hasImage: false, failed: true  }
    ]

    property int probesDone: 0
    property int failures: 0

    function report(name, value) {
        console.log("RESULT|" + name + "|" + value)
    }

    /* A probe is an ASSERTION, not a log dump: it fails the run when the
     * delegate contract the product QML relies on is not what we think. */
    function expect(name, actual, wanted) {
        const ok = actual === wanted
        if (!ok)
            failures++
        report("ASSERT." + name, (ok ? "PASS" : "FAIL") + " (expected " + wanted + ", got " + actual + ")")
    }

    ListView {
        anchors.fill: parent
        model: root.thumbList
        delegate: Item {
            width: 20
            height: 20

            Component.onCompleted: {
                const m = (typeof model === "undefined" || model === null)
                          ? "undefined" : typeof model.timeMs
                const md = (typeof modelData === "undefined" || modelData === null)
                           ? "undefined" : typeof modelData.timeMs
                const mdImg = (typeof modelData === "undefined" || modelData === null)
                              ? "undefined" : typeof modelData.hasImage

                root.report("delegate[" + index + "].model.timeMs", m)
                root.report("delegate[" + index + "].modelData.timeMs", md)
                root.report("delegate[" + index + "].modelData.hasImage", mdImg)

                /* The contract: a QVariantList-of-QVariantMap model exposes its
                 * roles through modelData.* and NOT through model.*. */
                root.expect("delegate[" + index + "].model.roles.absent", m, "undefined")
                root.expect("delegate[" + index + "].modelData.timeMs", md, "number")
                root.expect("delegate[" + index + "].modelData.hasImage", mdImg, "boolean")

                root.probesDone++
                if (root.probesDone >= root.thumbList.length)
                    finishTimer.start()
            }
        }
    }

    Timer {
        id: finishTimer
        interval: 50
        onTriggered: {
            root.report("VERDICT", root.failures === 0 ? "PASS" : "FAIL")
            console.log("RESULT|PROBE_COMPLETE|ProbeModelBinding")
            Qt.exit(root.failures === 0 ? 0 : 1)
        }
    }
}
