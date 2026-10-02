/*****************************************************************************
 * PaintbrushMediaDrawer.qml : NAI Paintbrush — media drawer / timeline
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

/* Bottom drawer: transport controls, timestamp, frame stepping, seek bar,
 * "use this frame", and the tracked-object lane. */
Item {
    id: root

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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_xsmall

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.IconToolButton {
                text: VLCIcons.previous_frame
                description: qsTr("Previous frame")
                onClicked: PaintbrushCtx.playerFramePrev()
            }
            Widgets.IconToolButton {
                text: PaintbrushCtx.playerTimeMs > 0 ? VLCIcons.pause_filled : VLCIcons.play
                description: qsTr("Play / pause")
                onClicked: PaintbrushCtx.playerPlayPause()
            }
            Widgets.IconToolButton {
                text: VLCIcons.next_frame
                description: qsTr("Next frame")
                onClicked: PaintbrushCtx.playerFrameNext()
            }

            Widgets.LabelExt {
                text: formatTime(PaintbrushCtx.playerTimeMs) + " / " + formatTime(PaintbrushCtx.playerLengthMs)
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
            }

            Slider {
                id: seekSlider
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                from: 0
                to: Math.max(1, PaintbrushCtx.playerLengthMs)
                onMoved: PaintbrushCtx.playerSeek(value)

                /* Follow playback time, but pause the binding while the
                 * user drags so the slider does not fight the model and
                 * QML does not report a binding loop. */
                Binding {
                    target: seekSlider
                    property: "value"
                    value: PaintbrushCtx.playerTimeMs
                    when: !seekSlider.pressed
                }
            }

            Widgets.ActionButtonPrimary {
                text: qsTr("Use this frame")
                onClicked: PaintbrushCtx.captureVideoFrame()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: qsTr("Media")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
            }
            Widgets.LabelExt {
                Layout.fillWidth: true
                text: PaintbrushCtx.currentMediaTitle
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                elide: Text.ElideRight
            }
        }

        /* tracked object lane */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(46, VLCStyle.scale)
            radius: VLCStyle.margin_xsmall
            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
            visible: PaintbrushCtx.tracking || PaintbrushCtx.document.trackCount > 0

            RowLayout {
                anchors.fill: parent
                anchors.margins: VLCStyle.margin_xsmall
                spacing: VLCStyle.margin_xsmall

                Widgets.LabelExt {
                    text: qsTr("Tracked objects")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                }

                ProgressBar {
                    Layout.preferredWidth: VLCStyle.dp(120, VLCStyle.scale)
                    Layout.alignment: Qt.AlignVCenter
                    from: 0
                    to: Math.max(1, PaintbrushCtx.trackingTotal)
                    value: PaintbrushCtx.trackingDone
                    visible: PaintbrushCtx.tracking
                }

                Widgets.LabelExt {
                    text: PaintbrushCtx.tracking ? PaintbrushCtx.trackingStatus : ""
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }

                Widgets.IconToolButton {
                    text: VLCIcons.remove
                    description: qsTr("Cancel tracking")
                    visible: PaintbrushCtx.tracking
                    onClicked: PaintbrushCtx.cancelTracking()
                }

                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.document.trackCount > 0
                          ? qsTr("%n track(s) saved", "", PaintbrushCtx.document.trackCount)
                          : ""
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }

    function formatTime(ms) {
        if (ms <= 0)
            return "00:00"
        const totalSec = Math.floor(ms / 1000)
        const h = Math.floor(totalSec / 3600)
        const m = Math.floor((totalSec % 3600) / 60)
        const s = totalSec % 60
        if (h > 0)
            return ("0" + h).slice(-1) + ":" + ("0" + m).slice(-2) + ":" + ("0" + s).slice(-2)
        return ("0" + m).slice(-2) + ":" + ("0" + s).slice(-2)
    }
}