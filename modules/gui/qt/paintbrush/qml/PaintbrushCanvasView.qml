/*****************************************************************************
 * PaintbrushCanvasView.qml : NAI Paintbrush — canvas + selection overlay
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The canvas area: checkered transparency background, the tile canvas item
     * and the contextual selection/object toolbar that appears when something
     * is selected. The canvas item itself is owned by the workspace root and
     * is reparented here so it can be shared with the tool rail and palette.
     *
     * When a video is loaded the video output (a native window layered behind
     * this transparent QML view) is anchored to this area by the VideoSurface
     * below, and the checkerboard is hidden so the playing video shows through
     * the blank canvas as the frame source. The chrome (tool rail, top bar,
     * palette, docks, media drawer) paints its own opaque background, so the
     * video stays confined to the canvas area. */
    Item {
        id: root

        required property PaintbrushCanvas canvas

        /* pick a "nice" ruler tick interval (in document pixels) so ticks are
         * not closer than ~40 view pixels at the current zoom */
        function rulerStep(zoom) {
            const targetPx = 40
            const steps = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000]
            for (let i = 0; i < steps.length; ++i) {
                if (steps[i] / Math.max(zoom, 0.0001) >= targetPx)
                    return steps[i]
            }
            return 10000
        }

        ColorContext {
            id: theme
            colorSet: ColorContext.View
        }

        Component.onCompleted: {
            canvas.parent = root
            canvas.anchors.fill = root
            canvas.visible = true
        }

        /* anchors the native video window to the canvas area */
        VideoSurface {
            id: videoSurface
            anchors.fill: parent
            /* Harvest mode: render the LIVE video through the transparent
             * canvas so Object Select / selection happens directly on the
             * moving picture — no "Use this frame" required to see it. */
            renderingEnabled: true
            videoSurfaceProvider: MainCtx.videoSurfaceProvider
            visible: PaintbrushCtx.playerHasVideo
        }

        /* checkerboard behind transparent content; hidden while a video is
         * loaded so the video picture serves as the canvas backdrop.
         * Cells are positioned manually (not via Grid/Repeater) so the number
         * of items is always consistent, avoiding QML "Grid contains more
         * visible items than rows*columns" warnings when the area is resized. */
        Repeater {
            id: checkerGrid

            visible: !PaintbrushCtx.playerHasVideo

            readonly property int cellSize: 16
            readonly property int cols: Math.max(1, Math.ceil(parent.width / cellSize))
            readonly property int rows: Math.max(1, Math.ceil(parent.height / cellSize))

            model: checkerGrid.cols * checkerGrid.rows
            delegate: Rectangle {
                width: checkerGrid.cellSize
                height: checkerGrid.cellSize
                x: (index % checkerGrid.cols) * checkerGrid.cellSize
                y: Math.floor(index / checkerGrid.cols) * checkerGrid.cellSize
                color: (Math.floor(index / checkerGrid.cols) + index % checkerGrid.cols) % 2 === 0
                       ? theme.bg.primary
                       : Qt.darker(theme.bg.primary, 1.06)
            }
        }

    /* contextual toolbar: selection / object actions */
    Rectangle {
        id: selectionBar

        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.margins: VLCStyle.margin_small

        height: selectionBarRow.height + VLCStyle.margin_small * 2
        width: selectionBarRow.width + VLCStyle.margin_normal * 2
        radius: VLCStyle.margin_small
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        visible: (canvas.hasSelection || PaintbrushCtx.objectSelected) && !PaintbrushCtx.welcomeVisible

        RowLayout {
            id: selectionBarRow
            anchors.centerIn: parent
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: canvas.hasSelection ? qsTr("Selection")
                                          : qsTr("Object %1").arg(PaintbrushCtx.objectMaskBoundsText)
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                visible: text.length > 0
            }

            Widgets.IconToolButton {
                description: qsTr("Cut out")
                text: VLCIcons.del
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.cutOutObject()
            }
            Widgets.IconToolButton {
                description: qsTr("Add to canvas")
                text: VLCIcons.add
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.addObjectToCanvas()
            }
            Widgets.IconToolButton {
                description: qsTr("Ask about this…")
                text: VLCIcons.info
                visible: PaintbrushCtx.objectSelected || canvas.hasSelection
                onClicked: {
                    if (PaintbrushCtx.objectSelected)
                        PaintbrushCtx.askAboutObject(qsTr("What is this object?"))
                    else
                        PaintbrushCtx.askAboutSelection(qsTr("What is shown in this selection?"))
                }
            }
            Widgets.IconToolButton {
                description: qsTr("Track in video")
                text: VLCIcons.play
                visible: PaintbrushCtx.objectSelected && PaintbrushCtx.playerHasVideo
                onClicked: PaintbrushCtx.trackObject(0, 24)
            }
            Widgets.IconToolButton {
                description: qsTr("Make sticker")
                text: VLCIcons.add
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.makeSticker()
            }
            Widgets.IconToolButton {
                description: qsTr("Remove object")
                text: VLCIcons.remove
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.removeObject("")
            }
            Widgets.IconToolButton {
                description: qsTr("Export object PNG")
                text: VLCIcons.ic_fluent_arrow_download_24_regular
                visible: PaintbrushCtx.objectSelected
                onClicked: exportObjectDialog.open()
            }

            Widgets.IconToolButton {
                description: qsTr("Copy")
                text: VLCIcons.ic_fluent_document_copy_24_regular
                visible: canvas.hasSelection
                onClicked: canvas.copySelection()
            }
            Widgets.IconToolButton {
                description: qsTr("Cut")
                text: VLCIcons.del
                visible: canvas.hasSelection
                onClicked: canvas.cutSelection()
            }
            Widgets.IconToolButton {
                description: qsTr("Clear selection")
                text: VLCIcons.clear
                visible: canvas.hasSelection
                onClicked: canvas.selectionClear()
            }
        }
    }

    /* pixel-coordinate rulers along the top and left edges. The canvas item
     * fills this view, so a document point (dx, dy) maps to the view at
     * (dx * zoom + panOffset.x, dy * zoom + panOffset.y). */
    Canvas {
        id: hRuler
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: VLCStyle.dp(18, VLCStyle.scale)
        z: 6
        visible: canvas.rulersVisible
        antialiasing: true

        readonly property int step: root.rulerStep(canvas.zoom)

        onStepChanged: requestPaint()
        Connections {
            target: canvas
            function onZoomChanged() { hRuler.requestPaint() }
            function onPanOffsetChanged() { hRuler.requestPaint() }
        }
        onWidthChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.fillStyle = Qt.rgba(0.08, 0.08, 0.09, 0.92)
            ctx.fillRect(0, 0, width, height)

            const stepPx = step * canvas.zoom
            // first tick at or before the left edge
            let first = Math.floor(-canvas.panOffset.x / stepPx) * step
            ctx.strokeStyle = "white"
            ctx.fillStyle = "white"
            ctx.font = (VLCStyle.fontSize_xsmall) + "px sans-serif"
            ctx.lineWidth = 1
            const labelEvery = stepPx >= 50 ? step : 100
            for (let dx = first; ; dx += step) {
                const vx = dx * canvas.zoom + canvas.panOffset.x
                if (vx > width) break
                if (vx < -10) continue
                if (dx % labelEvery === 0) {
                    ctx.beginPath()
                    ctx.moveTo(vx + 0.5, 12)
                    ctx.lineTo(vx + 0.5, height)
                    ctx.stroke()
                    ctx.fillText(String(dx), vx + 2, 8)
                } else if (stepPx >= 28) {
                    ctx.beginPath()
                    ctx.moveTo(vx + 0.5, 14)
                    ctx.lineTo(vx + 0.5, height)
                    ctx.stroke()
                }
            }
            // thin divider line at the bottom
            ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.5)
            ctx.beginPath()
            ctx.moveTo(0, height - 0.5)
            ctx.lineTo(width, height - 0.5)
            ctx.stroke()
        }
    }

    Canvas {
        id: vRuler
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: VLCStyle.dp(18, VLCStyle.scale)
        z: 6
        visible: canvas.rulersVisible
        antialiasing: true

        readonly property int step: root.rulerStep(canvas.zoom)

        onStepChanged: requestPaint()
        Connections {
            target: canvas
            function onZoomChanged() { vRuler.requestPaint() }
            function onPanOffsetChanged() { vRuler.requestPaint() }
        }
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.fillStyle = Qt.rgba(0.08, 0.08, 0.09, 0.92)
            ctx.fillRect(0, 0, width, height)

            const stepPx = step * canvas.zoom
            let first = Math.floor(-canvas.panOffset.y / stepPx) * step
            ctx.strokeStyle = "white"
            ctx.fillStyle = "white"
            ctx.font = (VLCStyle.fontSize_xsmall) + "px sans-serif"
            ctx.lineWidth = 1
            const labelEvery = stepPx >= 50 ? step : 100
            for (let dy = first; ; dy += step) {
                const vy = dy * canvas.zoom + canvas.panOffset.y
                if (vy > height) break
                if (vy < -10) continue
                if (dy % labelEvery === 0) {
                    ctx.beginPath()
                    ctx.moveTo(12, vy + 0.5)
                    ctx.lineTo(width, vy + 0.5)
                    ctx.stroke()
                    ctx.fillText(String(dy), 2, vy - 2)
                } else if (stepPx >= 28) {
                    ctx.beginPath()
                    ctx.moveTo(14, vy + 0.5)
                    ctx.lineTo(width, vy + 0.5)
                    ctx.stroke()
                }
            }
            ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.5)
            ctx.beginPath()
            ctx.moveTo(width - 0.5, 0)
            ctx.lineTo(width - 0.5, height)
            ctx.stroke()
        }
    }

    /* status overlay: coordinates / zoom */
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: VLCStyle.margin_small

        height: VLCStyle.dp(24, VLCStyle.scale)
        width: statusLabel.implicitWidth + VLCStyle.margin_small * 2
        radius: VLCStyle.margin_xsmall
        color: Qt.rgba(0, 0, 0, 0.55)

        Widgets.LabelExt {
            id: statusLabel
            anchors.centerIn: parent
            text: canvas.statusText
            color: "white"
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
    }

    FileDialog {
        id: exportObjectDialog
        title: qsTr("Export selected object")
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)", "All files (*)"]
        defaultSuffix: "png"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportObjectPng(selectedFile)
        }
    }
}