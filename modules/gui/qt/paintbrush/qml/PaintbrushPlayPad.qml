/*****************************************************************************
 * PaintbrushPlayPad.qml : NAI Paintbrush — Creative Play Pad
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The Play Pad: a floating performance surface, never a second document.
 *
 * Flow: Keep a moment -> the pad opens on the kept cutout with Neon Idol
 * already rendering. Pick a patch, drag the pad (hold blooms, speed sparks),
 * record/loop a move, then Make it real. Every piece of the committed look
 * becomes a normal editable layer in ONE undo step.
 *
 * The stage image is the real composite of the same pieces the commit
 * materializes, served by the pbcreative image provider at preview scale. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    readonly property var creative: PaintbrushCtx.creative
    readonly property var active: creative.activePatch
    readonly property color accent: active.accent ? active.accent : "#7BE7FF"
    readonly property color secondary: active.secondary ? active.secondary : "#FF7AA8"
    readonly property bool open: creative.open

    visible: open
    z: 30

    width: Math.min(VLCStyle.dp(430), parent ? parent.width - VLCStyle.margin_normal * 4 : 430)
    height: Math.min(VLCStyle.dp(720), parent ? parent.height - VLCStyle.margin_normal * 4 : 720)

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    function patchById(id) {
        const list = creative.patchList
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === id)
                return list[i]
        return ({})
    }

    Rectangle {
        anchors.fill: parent
        radius: VLCStyle.dp(16)
        color: "#0C0D14"
        border.width: 1
        border.color: theme.border
        opacity: 0.985
    }

    /* soft stage light behind the panel */
    Rectangle {
        anchors.fill: parent
        anchors.margins: -12
        radius: VLCStyle.dp(24)
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.10)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_normal
        spacing: VLCStyle.margin_small

        /* --------------------------------------------------------- header */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_small

            Rectangle {
                width: VLCStyle.dp(8)
                height: VLCStyle.dp(8)
                radius: width / 2
                color: creative.busy ? "#FFD37A" : root.accent
                SequentialAnimation on opacity {
                    running: root.open && !creative.busy
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.35; to: 1.0; duration: 900; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 1.0; to: 0.35; duration: 900; easing.type: Easing.InOutSine }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Widgets.LabelExt {
                    text: qsTr("Play Pad")
                    color: theme.fg.primary
                    font.pixelSize: VLCStyle.fontSize_large
                    font.weight: Font.DemiBold
                }
                Widgets.LabelExt {
                    text: root.active.name
                          ? root.active.name + "  ·  " + root.active.tagline
                          : qsTr("Pick a patch and play")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Widgets.ButtonExt {
                text: qsTr("Close")
                Accessible.name: qsTr("Close the Play Pad")
                onClicked: creative.close()
            }
        }

        /* ---------------------------------------------------------- stage */
        Rectangle {
            id: stageFrame
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: VLCStyle.dp(160)
            radius: VLCStyle.dp(12)
            color: "#05060B"
            border.width: 1
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.35)
            clip: true

            readonly property real previewAspect:
                root.creative.previewHeight > 0
                    ? root.creative.previewWidth / root.creative.previewHeight
                    : 0.8

            /* ambient blow-up of the stage fills the frame; the real stage is
             * fitted on top, so the preview is never letterboxed into a stamp */
            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                opacity: 0.30
                cache: false
                asynchronous: false
                smooth: true
                source: root.creative.hasSubject
                            ? "image://pbcreative/live/" + root.creative.previewRevision
                            : ""
            }

            Image {
                id: stageImage
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height * parent.previewAspect)
                height: width / parent.previewAspect
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: false
                smooth: true
                source: root.creative.hasSubject
                            ? "image://pbcreative/live/" + root.creative.previewRevision
                            : ""
            }

            /* darken the ambient copy so the live stage reads first */
            Rectangle {
                anchors.fill: parent
                color: Qt.rgba(0, 0, 0, 0.18)
            }

            /* live badge + subject label */
            RowLayout {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.margin_xsmall

                Rectangle {
                    width: VLCStyle.dp(38)
                    height: VLCStyle.dp(18)
                    radius: height / 2
                    color: Qt.rgba(0, 0, 0, 0.55)
                    border.width: 1
                    border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.5)
                    Widgets.LabelExt {
                        anchors.centerIn: parent
                        text: qsTr("LIVE")
                        color: root.accent
                        font.pixelSize: VLCStyle.dp(9)
                        font.weight: Font.Bold
                        font.letterSpacing: 1.2
                    }
                }
            }

            Widgets.LabelExt {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: VLCStyle.margin_small
                text: root.creative.subjectLabel
                color: Qt.rgba(1, 1, 1, 0.72)
                font.pixelSize: VLCStyle.fontSize_small
                elide: Text.ElideRight
                width: parent.width - VLCStyle.margin_small * 2
            }

            /* busy veil */
            Rectangle {
                anchors.fill: parent
                visible: root.creative.busy
                color: Qt.rgba(0, 0, 0, 0.55)
                BusyIndicator {
                    anchors.centerIn: parent
                    running: root.creative.busy
                }
            }

            /* post-Keep chooser: two big patches, default already rendering */
            Rectangle {
                id: chooser
                anchors.fill: parent
                visible: root.creative.chooserOpen && root.creative.hasSubject
                color: Qt.rgba(4, 5, 10, 0.72)

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: VLCStyle.margin_small
                    spacing: VLCStyle.margin_small

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Start here")
                        color: "#FFFFFF"
                        font.pixelSize: VLCStyle.fontSize_large
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Two looks, one tap. Every result stays editable.")
                        color: Qt.rgba(1, 1, 1, 0.72)
                        font.pixelSize: VLCStyle.fontSize_small
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: VLCStyle.margin_small

                        Repeater {
                            model: ["neon-idol", "sticker-forge"]
                            delegate: Rectangle {
                                id: card
                                required property string modelData
                                readonly property var patch: root.patchById(modelData)
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: VLCStyle.dp(110)
                                radius: VLCStyle.dp(11)
                                color: Qt.rgba(1, 1, 1, 0.06)
                                border.width: 1
                                border.color: Qt.rgba(root.accent.r,
                                                      root.accent.g,
                                                      root.accent.b, 0.35)

                                Rectangle {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop {
                                            position: 0.0
                                            color: Qt.rgba(root.accent.r,
                                                           root.accent.g,
                                                           root.accent.b, 0.28)
                                        }
                                        GradientStop {
                                            position: 1.0
                                            color: Qt.rgba(root.secondary.r,
                                                           root.secondary.g,
                                                           root.secondary.b, 0.10)
                                        }
                                    }
                                }

                                Column {
                                    anchors.fill: parent
                                    anchors.margins: VLCStyle.margin_small
                                    spacing: VLCStyle.margin_xsmall

                                    Widgets.LabelExt {
                                        width: parent.width
                                        text: card.patch.name
                                        color: "#FFFFFF"
                                        font.pixelSize: VLCStyle.fontSize_large
                                        font.weight: Font.Bold
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                                    Widgets.LabelExt {
                                        width: parent.width
                                        text: card.patch.tagline
                                        color: Qt.rgba(1, 1, 1, 0.70)
                                        font.pixelSize: VLCStyle.fontSize_small
                                        horizontalAlignment: Text.AlignHCenter
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.creative.selectPatch(card.modelData)
                                }
                            }
                        }
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("More under the stage")
                        color: Qt.rgba(1, 1, 1, 0.5)
                        font.pixelSize: VLCStyle.fontSize_small
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }

            /* no subject yet */
            ColumnLayout {
                anchors.centerIn: parent
                visible: !root.creative.hasSubject
                spacing: VLCStyle.margin_small

                Widgets.LabelExt {
                    text: qsTr("Pick a cutout first")
                    color: "#FFFFFF"
                    font.pixelSize: VLCStyle.fontSize_large
                    horizontalAlignment: Text.AlignHCenter
                }
                Widgets.LabelExt {
                    text: qsTr("Select an object and Keep it, then the pad opens here.")
                    color: Qt.rgba(1, 1, 1, 0.7)
                    font.pixelSize: VLCStyle.fontSize_small
                    horizontalAlignment: Text.AlignHCenter
                    Layout.maximumWidth: root.width - VLCStyle.margin_normal * 3
                    wrapMode: Text.WordWrap
                }
                Widgets.ButtonExt {
                    text: qsTr("Use the selected layer")
                    onClicked: PaintbrushCtx.openCreativePad("")
                }
            }
        }

        /* ------------------------------------------------------------ pad */
        Rectangle {
            id: pad
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(stageFrame.width, VLCStyle.dp(220))
            Layout.preferredHeight: Layout.preferredWidth
            radius: width / 2
            color: "#080910"
            border.width: 1
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.45)
            clip: true

            property real lastX: width / 2
            property real lastY: height / 2
            property bool touchActive: false

            /* stage grid */
            Repeater {
                model: 5
                delegate: Rectangle {
                    x: pad.width * (index + 1) / 6
                    width: 1
                    height: pad.height
                    color: Qt.rgba(1, 1, 1, 0.07)
                }
            }
            Repeater {
                model: 5
                delegate: Rectangle {
                    y: pad.height * (index + 1) / 6
                    width: pad.width
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.07)
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.52
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.10)
            }
            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.24
                height: width
                radius: width / 2
                color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.08)
                border.width: 1
                border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.55)
            }

            /* crosshair follows the performance */
            Rectangle {
                width: parent.width
                height: 1
                y: Math.max(0, Math.min(parent.height - 1,
                                        root.creative.padY * parent.height))
                color: Qt.rgba(root.secondary.r, root.secondary.g,
                               root.secondary.b, 0.35)
            }
            Rectangle {
                height: parent.height
                width: 1
                x: Math.max(0, Math.min(parent.width - 1,
                                        root.creative.padX * parent.width))
                color: Qt.rgba(root.secondary.r, root.secondary.g,
                               root.secondary.b, 0.35)
            }

            /* puck */
            Item {
                id: puck
                width: VLCStyle.dp(46)
                height: width
                x: root.creative.padX * (pad.width - width)
                y: root.creative.padY * (pad.height - height)
                scale: 1.0 + root.creative.padEnergy * 0.18
                opacity: 1.0

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 1.7
                    height: width
                    radius: width / 2
                    color: Qt.rgba(root.accent.r, root.accent.g,
                                   root.accent.b, 0.16)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: width
                    radius: width / 2
                    color: Qt.rgba(root.accent.r, root.accent.g,
                                   root.accent.b, 0.30)
                    border.width: 2
                    border.color: "#FFFFFF"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.30
                    height: width
                    radius: width / 2
                    color: "#FFFFFF"
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: !pad.touchActive && root.creative.hasSubject
                cursorShape: Qt.CrossCursor
                preventStealing: true

                function apply(x, y, hold) {
                    const nx = Math.max(0, Math.min(1, x / pad.width))
                    const ny = Math.max(0, Math.min(1, y / pad.height))
                    const dx = (x - pad.lastX) / pad.width
                    const dy = (y - pad.lastY) / pad.height
                    const speed = Math.min(1.0, Math.sqrt(dx * dx + dy * dy) * 9.0)
                    pad.lastX = x
                    pad.lastY = y
                    root.creative.movePad(nx, ny, speed, hold)
                }

                onPressed: (mouse) => {
                    pad.lastX = mouse.x
                    pad.lastY = mouse.y
                    root.creative.beginPad()
                    apply(mouse.x, mouse.y, holdButton.pressed)
                }
                onPositionChanged: (mouse) => {
                    if (pressed)
                        apply(mouse.x, mouse.y, holdButton.pressed)
                }
                onReleased: root.creative.endPad()
            }

            MultiPointTouchArea {
                anchors.fill: parent
                enabled: root.creative.hasSubject
                minimumTouchPoints: 1
                maximumTouchPoints: 1
                touchPoints: [ TouchPoint { id: touchPoint } ]
                onPressed: {
                    pad.touchActive = true
                    root.creative.beginPad()
                    root.creative.movePad(touchPoint.x / pad.width,
                                          touchPoint.y / pad.height,
                                          touchPoint.pressure,
                                          holdButton.pressed)
                }
                onUpdated: {
                    root.creative.movePad(touchPoint.x / pad.width,
                                          touchPoint.y / pad.height,
                                          touchPoint.pressure,
                                          holdButton.pressed)
                }
                onReleased: {
                    pad.touchActive = false
                    root.creative.endPad()
                }
            }
        }

        /* -------------------------------------------------------- controls */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.ButtonExt {
                id: holdButton
                Layout.fillWidth: true
                text: qsTr("Hold")
                Accessible.name: qsTr("Bloom while pressed")
                onPressed: root.creative.setHold(true)
                onPressedChanged: {
                    if (!pressed)
                        root.creative.setHold(false)
                }
            }

            Widgets.ButtonExt {
                Layout.fillWidth: true
                text: root.creative.recording ? qsTr("Stop") : qsTr("Record")
                Accessible.name: qsTr("Capture the movement")
                onClicked: root.creative.toggleRecord()
            }

            Widgets.ButtonExt {
                Layout.fillWidth: true
                text: root.creative.replaying ? qsTr("Stop loop") : qsTr("Loop")
                enabled: root.creative.hasRecording || root.creative.replaying
                onClicked: root.creative.toggleReplay()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.ButtonExt {
                Layout.fillWidth: true
                text: qsTr("Surprise")
                Accessible.name: qsTr("Try another patch with a new seed")
                onClicked: root.creative.surprise()
            }

            Widgets.ButtonExt {
                Layout.fillWidth: true
                text: qsTr("Reseed")
                Accessible.name: qsTr("New seed for this patch")
                onClicked: root.creative.reseed()
            }

            Widgets.ButtonExt {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(40)
                text: root.creative.committed ? qsTr("Committed")
                                              : qsTr("Make it real")
                enabled: root.creative.canCommit
                Accessible.name: qsTr("Commit every piece as editable layers, one undo")
                onClicked: root.creative.commit()
            }

            Widgets.ButtonExt {
                Layout.preferredWidth: VLCStyle.dp(88)
                text: qsTr("Close")
                onClicked: root.creative.close()
            }
        }

        /* ------------------------------------------------------ suggestion */
        Rectangle {
            Layout.fillWidth: true
            visible: root.creative.suggestionVisible
            radius: VLCStyle.dp(10)
            color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.10)
            border.width: 1
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.4)
            implicitHeight: suggestionRow.implicitHeight + VLCStyle.margin_small * 2

            RowLayout {
                id: suggestionRow
                anchors.fill: parent
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.margin_small

                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: root.creative.suggestionText
                    color: "#FFFFFF"
                    font.pixelSize: VLCStyle.fontSize_small
                    wrapMode: Text.WordWrap
                }
                Widgets.ButtonExt {
                    text: qsTr("Add")
                    onClicked: root.creative.acceptSuggestion()
                }
                Widgets.ButtonExt {
                    text: qsTr("Not now")
                    onClicked: root.creative.dismissSuggestion()
                }
            }
        }

        /* ------------------------------------------------------ patch rail */
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(66)
            orientation: ListView.Horizontal
            spacing: VLCStyle.margin_xsmall
            clip: true
            model: root.creative.patchList

            delegate: Rectangle {
                id: chip
                required property var modelData
                readonly property bool active: modelData.id === root.creative.activePatchId
                width: Math.max(VLCStyle.dp(92), chipLabel.implicitWidth + VLCStyle.margin_normal * 2)
                height: VLCStyle.dp(52)
                radius: VLCStyle.dp(9)
                color: active ? Qt.rgba(root.accent.r, root.accent.g,
                                        root.accent.b, 0.22)
                              : Qt.rgba(1, 1, 1, 0.05)
                border.width: 1
                border.color: active ? Qt.rgba(root.accent.r, root.accent.g,
                                               root.accent.b, 0.75)
                                     : Qt.rgba(1, 1, 1, 0.10)

                Column {
                    anchors.centerIn: parent
                    spacing: 1
                    Widgets.LabelExt {
                        id: chipLabel
                        text: chip.modelData.name
                        color: "#FFFFFF"
                        font.pixelSize: VLCStyle.fontSize_small
                        font.weight: chip.active ? Font.DemiBold : Font.Normal
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    Widgets.LabelExt {
                        text: chip.modelData.family
                        color: Qt.rgba(1, 1, 1, 0.5)
                        font.pixelSize: VLCStyle.dp(9)
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.creative.selectPatch(chip.modelData.id)
                }
            }
        }

        /* ---------------------------------------------------------- status */
        Widgets.LabelExt {
            Layout.fillWidth: true
            text: root.creative.status
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
            elide: Text.ElideRight
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.open
        onActivated: root.creative.close()
    }

    FocusScope {
        anchors.fill: parent
        Keys.onEscapePressed: root.creative.close()
    }
}
