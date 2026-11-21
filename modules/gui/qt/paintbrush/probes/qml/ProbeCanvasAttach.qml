/*****************************************************************************
 * ProbeCanvasAttach.qml : NAI Paintbrush — canvas attach/geometry probe
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * The product pattern is:
 *
 *     PaintbrushCanvas { id: paintbrushCanvas; visible: false }   // workspace root
 *     ...
 *     PaintbrushCanvasView {
 *         Component.onCompleted: {
 *             canvas.parent = root
 *             canvas.anchors.fill = root
 *             canvas.visible = true
 *         }
 *     }
 *
 * Runtime evidence says the item stays 0x0 forever ("setCanvasItem: attached
 * 0x0", and every later presentation log still prints canvas=0x0), which
 * breaks frame placement and video layering.
 *
 * This probe runs three attach strategies side by side and reports the real
 * width/height after a layout pass, so the fix is chosen from evidence.
 *
 * Run:  qml.exe ProbeCanvasAttach.qml
 *****************************************************************************/

import QtQuick

Item {
    id: win
    width: 400
    height: 300

    property int failures: 0

    function expect(name, actual, wanted) {
        const ok = actual === wanted
        if (!ok)
            failures++
        console.log("RESULT|ASSERT." + name + "|" + (ok ? "PASS" : "FAIL")
                    + " (expected " + wanted + ", got " + actual + ")")
    }

    /* ---- A: control — declarative parent + declarative anchors ---------- */
    Item {
        id: hostA
        anchors.fill: parent
        clip: true

        Item {
            id: canvasA
            anchors.fill: parent
        }
    }

    /* ---- B: the product pattern — set from JS in Component.onCompleted -- */
    Item {
        id: hostB
        anchors.top: hostA.bottom
        width: parent.width
        height: 100
        clip: true

        Component.onCompleted: {
            canvasB.parent = hostB
            canvasB.anchors.fill = hostB
            canvasB.visible = true
        }
    }

    /* ---- C: reparent + explicit size binding from JS -------------------- */
    Item {
        id: hostC
        anchors.top: hostB.bottom
        width: parent.width
        height: 100
        clip: true

        Component.onCompleted: {
            canvasC.parent = hostC
            canvasC.width = Qt.binding(() => hostC.width)
            canvasC.height = Qt.binding(() => hostC.height)
            canvasC.visible = true
        }
    }

    /* the "canvas" items live under the window root, exactly like the
     * workspace-owned PaintbrushCanvas */
    Item { id: canvasB; visible: false }
    Item { id: canvasC; visible: false }

    Timer {
        interval: 120
        running: true
        onTriggered: {
            console.log("RESULT|A.declarative|" + canvasA.width + "x" + canvasA.height)
            console.log("RESULT|B.js-anchors-fill|" + canvasB.width + "x" + canvasB.height)
            console.log("RESULT|C.js-size-binding|" + canvasC.width + "x" + canvasC.height)

            expect("A.declarative.has-size", canvasA.width > 0 && canvasA.height > 0, true)
            expect("B.js-anchors-fill.has-size", canvasB.width > 0 && canvasB.height > 0, true)
            expect("C.js-size-binding.has-size", canvasC.width > 0 && canvasC.height > 0, true)

            console.log("RESULT|VERDICT|" + (failures === 0 ? "PASS" : "FAIL"))
            Qt.exit(failures === 0 ? 0 : 1)
        }
    }
}
