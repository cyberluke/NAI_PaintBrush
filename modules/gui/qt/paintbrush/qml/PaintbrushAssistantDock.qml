/*****************************************************************************
 * PaintbrushAssistantDock.qml : NAI Paintbrush — companion chat
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

/* The creative companion (A41), rebuilt as a real chat surface.
 *
 * - collapsed to a sparkle pill; expands in place without taking canvas width
 * - header: identity, live status, clear conversation, collapse
 * - compact Off/Quiet/Buddy/Kids + talk-level controls (no wall of buttons)
 * - a small state-driven Buddy nudge card (no AI request, fully dismissible)
 * - message bubbles (user right, companion left, system/error cards)
 * - live streaming answer with a blinking caret and typing dots
 * - collapsible reasoning ("Thinking…") that streams in its own card
 * - tool-call chips with a spinning state, then a result/check
 * - Stop while answering, Retry on error, Enter sends / Shift+Enter newline
 * - always-visible scrollbar and follow-the-answer auto-scroll
 */
Item {
    id: root

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    property bool expanded: false
    property bool reasoningOpen: true
    property bool follow: true

    readonly property real pillSize: VLCStyle.dp(48, VLCStyle.scale)
    readonly property real panelWidth: VLCStyle.dp(420, VLCStyle.scale)
    readonly property real panelHeight: Math.min(
        VLCStyle.dp(640, VLCStyle.scale),
        (parent ? parent.height : 640) - VLCStyle.margin_normal * 2)
    readonly property bool busy: PaintbrushCtx.assistantBusy
    readonly property bool canChat: PaintbrushCtx.companionMode !== 0
    readonly property bool kidsMode: PaintbrushCtx.companionMode === 3
    readonly property bool buddyMode: PaintbrushCtx.companionMode === 2
    readonly property bool buddyish: root.kidsMode || root.buddyMode
    readonly property bool offlineKids: root.kidsMode
                                        && (!PaintbrushCtx.kidsNetworkAllowed
                                            || !PaintbrushCtx.assistantConfigured)
    readonly property bool offlineBuddy: root.buddyMode
                                          && !PaintbrushCtx.assistantConfigured

    width: root.expanded ? root.panelWidth : root.pillSize
    height: root.expanded ? root.panelHeight : root.pillSize
    Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    function greetingText() {
        if (PaintbrushCtx.companionMode === 0)
            return qsTr("Buddy is napping.")
        if (root.kidsMode)
            return qsTr("Hi! I’m Buddy. Ask me about your picture.")
        return qsTr("Hey — I’m here. Ask about the picture, or tell me what to change.")
    }

    function exampleText() {
        if (root.kidsMode)
            return qsTr("Try “add some glitter” or “what should I draw next?”")
        return qsTr("Try “make the colors warmer” or “what should I add?”")
    }

    function offlineNoteText() {
        if (root.offlineKids)
            return qsTr("I can’t use picture magic right now, but I can still share ideas.")
        if (root.offlineBuddy)
            return qsTr("The AI is not connected, so I’ll stick to offline ideas.")
        return ""
    }

    function modeHelpText() {
        switch (PaintbrushCtx.companionMode) {
        case 0:
            return qsTr("Off — Buddy is napping.")
        case 1:
            return qsTr("Quiet — Buddy answers when you talk, and stays calm otherwise.")
        case 2:
            return qsTr("Buddy — cheers you on and drops one small idea now and then.")
        case 3:
            return qsTr("Kids — gentle, playful and safe. A grown-up can turn on picture magic.")
        default:
            return ""
        }
    }

    function inputHintText() {
        if (!root.canChat)
            return qsTr("Companion is off")
        return root.kidsMode ? qsTr("Ask Buddy anything…")
                             : qsTr("Ask about your drawing…")
    }

    function sendQuestion() {
        const text = chatInput.text.trim()
        if (!text || root.busy || !root.canChat)
            return
        chatInput.clear()
        PaintbrushCtx.companionAsk(text)
        root.follow = true
        Qt.callLater(chatInput.forceActiveFocus)
    }

    function retryLast() {
        if (root.busy)
            return
        const list = PaintbrushCtx.assistantMessages
        for (let i = list.length - 1; i >= 0; --i) {
            if (list[i].role === "user") {
                PaintbrushCtx.companionAsk(list[i].text)
                root.follow = true
                return
            }
        }
    }

    /* ------------------------------------------------------------- pill */
    Rectangle {
        id: pill

        width: root.pillSize
        height: root.pillSize
        radius: width / 2
        visible: !root.expanded
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

        Rectangle {
            width: 10
            height: 10
            radius: 5
            color: root.busy ? theme.accent
                             : (PaintbrushCtx.companionMode !== 0 ? "limegreen" : "#888")
            border.width: 2
            border.color: theme.bg.primary
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }

        /* A tiny idea dot, never a popup: the child opens the dock only when
         * they want company. */
        Rectangle {
            width: 9
            height: 9
            radius: 4.5
            color: theme.accent
            border.width: 2
            border.color: theme.bg.primary
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 2
            anchors.topMargin: 2
            visible: root.buddyish && PaintbrushCtx.companionSuggestionVisible
                     && !root.busy
        }

        MouseArea {
            id: pillMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                root.expanded = true
                root.follow = true
                Qt.callLater(chatInput.forceActiveFocus)
            }
            ToolTip.visible: containsMouse
            ToolTip.text: root.buddyish
                          ? qsTr("Buddy — tap for an idea or a chat")
                          : qsTr("Creative companion — ask about your drawing")
            ToolTip.delay: 600
        }
    }

    /* ------------------------------------------------------------ panel */
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

            /* header */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Rectangle {
                    width: VLCStyle.dp(30, VLCStyle.scale)
                    height: width
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

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Widgets.LabelExt {
                        text: qsTr("Companion")
                        color: theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_large
                    }
                    RowLayout {
                        spacing: 4
                        Rectangle {
                            width: 7; height: 7; radius: 3.5
                            color: root.busy ? theme.accent
                                             : (root.canChat ? "limegreen" : "#888")
                        }
                        Widgets.LabelExt {
                            text: root.busy ? qsTr("Thinking…")
                                            : (root.canChat ? qsTr("Here")
                                                            : qsTr("Off"))
                            color: root.busy ? theme.accent : theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                    }
                }

                Widgets.IconToolButton {
                    text: VLCIcons.del
                    description: qsTr("Clear conversation")
                    onClicked: PaintbrushCtx.companionClearConversation()
                }
                Widgets.IconToolButton {
                    text: VLCIcons.close
                    description: qsTr("Collapse companion")
                    onClicked: root.expanded = false
                }
            }

            /* compact settings, not eight big buttons */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Widgets.LabelExt {
                    text: qsTr("Mode")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                }
                ComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Off"), qsTr("Quiet"), qsTr("Buddy"), qsTr("Kids")]
                    currentIndex: PaintbrushCtx.companionMode
                    onActivated: PaintbrushCtx.setCompanionMode(currentIndex)
                    font.pixelSize: VLCStyle.fontSize_small
                }
                Widgets.LabelExt {
                    text: qsTr("Talk")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                    visible: root.canChat
                }
                ComboBox {
                    Layout.preferredWidth: VLCStyle.dp(128, VLCStyle.scale)
                    visible: root.canChat
                    enabled: root.canChat && PaintbrushCtx.companionMode !== 1
                    model: [qsTr("Only when I ask"), qsTr("Talk less"), qsTr("Talk")]
                    currentIndex: PaintbrushCtx.companionTalkLevel
                    onActivated: PaintbrushCtx.companionTalkLevel = currentIndex
                    font.pixelSize: VLCStyle.fontSize_small
                }
            }

            /* The mode/talk model, in the UI where the question is asked:
             * Quiet is answer-only, and Talk is ignored there. */
            Widgets.LabelExt {
                Layout.fillWidth: true
                text: root.modeHelpText()
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
                wrapMode: Text.WordWrap
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: theme.border
            }

            /* Buddy nudge: one small, dismissible next step derived from the
             * real editor state. It never blocks the canvas and never starts
             * an AI request. */
            Rectangle {
                id: nudgeCard

                Layout.fillWidth: true
                visible: root.buddyish && PaintbrushCtx.companionSuggestionVisible
                         && !root.busy
                implicitHeight: nudgeColumn.implicitHeight
                                + 2 * VLCStyle.margin_small
                radius: VLCStyle.margin_xsmall
                color: Qt.rgba(theme.accent.r, theme.accent.g,
                               theme.accent.b, 0.08)
                border.width: 1
                border.color: Qt.rgba(theme.accent.r, theme.accent.g,
                                      theme.accent.b, 0.35)

                ColumnLayout {
                    id: nudgeColumn
                    anchors.fill: parent
                    anchors.margins: VLCStyle.margin_small
                    spacing: VLCStyle.margin_xsmall

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall

                        Widgets.IconLabel {
                            text: VLCIcons.topbar_discover
                            color: theme.accent
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                        Widgets.LabelExt {
                            text: qsTr("Buddy has an idea")
                            color: theme.accent
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: PaintbrushCtx.companionSuggestionText
                        color: theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_normal
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall

                        Widgets.ButtonExt {
                            visible: PaintbrushCtx.companionSuggestionActionLabel.length > 0
                                     && PaintbrushCtx.companionSuggestionAction !== "none"
                            text: PaintbrushCtx.companionSuggestionActionLabel
                            onClicked: PaintbrushCtx.acceptCompanionSuggestion()
                            Accessible.name: text
                        }
                        Widgets.ButtonExt {
                            text: qsTr("Not now")
                            onClicked: PaintbrushCtx.dismissCompanionSuggestion()
                            Accessible.name: qsTr("Dismiss Buddy's idea")
                        }
                        Item { Layout.fillWidth: true }
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
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

                property bool atBottom: true
                onMovementEnded: root.follow = atYEnd
                onContentHeightChanged: if (root.follow) Qt.callLater(positionViewAtEnd)
                onCountChanged: {
                    root.follow = true
                    Qt.callLater(positionViewAtEnd)
                }

                function scrollToEnd() {
                    if (root.follow)
                        Qt.callLater(positionViewAtEnd)
                }

                delegate: MessageBubble {
                    width: conversationList.width - VLCStyle.dp(12, VLCStyle.scale)
                    role: modelData.role
                    text: modelData.text
                }

                footer: Item {
                    width: conversationList.width - VLCStyle.dp(12, VLCStyle.scale)
                    implicitHeight: footerColumn.implicitHeight

                    ColumnLayout {
                        id: footerColumn
                        width: parent.width
                        spacing: VLCStyle.margin_xsmall

                        /* empty state */
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: emptyColumn.implicitHeight
                                             + 2 * VLCStyle.margin_small
                            radius: VLCStyle.margin_xsmall
                            color: theme.bg.secondary
                            border.width: 1
                            border.color: theme.border
                            visible: PaintbrushCtx.assistantMessages.length === 0
                                     && !root.busy
                                     && PaintbrushCtx.assistantReasoningText.length === 0

                            ColumnLayout {
                                id: emptyColumn
                                x: VLCStyle.margin_small
                                y: VLCStyle.margin_small
                                width: parent.width - 2 * VLCStyle.margin_small
                                spacing: VLCStyle.margin_xxxsmall

                                Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    text: root.greetingText()
                                    color: theme.fg.primary
                                    font.pixelSize: VLCStyle.fontSize_normal
                                }
                                Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    visible: PaintbrushCtx.companionMode !== 0
                                    text: root.exampleText()
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                    wrapMode: Text.WordWrap
                                }
                                Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    visible: root.offlineKids || root.offlineBuddy
                                    text: root.offlineNoteText()
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                    wrapMode: Text.WordWrap
                                    font.italic: true
                                }
                            }
                        }

                        /* reasoning */
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: reasoningColumn.implicitHeight
                                             + 2 * VLCStyle.margin_small
                            radius: VLCStyle.margin_xsmall
                            color: Qt.rgba(theme.accent.r, theme.accent.g,
                                           theme.accent.b, 0.07)
                            border.width: 1
                            border.color: Qt.rgba(theme.accent.r, theme.accent.g,
                                                  theme.accent.b, 0.22)
                            visible: PaintbrushCtx.assistantReasoningText.length > 0

                            ColumnLayout {
                                id: reasoningColumn
                                x: VLCStyle.margin_small
                                y: VLCStyle.margin_small
                                width: parent.width - 2 * VLCStyle.margin_small
                                spacing: VLCStyle.margin_xxxsmall

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: VLCStyle.margin_xsmall

                                    Widgets.IconLabel {
                                        text: VLCIcons.topbar_discover
                                        color: theme.accent
                                        font.pixelSize: VLCStyle.fontSize_small
                                        opacity: 1
                                        SequentialAnimation on opacity {
                                            running: root.busy
                                            loops: Animation.Infinite
                                            NumberAnimation { to: 0.25; duration: 700 }
                                            NumberAnimation { to: 1.0; duration: 700 }
                                        }
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: root.busy ? qsTr("Thinking…")
                                                        : qsTr("Thought process")
                                        color: theme.accent
                                        font.pixelSize: VLCStyle.fontSize_small
                                    }
                                    Widgets.IconLabel {
                                        text: root.reasoningOpen
                                              ? VLCIcons.chevron_down
                                              : VLCIcons.ic_fluent_chevron_right_24
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
                                    }
                                    TapHandler {
                                        onTapped: root.reasoningOpen = !root.reasoningOpen
                                    }
                                }

                                TextEdit {
                                    Layout.fillWidth: true
                                    visible: root.reasoningOpen
                                    text: PaintbrushCtx.assistantReasoningText
                                    readOnly: true
                                    selectByMouse: true
                                    color: theme.fg.secondary
                                    selectionColor: Qt.rgba(theme.accent.r, theme.accent.g,
                                                            theme.accent.b, 0.35)
                                    selectedTextColor: theme.fg.primary
                                    font.pixelSize: VLCStyle.fontSize_small
                                    font.italic: true
                                    wrapMode: TextEdit.WordWrap
                                    textFormat: TextEdit.PlainText
                                    persistentSelection: true
                                }
                            }
                        }

                        /* tool calls */
                        Repeater {
                            model: PaintbrushCtx.assistantToolCalls
                            delegate: Rectangle {
                                id: toolChip

                                required property var modelData
                                readonly property bool running: modelData.status === "running"
                                readonly property bool failed: modelData.status === "error"

                                Layout.fillWidth: true
                                implicitHeight: VLCStyle.dp(34, VLCStyle.scale)
                                radius: VLCStyle.margin_xsmall
                                color: failed
                                       ? Qt.rgba(0.75, 0.25, 0.25, 0.14)
                                       : theme.bg.secondary
                                border.width: 1
                                border.color: failed
                                              ? Qt.rgba(0.85, 0.35, 0.35, 0.45)
                                              : theme.border
                                opacity: 0
                                Component.onCompleted: opacity = 1
                                Behavior on opacity { NumberAnimation { duration: 180 } }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: VLCStyle.margin_small
                                    anchors.rightMargin: VLCStyle.margin_small
                                    spacing: VLCStyle.margin_xsmall

                                    Widgets.IconLabel {
                                        text: toolChip.failed ? VLCIcons.remove
                                              : toolChip.running
                                                ? VLCIcons.ic_fluent_arrow_sync_24_regular
                                                : VLCIcons.ok
                                        color: toolChip.failed ? theme.accent : theme.accent
                                        font.pixelSize: VLCStyle.fontSize_small
                                        RotationAnimator on rotation {
                                            running: toolChip.running
                                            loops: Animation.Infinite
                                            from: 0; to: 360; duration: 900
                                        }
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: modelData.name
                                        color: theme.fg.primary
                                        font.pixelSize: VLCStyle.fontSize_small
                                        elide: Text.ElideRight
                                    }
                                    Widgets.LabelExt {
                                        text: modelData.summary
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
                                        elide: Text.ElideRight
                                        Layout.maximumWidth: parent.width * 0.5
                                    }
                                }
                            }
                        }

                        /* streaming answer */
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: streamColumn.implicitHeight
                                             + 2 * VLCStyle.margin_small
                            radius: VLCStyle.margin_xsmall
                            color: theme.bg.secondary
                            border.width: 1
                            border.color: theme.border
                            visible: root.busy
                                     || PaintbrushCtx.assistantStreamingText.length > 0

                            ColumnLayout {
                                id: streamColumn
                                x: VLCStyle.margin_small
                                y: VLCStyle.margin_small
                                width: parent.width - 2 * VLCStyle.margin_small
                                spacing: VLCStyle.margin_xxxsmall

                                RowLayout {
                                    spacing: 4
                                    visible: PaintbrushCtx.assistantStreamingText.length === 0
                                    TypingDots { }
                                }

                                TextEdit {
                                    Layout.fillWidth: true
                                    visible: PaintbrushCtx.assistantStreamingText.length > 0
                                    text: PaintbrushCtx.assistantStreamingText
                                    readOnly: true
                                    selectByMouse: true
                                    color: theme.fg.primary
                                    selectionColor: Qt.rgba(theme.accent.r, theme.accent.g,
                                                            theme.accent.b, 0.35)
                                    selectedTextColor: theme.fg.primary
                                    font.pixelSize: VLCStyle.fontSize_normal
                                    wrapMode: TextEdit.WordWrap
                                    textFormat: TextEdit.PlainText
                                    persistentSelection: true
                                }

                                Rectangle {
                                    width: 2
                                    height: 14
                                    color: theme.accent
                                    opacity: 1
                                    visible: root.busy
                                    SequentialAnimation on opacity {
                                        running: root.busy
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0; duration: 450 }
                                        NumberAnimation { to: 1; duration: 450 }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Connections {
                target: PaintbrushCtx
                function onAssistantStreamChanged() { conversationList.scrollToEnd() }
                function onAssistantToolsChanged() { conversationList.scrollToEnd() }
                function onCompanionStateChanged() { conversationList.scrollToEnd() }
                function onProactiveMessageArrived() {
                    /* The product goal: the companion reacts to the drawing.
                     * Surface the message instead of writing it to a closed
                     * drawer. Quiet and "only when asked" never get here. */
                    if ((PaintbrushCtx.companionMode === 2
                         || PaintbrushCtx.companionMode === 3)
                        && PaintbrushCtx.companionTalkLevel > 0) {
                        root.expanded = true
                        root.follow = true
                    }
                }
            }

            /* input */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: VLCStyle.dp(54, VLCStyle.scale)

                    Basic.TextArea {
                        id: chatInput

                        anchors.fill: parent
                        enabled: root.canChat
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_normal
                        padding: VLCStyle.margin_small
                        background: Rectangle {
                            color: theme.bg.secondary
                            radius: VLCStyle.margin_xsmall
                            border.width: 1
                            border.color: chatInput.activeFocus ? theme.accent : theme.border
                        }
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                if (!(event.modifiers & Qt.ShiftModifier)) {
                                    root.sendQuestion()
                                    event.accepted = true
                                }
                            }
                        }
                    }

                    Text {
                        anchors.fill: parent
                        anchors.margins: VLCStyle.margin_small + 4
                        text: root.inputHintText()
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_normal
                        wrapMode: Text.WordWrap
                        visible: chatInput.text.length === 0
                        enabled: false
                    }
                }

                Rectangle {
                    id: sendButton

                    Layout.preferredWidth: VLCStyle.dp(70, VLCStyle.scale)
                    Layout.preferredHeight: VLCStyle.dp(38, VLCStyle.scale)
                    radius: VLCStyle.margin_xsmall
                    color: root.busy
                           ? Qt.rgba(0.75, 0.25, 0.25, 0.18)
                           : (chatInput.text.trim().length > 0 && root.canChat
                              ? theme.accent : theme.bg.secondary)
                    border.width: 1
                    border.color: root.busy
                                  ? Qt.rgba(0.85, 0.35, 0.35, 0.45)
                                  : (chatInput.text.trim().length > 0 && root.canChat
                                     ? theme.accent : theme.border)
                    opacity: (root.busy || (chatInput.text.trim().length > 0 && root.canChat))
                             ? 1.0 : 0.45

                    Widgets.LabelExt {
                        anchors.centerIn: parent
                        text: root.busy ? qsTr("Stop") : qsTr("Send")
                        color: root.busy
                               ? theme.fg.primary
                               : (chatInput.text.trim().length > 0 && root.canChat
                                  ? theme.bg.primary : theme.fg.secondary)
                        font.pixelSize: VLCStyle.fontSize_small
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: root.busy
                                 || (chatInput.text.trim().length > 0 && root.canChat)
                        onClicked: {
                            if (root.busy)
                                PaintbrushCtx.cancelAssistant()
                            else
                                root.sendQuestion()
                        }
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: root.busy ? qsTr("Stop the answer") : qsTr("Send")
                }
            }

            Widgets.LabelExt {
                Layout.fillWidth: true
                text: qsTr("Press Enter to send.")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
            }
        }
    }

    /* ---------------------------------------------------------- components */
    component TypingDots: Row {
        spacing: 4
        Repeater {
            model: 3
            Rectangle {
                width: 6
                height: 6
                radius: 3
                color: theme.accent
                opacity: 0.25
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    PauseAnimation { duration: index * 140 }
                    NumberAnimation { to: 1.0; duration: 380 }
                    NumberAnimation { to: 0.25; duration: 380 }
                    PauseAnimation { duration: 260 }
                }
            }
        }
    }

    component MessageBubble: Item {
        id: bubble

        property string role: "assistant"
        property string text: ""

        readonly property bool isUser: bubble.role === "user"
        readonly property bool isError: bubble.role === "error"
        readonly property bool isSystem: bubble.role === "system"
        readonly property bool isNote: bubble.isError || bubble.isSystem
        readonly property real maxWidth: bubble.width * 0.86

        implicitHeight: bubbleColumn.implicitHeight
        opacity: 0
        Component.onCompleted: opacity = 1
        Behavior on opacity { NumberAnimation { duration: 180 } }

        ColumnLayout {
            id: bubbleColumn
            width: parent.width
            spacing: 2

            Widgets.LabelExt {
                Layout.alignment: bubble.isUser ? Qt.AlignRight : Qt.AlignLeft
                text: bubble.isUser ? qsTr("You")
                      : bubble.isError ? qsTr("Couldn't answer")
                      : bubble.isSystem ? qsTr("Companion")
                      : qsTr("Companion")
                color: bubble.isError ? theme.accent : theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
                visible: !bubble.isNote || bubble.isError
            }

            Rectangle {
                Layout.alignment: bubble.isUser ? Qt.AlignRight : Qt.AlignLeft
                Layout.preferredWidth: bubble.isNote
                                       ? bubble.width
                                       : Math.min(bubble.maxWidth,
                                                  bubbleText.implicitWidth
                                                  + 2 * VLCStyle.margin_small)
                Layout.preferredHeight: bubbleText.contentHeight
                                        + 2 * VLCStyle.margin_small
                radius: VLCStyle.margin_xsmall
                color: bubble.isUser
                       ? Qt.rgba(theme.accent.r, theme.accent.g,
                                 theme.accent.b, 0.16)
                       : bubble.isError
                         ? Qt.rgba(0.75, 0.25, 0.25, 0.14)
                         : bubble.isSystem
                           ? Qt.rgba(theme.accent.r, theme.accent.g,
                                     theme.accent.b, 0.07)
                           : theme.bg.secondary
                border.width: 1
                border.color: bubble.isUser
                              ? Qt.rgba(theme.accent.r, theme.accent.g,
                                        theme.accent.b, 0.35)
                              : bubble.isError
                                ? Qt.rgba(0.85, 0.35, 0.35, 0.45)
                                : theme.border

                TextEdit {
                    id: bubbleText
                    x: VLCStyle.margin_small
                    y: VLCStyle.margin_small
                    width: parent.width - 2 * VLCStyle.margin_small
                    text: bubble.text
                    readOnly: true
                    selectByMouse: true
                    color: theme.fg.primary
                    selectionColor: Qt.rgba(theme.accent.r, theme.accent.g,
                                            theme.accent.b, 0.35)
                    selectedTextColor: theme.fg.primary
                    font.pixelSize: VLCStyle.fontSize_normal
                    wrapMode: TextEdit.WordWrap
                    textFormat: TextEdit.PlainText
                    persistentSelection: true
                }

                MouseArea {
                    anchors.fill: parent
                    visible: bubble.isError
                    onClicked: root.retryLast()
                    ToolTip.visible: containsMouse
                    ToolTip.text: qsTr("Click to try again")
                }
            }
        }
    }
}
