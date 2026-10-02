/*****************************************************************************
 * PaintbrushMediaDrawer.qml : NAI Paintbrush — media drawer / timeline
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
                    text: PaintbrushCtx.trackingPaused ? VLCIcons.play : VLCIcons.pause_filled
                    description: PaintbrushCtx.trackingPaused ? qsTr("Resume tracking")
                                                              : qsTr("Pause tracking")
                    visible: PaintbrushCtx.tracking
                    onClicked: PaintbrushCtx.pauseResumeTracking()
                }

                Widgets.IconToolButton {
                    text: VLCIcons.remove
                    description: qsTr("Cancel tracking")
                    visible: PaintbrushCtx.tracking
                    onClicked: PaintbrushCtx.cancelTracking()
                }

                /* saved tracks: label, range, corrections, seek, delete */
                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: VLCStyle.dp(30, VLCStyle.scale)
                    orientation: ListView.Horizontal
                    spacing: VLCStyle.margin_xsmall
                    clip: true
                    model: PaintbrushCtx.trackList
                    visible: PaintbrushCtx.document.trackCount > 0
                    delegate: Rectangle {
                        width: VLCStyle.dp(150, VLCStyle.scale)
                        height: parent.height
                        radius: VLCStyle.margin_xxxsmall
                        color: Qt.rgba(theme.fg.primary.r, theme.fg.primary.g,
                                       theme.fg.primary.b, 0.06)
                        border.width: 1
                        border.color: modelData.complete ? theme.accent : theme.border

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: VLCStyle.margin_xxxsmall
                            spacing: VLCStyle.margin_xxxsmall

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Widgets.LabelExt {
                                    text: modelData.label
                                    color: theme.fg.primary
                                    font.pixelSize: VLCStyle.fontSize_xxxsmall
                                    elide: Text.ElideRight
                                }
                                Widgets.LabelExt {
                                    text: qsTr("%1 kf · %2 corr")
                                          .arg(modelData.keyframes)
                                          .arg(modelData.corrections)
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_xxxsmall
                                }
                            }
                            Widgets.IconToolButton {
                                text: VLCIcons.remove
                                description: qsTr("Delete track")
                                onClicked: PaintbrushCtx.deleteTrack(modelData.id)
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: PaintbrushCtx.seekToTrackKeyframe(modelData.id, 0)
                        }
                    }
                }

                Widgets.IconToolButton {
                    text: VLCIcons.add
                    description: qsTr("Make animated sticker from the last track")
                    visible: PaintbrushCtx.document.trackCount > 0
                    onClicked: stickerDirDialog.open()
                }
            }
        }

        FolderDialog {
            id: stickerDirDialog
            title: qsTr("Choose a folder for the animated sticker sequence")
            onAccepted: {
                if (currentFolder)
                    PaintbrushCtx.exportTrackedSticker(0, currentFolder)
            }
        }

        /* thumbnail timeline */
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(92, VLCStyle.scale)
            radius: VLCStyle.margin_xsmall
            color: Qt.rgba(theme.bg.primary.r, theme.bg.primary.g, theme.bg.primary.b, 0.5)
            border.width: 1
            border.color: theme.border
            visible: PaintbrushCtx.playerLengthMs > 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: VLCStyle.margin_xsmall
                spacing: VLCStyle.margin_xxxsmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall

                    Widgets.LabelExt {
                        text: qsTr("Timeline")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: PaintbrushCtx.thumbnailsGenerating
                              ? qsTr("Generating thumbnails…")
                              : ""
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                    Widgets.IconToolButton {
                        text: VLCIcons.history
                        description: qsTr("Generate thumbnails")
                        visible: !PaintbrushCtx.thumbnailsGenerating
                        onClicked: PaintbrushCtx.generateThumbnails()
                    }
                    Widgets.IconToolButton {
                        text: VLCIcons.remove
                        description: qsTr("Stop generating")
                        visible: PaintbrushCtx.thumbnailsGenerating
                        onClicked: PaintbrushCtx.cancelThumbnails()
                    }
                }

                ListView {
                    id: thumbStrip
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    orientation: ListView.Horizontal
                    spacing: VLCStyle.margin_xxxsmall
                    clip: true
                    model: PaintbrushCtx.thumbnailList
                    delegate: Rectangle {
                        width: VLCStyle.dp(86, VLCStyle.scale)
                        height: thumbStrip.height
                        radius: VLCStyle.margin_xxxsmall
                        color: model.failed ? Qt.rgba(0.8, 0.2, 0.2, 0.15)
                                            : Qt.rgba(theme.fg.primary.r, theme.fg.primary.g,
                                                      theme.fg.primary.b, 0.06)
                        border.width: model.timeMs === PaintbrushCtx.playerTimeMs ? 2 : 0
                        border.color: theme.accent

                        Image {
                            anchors.fill: parent
                            anchors.margins: 1
                            source: model.hasImage ? PaintbrushCtx.thumbnailDataUrl(index) : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }

                        Widgets.LabelExt {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: 2
                            text: model.hasImage || model.failed
                                  ? root.formatTime(model.timeMs)
                                  : ""
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_xxxsmall
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (model.hasImage)
                                    PaintbrushCtx.playerSeek(model.timeMs)
                            }
                        }
                    }
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