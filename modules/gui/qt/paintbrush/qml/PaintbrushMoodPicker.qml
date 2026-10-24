/*****************************************************************************
 * PaintbrushMoodPicker.qml : NAI Paintbrush — moods, paper, type, badges
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

/* Art direction in one small popover: six Moods (paper + light + palette +
 * type + badge family at once, one undo), the six paper stocks, a styled
 * text entry and the curated badges. No 50-preset menus (spec §8.2, §21-§23). */
Popup {
    id: root

    required property PaintbrushCanvas canvas

    width: VLCStyle.dp(380, VLCStyle.scale)
    padding: VLCStyle.margin_normal
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    background: Rectangle {
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        radius: VLCStyle.margin_xsmall
    }

    contentItem: ColumnLayout {
        spacing: VLCStyle.margin_small

        Widgets.LabelExt {
            text: qsTr("Mood")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        Flow {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            Repeater {
                model: PaintbrushCtx.moodList
                delegate: Button {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: PaintbrushCtx.currentMoodId === modelData.id
                    onClicked: PaintbrushCtx.applyMood(modelData.id)
                    /* a tiny palette strip previews the mood */
                    Row {
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 2
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 1
                        Rectangle { width: 10; height: 3; color: modelData.primary }
                        Rectangle { width: 10; height: 3; color: modelData.secondary }
                        Rectangle { width: 10; height: 3; color: modelData.accent }
                        Rectangle { width: 10; height: 3; color: modelData.glitter }
                    }
                }
            }
        }

        Widgets.LabelExt {
            text: qsTr("Paper")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        Row {
            spacing: VLCStyle.margin_xsmall
            Repeater {
                model: PaintbrushCtx.paperList
                delegate: Rectangle {
                    required property var modelData
                    width: VLCStyle.dp(48, VLCStyle.scale)
                    height: VLCStyle.dp(48, VLCStyle.scale)
                    radius: 4
                    color: modelData.color
                    border.width: PaintbrushCtx.currentPaperId === modelData.id ? 2 : 1
                    border.color: PaintbrushCtx.currentPaperId === modelData.id ? theme.accent : theme.border
                    Image {
                        anchors.fill: parent
                        anchors.margins: 1
                        source: PaintbrushCtx.paperPreviewDataUrl(modelData.id)
                        fillMode: Image.Stretch
                        smooth: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: PaintbrushCtx.setPaper(modelData.id)
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: modelData.name
                    }
                }
            }
        }

        Widgets.LabelExt {
            text: qsTr("Type")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            TextField {
                id: textEntry
                Layout.fillWidth: true
                placeholderText: qsTr("Headline…")
                onAccepted: addText()
            }
            ComboBox {
                id: styleBox
                Layout.preferredWidth: VLCStyle.dp(130, VLCStyle.scale)
                model: PaintbrushCtx.typographyList
                textRole: "name"
                valueRole: "id"
            }
            Button {
                text: qsTr("Add")
                enabled: textEntry.text.trim().length > 0
                onClicked: addText()
            }
        }

        Widgets.LabelExt {
            text: qsTr("Badges")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        Flow {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            Repeater {
                model: PaintbrushCtx.badgeList
                delegate: Rectangle {
                    required property string modelData
                    width: VLCStyle.dp(44, VLCStyle.scale)
                    height: VLCStyle.dp(44, VLCStyle.scale)
                    radius: 4
                    color: badgeArea.containsMouse ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)
                                                   : "transparent"
                    border.width: 1
                    border.color: theme.border
                    Image {
                        anchors.centerIn: parent
                        width: VLCStyle.dp(36, VLCStyle.scale)
                        height: width
                        source: PaintbrushCtx.badgePreviewDataUrl(modelData, PaintbrushCtx.moodPalette[2])
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                    }
                    MouseArea {
                        id: badgeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: PaintbrushCtx.addBadge(modelData, PaintbrushCtx.moodPalette[2], 160)
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: modelData.replace("_", " ")
                    }
                }
            }
        }
    }

    function addText() {
        const t = textEntry.text.trim()
        if (t.length === 0)
            return
        PaintbrushCtx.addStyledText(t, styleBox.currentValue, 96, PaintbrushCtx.moodPalette[3])
        textEntry.text = ""
    }
}
