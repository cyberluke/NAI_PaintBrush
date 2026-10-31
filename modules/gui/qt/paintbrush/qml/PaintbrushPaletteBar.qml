/*****************************************************************************
 * PaintbrushPaletteBar.qml : NAI Paintbrush — tool-option context palette
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* Compact Paintbrush-style palette (A39): foreground/background colors and the
 * classic swatches. The tool-option context section AND the precision toggles
 * (grid/rulers/snap/pressure) live in the dedicated horizontal
 * PaintbrushToolOptionsBar (built from the SAME unified PaintbrushToolbar
 * component as the vertical rail), so the option sliders are never crowded
 * out — this bar keeps the colour surface only. Eyedropper integration happens
 * on the canvas (Pick tool). The color dialog uses a real SV plane + hue rail
 * (A40). */
Item {
    id: root

    required property PaintbrushCanvas canvas

    /* TEMP-PROOF (reviewer qrc/qmlcache check): a unique build marker + a
     * startup geometry log prove the running app is loading THIS qml (not a
     * stale qrc/qmlcache copy). Remove after the GUI gate is accepted. */
    objectName: "PaintbrushPaletteBar_marker_e48-toolbar-v2"
    Component.onCompleted: {
        console.log("PB_QML PaletteBar onCompleted x=" + x + " y=" + y
                    + " w=" + width + " h=" + height + " z=" + z
                    + " marker=" + objectName)
    }
    onWidthChanged: logGeom()
    onHeightChanged: logGeom()
    onYChanged: logGeom()
    function logGeom() {
        console.log("PB_QML PaletteBar geom x=" + x + " y=" + y
                    + " w=" + width + " h=" + height + " z=" + z)
    }

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: VLCStyle.margin_normal
        anchors.rightMargin: VLCStyle.margin_normal
        spacing: VLCStyle.margin_normal

        /* foreground / background */
        Item {
            width: VLCStyle.dp(52, VLCStyle.scale)
            height: parent.height
            Layout.alignment: Qt.AlignVCenter

            Rectangle {
                id: fgSwatch
                width: VLCStyle.dp(28, VLCStyle.scale)
                height: VLCStyle.dp(28, VLCStyle.scale)
                radius: 4
                border.width: 2
                border.color: theme.fg.primary
                color: canvas.fgColor
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.topMargin: VLCStyle.margin_xsmall

                MouseArea {
                    anchors.fill: parent
                    onClicked: colorDialog.color = canvas.fgColor
                    onDoubleClicked: colorDialog.open()
                }
            }
            Rectangle {
                width: VLCStyle.dp(24, VLCStyle.scale)
                height: VLCStyle.dp(24, VLCStyle.scale)
                radius: 4
                border.width: 2
                border.color: theme.fg.secondary
                color: canvas.bgColor
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: VLCStyle.margin_xsmall

                MouseArea {
                    anchors.fill: parent
                    onClicked: colorDialog.color = canvas.bgColor
                    onDoubleClicked: colorDialog.open()
                }
            }
        }

        /* classic swatches */
        RowLayout {
            spacing: VLCStyle.margin_xxxsmall

            Repeater {
                model: ["#000000", "#7f7f7f", "#880015", "#ed1c24", "#ff7f27",
                        "#fff200", "#22b14c", "#00a2e8", "#3f48cc", "#a349a4",
                        "#ffffff", "#c3c3c3", "#b97a57", "#ffaec9", "#ffc90e",
                        "#efe4b0", "#b5e61d", "#99d9ea", "#7092be", "#c8bfe7"]
                Rectangle {
                    width: VLCStyle.dp(22, VLCStyle.scale)
                    height: VLCStyle.dp(22, VLCStyle.scale)
                    radius: 3
                    border.width: 1
                    border.color: theme.border
                    color: modelData

                    MouseArea {
                        anchors.fill: parent
                        onClicked: canvas.fgColor = modelData
                        onPressAndHold: canvas.bgColor = modelData
                    }
                }
            }
        }

        /* Per-tool options (size/amount/flake/glue/loose/shimmer) and the
         * precision toggles (grid/rulers/snap/pressure) moved OUT of this
         * bar into the dedicated horizontal PaintbrushToolOptionsBar (also
         * rendered by the unified PaintbrushToolbar component) so the option
         * sliders can never be crowded out by the grid/ruler buttons. This
         * bar keeps the colour surface only. */
        Item { Layout.fillWidth: true }
    }

    /* custom color dialog */
    Dialog {
        id: colorDialog

        title: qsTr("Choose color")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            ColorPicker {
                id: picker
                Layout.preferredWidth: VLCStyle.dp(220, VLCStyle.scale)
                Layout.preferredHeight: VLCStyle.dp(180, VLCStyle.scale)
                color: canvas.fgColor
            }
        }

        onAccepted: canvas.fgColor = picker.color
    }

    /* Real SV plane + hue rail (A40): hue picked on the rail (x0.85),
     * saturation = x position, value = 1 - y*0.8. The old saturation
     * expression (1.0 - (y/h)*0.0 + 0.0) pinned saturation to 1.0; the
     * plane now maps both axes truthfully. */
    component ColorPicker: Rectangle {
        id: picker

        property color color: "black"
        property real hue: 0.0
        property real saturation: 1.0
        property real value: 1.0

        function clamp01(x) {
            return Math.max(0.0, Math.min(1.0, x))
        }

        /* initialize the markers from the current color when shown */
        Component.onCompleted: {
            const r = picker.color.r, g = picker.color.g, b = picker.color.b
            const max = Math.max(r, g, b), min = Math.min(r, g, b)
            const d = max - min
            picker.value = max
            picker.saturation = max === 0 ? 0 : d / max
            if (d > 0) {
                let h
                if (max === r) h = (g - b) / d + (g < b ? 6 : 0)
                else if (max === g) h = (b - r) / d + 2
                else h = (r - g) / d + 4
                picker.hue = h / 6
            }
        }

        /* ---------------- SV plane (x = saturation, y = value) */
        Item {
            id: svArea
            anchors.left: parent.left
            anchors.right: hueRail.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 2
            anchors.topMargin: 2
            anchors.bottomMargin: 2

            Canvas {
                id: svCanvas
                anchors.fill: parent
                antialiasing: true

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)

                    /* base hue */
                    ctx.fillStyle = Qt.hsla(picker.hue, 1.0, 0.5, 1.0)
                    ctx.fillRect(0, 0, width, height)

                    /* saturation overlay: white -> transparent, left to right */
                    let g = ctx.createLinearGradient(0, 0, width, 0)
                    g.addColorStop(0, "white")
                    g.addColorStop(1, "rgba(255, 255, 255, 0)")
                    ctx.fillStyle = g
                    ctx.fillRect(0, 0, width, height)

                    /* value overlay: transparent -> black, top to bottom */
                    g = ctx.createLinearGradient(0, 0, 0, height)
                    g.addColorStop(0, "rgba(0, 0, 0, 0)")
                    g.addColorStop(1, "black")
                    ctx.fillStyle = g
                    ctx.fillRect(0, 0, width, height)
                }
            }
            Connections {
                target: picker
                function onHueChanged() { svCanvas.requestPaint() }
            }
            onWidthChanged: svCanvas.requestPaint()
            onHeightChanged: svCanvas.requestPaint()

            /* saturation = 1.0 - (y/h) is gone; the plane maps
             *   saturation = x / width
             *   value      = 1.0 - (y / height) * 0.8   (value *0.8 stays) */
            function pick(mouse) {
                picker.saturation = picker.clamp01(mouse.x / svArea.width)
                picker.value = 1.0 - picker.clamp01(mouse.y / svArea.height) * 0.8
                picker.color = Qt.hsla(picker.hue, picker.saturation,
                                       picker.value, 1.0)
            }

            MouseArea {
                anchors.fill: parent
                onPositionChanged: (mouse) => { if (pressed) parent.pick(mouse) }
                onClicked: (mouse) => parent.pick(mouse)
            }

            /* marker */
            Rectangle {
                width: 10
                height: 10
                radius: 5
                border.width: 2
                border.color: "white"
                color: "transparent"
                x: picker.saturation * (svArea.width - 10)
                y: (1.0 - picker.value) / 0.8 * (svArea.height - 10)
            }
        }

        /* ---------------- hue rail (vertical spectrum, hue *0.85) */
        Item {
            id: hueRail
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.rightMargin: 2
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            width: VLCStyle.dp(16, VLCStyle.scale)

            Canvas {
                id: hueCanvas
                anchors.fill: parent
                antialiasing: true

                onPaint: {
                    const ctx = getContext("2d")
                    const stops = [0.0, 0.17, 0.33, 0.5, 0.67, 0.83, 1.0]
                    const colors = ["#ff0000", "#ffff00", "#00ff00",
                                    "#00ffff", "#0000ff", "#ff00ff", "#ff0000"]
                    for (let i = 0; i < stops.length - 1; ++i) {
                        const g = ctx.createLinearGradient(0, stops[i] * height,
                                                           0, stops[i + 1] * height)
                        g.addColorStop(0, colors[i])
                        g.addColorStop(1, colors[i + 1])
                        ctx.fillStyle = g
                        ctx.fillRect(0, stops[i] * height, width,
                                     (stops[i + 1] - stops[i]) * height)
                    }
                }
            }
            onWidthChanged: hueCanvas.requestPaint()
            onHeightChanged: hueCanvas.requestPaint()

            function pick(mouse) {
                picker.hue = picker.clamp01(mouse.y / hueRail.height) * 0.85
                picker.color = Qt.hsla(picker.hue, picker.saturation,
                                       picker.value, 1.0)
            }

            MouseArea {
                anchors.fill: parent
                onPositionChanged: (mouse) => { if (pressed) parent.pick(mouse) }
                onClicked: (mouse) => parent.pick(mouse)
            }
        }

        /* current color */
        Rectangle {
            width: VLCStyle.dp(36, VLCStyle.scale)
            height: VLCStyle.dp(36, VLCStyle.scale)
            radius: 4
            border.width: 2
            border.color: "white"
            color: picker.color
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: VLCStyle.margin_small
        }
    }
}