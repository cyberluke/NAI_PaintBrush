/*****************************************************************************
 * PaintbrushAssistantDock.qml : NAI Paintbrush — creative companion pill
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The creative companion (A41): a small avatar pill, collapsed by default,
 * docked in the corner of the canvas. Clicking the pill expands the chat in
 * place — the companion never occupies layout width at startup and never
 * steals canvas space when collapsed. The mode selector (Off/Quiet/Buddy/
 * Kids), talk level, conversation and input row all live inside the
 * expandable surface. */
Item {
    id: root

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    /* collapsed by default */
    property bool expanded: false

    readonly property real pillSize: VLCStyle.dp(48, VLCStyle.scale)
    readonly property real panelWidth: VLCStyle.dp(320, VLCStyle.scale)
    readonly property real panelHeight: Math.min(
        VLCStyle.dp(520, VLCStyle.scale),
        (parent ? parent.height : 520) - VLCStyle.margin_normal * 2)

    width: root.expanded ? root.panelWidth : root.pillSize
    height: root.expanded ? root.panelHeight : root.pillSize

    /* ---------------------------------------------------------- the pill */
    Rectangle {
        id: pill

        width: root.pillSize
        height: root.pillSize
        radius: width / 2
        visible: !root.expanded

        /* avatar: accent gradient disc with a sparkle glyph */
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.9) }
            GradientStop { position: 1.0; color: Qt.rgba(theme.accent.r * 0.55, theme.accent.g * 0.55, theme.accent.b * 0.55, 0.9) }
        }
        border.width: 2
        border.color: theme.bg.primary

        Widgets.IconLabel {
            anchors.centerIn: parent
            text: VLCIcons.topbar_discover
            font.pixelSize: VLCStyle.fontSize_large
            color: "white"
        }

        /* companion state dot: accent when active, gray when off */
        Rectangle {
            width: 10
            height: 10
            radius: 5
            color: PaintbrushCtx.companionMode !== 0 ? "limegreen" : "#888"
            border.width: 2
            border.color: theme.bg.primary
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }

        MouseArea {
            id: pillMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.expanded = true
        }

        ToolTip.visible: pillMouse.hovered
        ToolTip.text: qsTr("Creative companion")
        ToolTip.delay: 600
    }

    /* --------------------------------------------------- expanded chat */
    Rectangle {
        id: panel

        anchors.fill: parent
        visible: root.expanded
        radius: VLCStyle.margin_small
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: VLCStyle.margin_small
            spacing: VLCStyle.margin_xsmall

            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Rectangle {
                    width: VLCStyle.dp(26, VLCStyle.scale)
                    height: VLCStyle.dp(26, VLCStyle.scale)
                    radius: width / 2
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.9) }
                        GradientStop { position: 1.0; color: Qt.rgba(theme.accent.r * 0.55, theme.accent.g * 0.55, theme.accent.b * 0.55, 0.9) }
                    }
                    Widgets.IconLabel {
                        anchors.centerIn: parent
                        text: VLCIcons.topbar_discover
                        font.pixelSize: VLCStyle.fontSize_small
                        color: "white"
                    }
                }
                Widgets.LabelExt {
                    text: qsTr("Companion")
                    color: theme.fg.primary
                    font.pixelSize: VLCStyle.fontSize_large
                }
                Item { Layout.fillWidth: true }
                Widgets.IconToolButton {
                    text: VLCIcons.close
                    description: qsTr("Collapse companion")
                    onClicked: root.expanded = false
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
                    Basic.Button {
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

            /* talk level: how often the companion speaks unprompted */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xxxsmall
                visible: PaintbrushCtx.companionMode !== 0

                Repeater {
                    model: [
                        { level: 0, label: qsTr("Only when I ask") },
                        { level: 1, label: qsTr("Talk less") },
                        { level: 2, label: qsTr("Talk") }
                    ]
                    Basic.Button {
                        Layout.fillWidth: true
                        text: modelData.label
                        checkable: true
                        autoExclusive: true
                        checked: PaintbrushCtx.companionTalkLevel === modelData.level
                        onClicked: PaintbrushCtx.setCompanionTalkLevel(modelData.level)

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
                        implicitHeight: msgText.contentHeight + VLCStyle.margin_xsmall * 2

                        /* read-only TextEdit so the message text can be
                         * selected and copied (Label/Text do not support
                         * selection) */
                        TextEdit {
                            id: msgText
                            anchors.fill: parent
                            anchors.margins: VLCStyle.margin_xsmall
                            text: modelData.text
                            readOnly: true
                            selectByMouse: true
                            color: theme.fg.primary
                            selectionColor: Qt.rgba(theme.accent.r, theme.accent.g,
                                                    theme.accent.b, 0.35)
                            selectedTextColor: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_small
                            wrapMode: Text.WordWrap
                            persistentSelection: true
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
    }

    function sendQuestion() {
        const text = companionInput.text.trim()
        if (!text)
            return
        companionInput.clear()
        PaintbrushCtx.companionAsk(text)
    }
}