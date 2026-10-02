/*****************************************************************************
 * PaintbrushPaletteBar.qml : NAI Paintbrush — classic color palette
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

/* Compact Paintbrush-style palette: foreground/background colors, a row of
 * swatches, recent colors and a brush-size slider. Eyedropper integration
 * happens on the canvas (Pick tool). */
Item {
    id: root

    required property PaintbrushCanvas canvas

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

        Item { Layout.fillWidth: true }

        /* spray flow: how hard the can sprays (Spray tool only) */
        Widgets.LabelExt {
            text: qsTr("Flow")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
            visible: canvas.tool === 9
        }
        Slider {
            Layout.preferredWidth: VLCStyle.dp(90, VLCStyle.scale)
            Layout.alignment: Qt.AlignVCenter
            from: 0.1
            to: 4.0
            stepSize: 0.1
            visible: canvas.tool === 9
            value: canvas.sprayFlow
            onMoved: canvas.sprayFlow = value
        }
        Widgets.LabelExt {
            text: canvas.sprayFlow.toFixed(1)
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
            visible: canvas.tool === 9
        }

        /* brush size */
        Widgets.LabelExt {
            text: qsTr("Size")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
        Slider {
            Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
            Layout.alignment: Qt.AlignVCenter
            from: 1
            to: 64
            value: canvas.brushSize
            onMoved: canvas.brushSize = value
        }
        Widgets.LabelExt {
            text: Math.round(canvas.brushSize)
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
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

    /* simple hue/sat/value picker built from primitives */
    component ColorPicker: Rectangle {
        id: picker

        property color color: "black"

        Gradient {
            id: hsvGradient
            GradientStop { position: 0.0; color: "#ff0000" }
            GradientStop { position: 0.17; color: "#ffff00" }
            GradientStop { position: 0.33; color: "#00ff00" }
            GradientStop { position: 0.5; color: "#00ffff" }
            GradientStop { position: 0.67; color: "#0000ff" }
            GradientStop { position: 0.83; color: "#ff00ff" }
            GradientStop { position: 1.0; color: "#ff0000" }
        }

        Rectangle {
            anchors.fill: parent
            gradient: hsvGradient
        }

        Rectangle {
            id: valueOverlay
            anchors.fill: parent
            color: "transparent"
            /* darken towards bottom */
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: "#000000" }
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            onPositionChanged: (mouse) => {
                if (pressed)
                    picker.color = Qt.hsla(
                        (mouse.x / width) * 0.85,
                        1.0 - (mouse.y / height) * 0.0 + 0.0,
                        1.0 - (mouse.y / height) * 0.8,
                        1.0)
            }
            onClicked: (mouse) => {
                picker.color = Qt.hsla(
                    (mouse.x / width) * 0.85,
                    1.0,
                    1.0 - (mouse.y / height) * 0.8,
                    1.0)
            }
        }

        Rectangle {
            width: VLCStyle.dp(36, VLCStyle.scale)
            height: VLCStyle.dp(36, VLCStyle.scale)
            radius: 4
            border.width: 2
            border.color: "white"
            color: picker.color
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: VLCStyle.margin_small
        }
    }
}