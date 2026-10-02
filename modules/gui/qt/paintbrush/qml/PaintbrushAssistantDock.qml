/*****************************************************************************
 * PaintbrushAssistantDock.qml : NAI Paintbrush — creative companion
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

/* The companion dock: mode selector, conversation, and the ability to ask
 * the companion about the drawing. Collapsed by default for people who
 * only want to paint. */
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

            Widgets.IconLabel {
                text: VLCIcons.info
                color: theme.accent
                font.pixelSize: VLCStyle.fontSize_large
            }
            Widgets.LabelExt {
                text: qsTr("Companion")
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_large
            }
            Item { Layout.fillWidth: true }
            Widgets.IconToolButton {
                text: VLCIcons.close
                description: qsTr("Hide companion")
                onClicked: PaintbrushCtx.setCompanionMode(0)
            }
        }

        /* mode selector */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xxxsmall

            Repeater {
                model: [
                    { mode: 0, label: qsTr("Off") },
                    { mode: 1, label: qsTr("Quiet") },
                    { mode: 2, label: qsTr("Buddy") },
                    { mode: 3, label: qsTr("Kids") }
                ]
                Button {
                    Layout.fillWidth: true
                    text: modelData.label
                    checkable: true
                    autoExclusive: true
                    checked: PaintbrushCtx.companionMode === modelData.mode
                    onClicked: PaintbrushCtx.setCompanionMode(modelData.mode)

                    contentItem: Text {
                        text: parent.text
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: parent.checked ? theme.accent : theme.fg.primary
                    }
                    background: Rectangle {
                        color: parent.checked ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.15)
                                              : "transparent"
                        radius: VLCStyle.margin_xsmall
                    }
                }
            }
        }

        /* conversation */
        ListView {
            id: conversationList

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: VLCStyle.margin_xsmall
            model: PaintbrushCtx.assistantMessages

            delegate: ColumnLayout {
                width: conversationList.width
                spacing: 2

                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: modelData.role === "user" ? qsTr("You") : qsTr("Companion")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }
                Rectangle {
                    Layout.fillWidth: true
                    radius: VLCStyle.margin_xsmall
                    color: modelData.role === "user"
                           ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)
                           : Qt.rgba(0.5, 0.5, 0.5, 0.12)
                    implicitHeight: msgText.implicitHeight + VLCStyle.margin_xsmall * 2

                    Widgets.LabelExt {
                        id: msgText
                        anchors.fill: parent
                        anchors.margins: VLCStyle.margin_xsmall
                        text: modelData.text
                        color: theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            TextField {
                id: companionInput
                Layout.fillWidth: true
                placeholderText: qsTr("Ask about your drawing…")
                onAccepted: sendQuestion()
            }
            Widgets.IconToolButton {
                text: VLCIcons.ok
                description: qsTr("Send")
                onClicked: sendQuestion()
            }
        }

        Widgets.LabelExt {
            Layout.fillWidth: true
            text: PaintbrushCtx.companionThinking ? qsTr("Companion is looking at your drawing…")
                                                  : ""
            color: theme.accent
            font.pixelSize: VLCStyle.fontSize_xsmall
            visible: PaintbrushCtx.companionThinking
        }
    }

    function sendQuestion() {
        const text = companionInput.text.trim()
        if (!text)
            return
        companionInput.clear()
        PaintbrushCtx.companionAsk(text)
    }
}