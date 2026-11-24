/*****************************************************************************
 * PaintbrushMattePad.qml : NAI Paintbrush — playful matte edge control
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style

/* A two-axis pad for people who do not want to read six parameter names.
 *
 *   X (left -> right): edge band, tight -> wide        (1..24 px)
 *   Y (bottom -> top): clean cut -> hair and fine detail (0..100)
 *
 * The pad emits `changed(boundary, detail)`; the owner turns that gesture
 * into a coherent parameter bundle (hole fill, decontamination and temporal
 * stability follow the detail axis), so one drag feels like one decision
 * instead of six sliders. Advanced controls remain available collapsed. */
Item {
    id: root

    property int boundary: 4   /* 1..24 px inner band */
    property int detail: 50    /* 0..100 fine-detail recovery */

    signal changed(int boundary, int detail)

    implicitHeight: pad.height + labelRow.implicitHeight
                    + VLCStyle.margin_xsmall
    implicitWidth: VLCStyle.dp(230, VLCStyle.scale)

    ColorContext {
        id: theme
        palette: VLCStyle.palette
        colorSet: ColorContext.View
    }

    function applyFromPointer(mx, my) {
        const nx = Math.max(0, Math.min(1, mx / Math.max(1, pad.width)))
        const ny = Math.max(0, Math.min(1, my / Math.max(1, pad.height)))
        const b = Math.round(1 + nx * 23)
        const d = Math.round((1 - ny) * 100)
        root.boundary = b
        root.detail = d
        root.changed(b, d)
    }

    Rectangle {
        id: pad
        anchors.top: parent.top
        width: parent.width
        height: VLCStyle.dp(150, VLCStyle.scale)
        radius: VLCStyle.margin_small
        color: theme.bg.secondary
        border.width: 1
        border.color: theme.border
        clip: true

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 1
            height: parent.height
            color: Qt.rgba(theme.fg.primary.r, theme.fg.primary.g,
                           theme.fg.primary.b, 0.10)
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            width: parent.width
            color: Qt.rgba(theme.fg.primary.r, theme.fg.primary.g,
                           theme.fg.primary.b, 0.10)
        }

        Rectangle {
            width: VLCStyle.dp(34, VLCStyle.scale)
            height: width
            radius: width / 2
            x: Math.max(0, Math.min(pad.width - width,
                                    (root.boundary - 1) / 23 * pad.width - width / 2))
            y: Math.max(0, Math.min(pad.height - height,
                                    (1 - root.detail / 100) * pad.height - height / 2))
            color: theme.accent
            border.width: 2
            border.color: theme.bg.primary
            opacity: 0.92
        }

        MouseArea {
            anchors.fill: parent
            onPressed: (m) => root.applyFromPointer(m.x, m.y)
            onPositionChanged: (m) => {
                if (pressed)
                    root.applyFromPointer(m.x, m.y)
            }
        }

        Widgets.LabelExt {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: VLCStyle.margin_xsmall
            text: qsTr("clean")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
        Widgets.LabelExt {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: VLCStyle.margin_xsmall
            text: qsTr("hair & fine")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
        Widgets.LabelExt {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: VLCStyle.margin_xsmall
            text: qsTr("wide edge")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
    }

    RowLayout {
        id: labelRow
        anchors.top: pad.bottom
        anchors.topMargin: VLCStyle.margin_xsmall
        width: parent.width
        spacing: VLCStyle.margin_xsmall

        Widgets.LabelExt {
            Layout.fillWidth: true
            text: qsTr("Edge control")
            color: theme.fg.primary
            font.bold: true
        }
        Widgets.LabelExt {
            text: root.boundary + " px · " + root.detail + "%"
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
    }
}
