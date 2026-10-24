/*****************************************************************************
 * PaintbrushBoostBar.qml : NAI Paintbrush — Keep + Boost (spec §5, §24, §44)
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

/* The two product verbs: Keep (harvest the selected moment into the tray) and
 * Boost (one grouped, reversible art-direction pass). Boost speaks in one
 * confident sentence set and "Undo Boost" is one step — no sliders, no
 * per-rule toggles (spec §44, §46). Floats over the canvas, bottom-left. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    implicitWidth: row.implicitWidth + 2 * VLCStyle.margin_small
    implicitHeight: VLCStyle.dp(44, VLCStyle.scale)

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        opacity: 0.96
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: VLCStyle.margin_small

        Button {
            id: keepBtn
            text: qsTr("Keep")
            enabled: PaintbrushCtx.canKeep
            onClicked: PaintbrushCtx.keepSelection()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: PaintbrushCtx.canKeep
                          ? qsTr("Keep this moment in your tray")
                          : qsTr("Select something on the frame first (Object Select)")
        }

        Button {
            id: boostBtn
            text: PaintbrushCtx.boostBusy ? qsTr("Boosting…") : qsTr("Boost")
            highlighted: true
            enabled: PaintbrushCtx.canBoost && !PaintbrushCtx.boostBusy
            onClicked: PaintbrushCtx.boost()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Art-direct the composition: hierarchy, depth, motion. One undo.")
        }

        Button {
            text: qsTr("Undo Boost")
            visible: PaintbrushCtx.canUndo && PaintbrushCtx.undoText.indexOf("Boost") >= 0 && !PaintbrushCtx.boostBusy
            onClicked: PaintbrushCtx.undo()
        }

        Button {
            text: qsTr("Cancel")
            visible: PaintbrushCtx.boostBusy
            onClicked: PaintbrushCtx.cancelBoost()
        }
    }

    /* the summary: one short, confident sentence set shown briefly after a
     * Boost; it fades on its own (never a modal, never a diagnostics panel) */
    Rectangle {
        id: summary
        anchors.bottom: parent.top
        anchors.bottomMargin: VLCStyle.margin_small
        anchors.left: parent.left
        width: Math.min(VLCStyle.dp(460, VLCStyle.scale), summaryLabel.implicitWidth + 2 * VLCStyle.margin_normal)
        height: summaryLabel.implicitHeight + 2 * VLCStyle.margin_small
        radius: VLCStyle.margin_xsmall
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        opacity: 0.0
        visible: opacity > 0.01

        Widgets.LabelExt {
            id: summaryLabel
            anchors.fill: parent
            anchors.margins: VLCStyle.margin_small
            text: PaintbrushCtx.boostSummary
            color: theme.fg.primary
            font.pixelSize: VLCStyle.fontSize_small
            wrapMode: Text.WordWrap
            width: VLCStyle.dp(420, VLCStyle.scale)
        }

        Behavior on opacity { NumberAnimation { duration: 220 } }
        Timer {
            id: hideTimer
            interval: 9000
            onTriggered: summary.opacity = 0.0
        }
    }

    Connections {
        target: PaintbrushCtx
        function onBoostStateChanged() {
            if (!PaintbrushCtx.boostBusy && PaintbrushCtx.boostSummary.length > 0) {
                summary.opacity = 1.0
                hideTimer.restart()
            }
        }
    }
}
