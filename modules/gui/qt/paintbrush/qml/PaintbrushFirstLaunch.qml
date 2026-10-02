/*****************************************************************************
 * PaintbrushFirstLaunch.qml : NAI Paintbrush — first-run choices
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

Item {
    id: root

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary
        opacity: 0.96
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: VLCStyle.margin_xlarge
        width: Math.min(parent.width * 0.6, VLCStyle.dp(560, VLCStyle.scale))

        Widgets.LabelExt {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("NAI Paintbrush")
            font.pixelSize: VLCStyle.fontSize_xxxlarge
            color: theme.fg.primary
        }

        Widgets.LabelExt {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Draw it. Cut it out. Ask it. Remix it.")
            font.pixelSize: VLCStyle.fontSize_normal
            color: theme.fg.secondary
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            spacing: VLCStyle.margin_large

            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(96, VLCStyle.scale)
                text: qsTr("Start drawing")
                onClicked: {
                    PaintbrushCtx.newBlankCanvas(1280, 720)
                    PaintbrushCtx.companionNotifyEvent("start-drawing")
                }
            }

            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(96, VLCStyle.scale)
                text: qsTr("Open a picture")
                onClicked: openImageDialog.open()
            }
        }

        Widgets.ActionButtonPrimary {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: parent.width * 0.55
            Layout.preferredHeight: VLCStyle.dp(72, VLCStyle.scale)
            text: qsTr("Play with a video")
            enabled: PaintbrushCtx.playerHasVideo
            onClicked: PaintbrushCtx.captureVideoFrame()
        }

        Widgets.LabelExt {
            Layout.alignment: Qt.AlignHCenter
            text: PaintbrushCtx.playerHasVideo
                  ? qsTr("A video is playing — capture the current frame.")
                  : qsTr("Play a video in VLC first to cut objects out of it.")
            font.pixelSize: VLCStyle.fontSize_xsmall
            color: theme.fg.secondary
        }
    }

    FileDialog {
        id: openImageDialog
        title: qsTr("Open an image")
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)", "All files (*)"]
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.openImageFile(selectedFile)
        }
    }
}