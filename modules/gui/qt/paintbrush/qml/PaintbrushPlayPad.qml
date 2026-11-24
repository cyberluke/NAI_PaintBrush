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
    property string pendingGroup

    visible: open
    z: 30

    width: Math.min(VLCStyle.dp(430), parent ? parent.width - VLCStyle.margin_normal * 4 : 430)
    height: Math.min(VLCStyle.dp(820), parent ? parent.height - VLCStyle.margin_normal * 4 : 820)

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

    /* Tactile pad control: the VLC flat ButtonExt is right for toolbars, but
     * a performance surface needs a visible hit area and a primary CTA. */
    component PadButton: Rectangle {
        id: btn

        property string text
        property bool primary: false
        property bool selected: false
        property bool enabledAction: true

        signal clicked()
        signal held(bool down)

        implicitWidth: Math.max(VLCStyle.dp(64), btnLabel.implicitWidth + VLCStyle.dp(24))
        implicitHeight: VLCStyle.dp(36)
        radius: VLCStyle.dp(8)
        color: !enabledAction
                   ? "#0F1018"
                   : primary
                       ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.92)
                       : (hover.hovered ? "#232738" : "#171926")
        border.width: 1
        border.color: primary
                          ? root.accent
                          : (selected ? root.accent : "#2C3045")
        opacity: enabledAction ? 1.0 : 0.45

        Widgets.LabelExt {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.text
            color: !btn.enabledAction ? "#7A7F94"
                                      : (btn.primary ? "#071018" : "#EDEFF7")
            font.pixelSize: VLCStyle.fontSize_small
            font.weight: btn.primary ? Font.DemiBold : Font.Medium
        }

        HoverHandler { id: hover }

        MouseArea {
            anchors.fill: parent
            enabled: btn.enabledAction
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
            onPressed: btn.held(true)
            onReleased: btn.held(false)
            onCanceled: btn.held(false)
        }

        Accessible.role: Accessible.Button
        Accessible.name: btn.text
        Accessible.onPressAction: btn.clicked()
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

            PadButton {
                Layout.preferredWidth: VLCStyle.dp(64)
                text: qsTr("Close")
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
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#0B0C17" }
                GradientStop { position: 1.0; color: "#04050A" }
            }
            border.width: 1
            border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.35)
            clip: true

            readonly property real previewAspect:
                root.creative.previewHeight > 0
                    ? root.creative.previewWidth / root.creative.previewHeight
                    : 0.8

            /* Full-stage view: one-finger drag pans a zoomed stage, the wheel
             * and +/- zoom it, and the percentage chip resets it. At 1x the
             * pad drag stays untouched. */
            property real stageZoom: 1.0
            property real stagePanX: 0
            property real stagePanY: 0
            readonly property real stageBaseWidth:
                Math.min(width, height * previewAspect)
            readonly property real stageBaseHeight:
                previewAspect > 0 ? stageBaseWidth / previewAspect : height

            function clampStagePan() {
                const maxX = Math.max(0, (stageBaseWidth * stageZoom - width) / 2)
                const maxY = Math.max(0, (stageBaseHeight * stageZoom - height) / 2)
                stagePanX = Math.max(-maxX, Math.min(maxX, stagePanX))
                stagePanY = Math.max(-maxY, Math.min(maxY, stagePanY))
            }
            function setStageZoom(value) {
                const next = Math.max(1.0, Math.min(3.0, value))
                stageZoom = next
                if (next <= 1.001) {
                    stagePanX = 0
                    stagePanY = 0
                } else {
                    clampStagePan()
                }
            }
            function resetStageView() {
                stageZoom = 1.0
                stagePanX = 0
                stagePanY = 0
            }
            onWidthChanged: clampStagePan()
            onHeightChanged: clampStagePan()
            onPreviewAspectChanged: clampStagePan()

            Image {
                id: stageImage
                width: stageFrame.stageBaseWidth * stageFrame.stageZoom
                height: stageFrame.stageBaseHeight * stageFrame.stageZoom
                x: (stageFrame.width - width) / 2 + stageFrame.stagePanX
                y: (stageFrame.height - height) / 2 + stageFrame.stagePanY
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: false
                smooth: true
                source: root.creative.hasSubject
                            ? "image://pbcreative/live/" + root.creative.previewRevision
                            : ""
            }

            /* Drag the zoomed stage to inspect it; at 1x the pad drag wins. */
            MouseArea {
                anchors.fill: parent
                enabled: stageFrame.stageZoom > 1.001
                visible: enabled
                preventStealing: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property real lastX: 0
                property real lastY: 0
                onPressed: function(mouse) {
                    lastX = mouse.x
                    lastY = mouse.y
                }
                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    stageFrame.stagePanX += mouse.x - lastX
                    stageFrame.stagePanY += mouse.y - lastY
                    lastX = mouse.x
                    lastY = mouse.y
                    stageFrame.clampStagePan()
                }
                onDoubleClicked: stageFrame.resetStageView()
            }

            WheelHandler {
                onWheel: function(wheel) {
                    if (wheel.angleDelta.y === 0)
                        return
                    stageFrame.setStageZoom(
                        stageFrame.stageZoom
                        * (wheel.angleDelta.y > 0 ? 1.15 : 0.87))
                    wheel.accepted = true
                }
            }

            /* live badge + subject label */
            RowLayout {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.margin_xsmall

                Rectangle {
                    id: liveBadge
                    width: VLCStyle.dp(48)
                    height: VLCStyle.dp(18)
                    radius: height / 2
                    color: Qt.rgba(0, 0, 0, 0.55)
                    border.width: 1
                    border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.5)
                    Widgets.LabelExt {
                        anchors.centerIn: parent
                        text: root.creative.compareMode ? qsTr("BEFORE") : qsTr("LIVE")
                        color: root.creative.compareMode ? root.secondary : root.accent
                        font.pixelSize: VLCStyle.dp(9)
                        font.weight: Font.Bold
                        font.letterSpacing: 1.0
                    }
                    HoverHandler { id: liveHover }
                    ToolTip.visible: liveHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Preview p95 %1 ms · commit %2 ms")
                                      .arg(root.creative.previewP95Ms.toFixed(1))
                                      .arg(root.creative.lastCommitMs.toFixed(0))
                }
            }

            /* A/B: the untouched source vs the live look */
            PadButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: VLCStyle.margin_small
                implicitWidth: VLCStyle.dp(72)
                implicitHeight: VLCStyle.dp(28)
                text: root.creative.compareMode ? qsTr("After") : qsTr("Before")
                selected: root.creative.compareMode
                enabledAction: root.creative.hasSubject
                onClicked: root.creative.setCompare(!root.creative.compareMode)
            }

            Widgets.LabelExt {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: VLCStyle.margin_small
                text: root.creative.subjectLabel
                color: Qt.rgba(1, 1, 1, 0.72)
                font.pixelSize: VLCStyle.fontSize_small
                elide: Text.ElideRight
                width: Math.max(0, parent.width - VLCStyle.dp(150))
            }

            /* Stage view controls: zoom in/out, percentage resets, drag pans. */
            Row {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.dp(3)

                PadButton {
                    width: VLCStyle.dp(22)
                    height: VLCStyle.dp(22)
                    text: "−"
                    enabledAction: stageFrame.stageZoom > 1.001
                    onClicked: stageFrame.setStageZoom(
                                   stageFrame.stageZoom - 0.25)
                }
                PadButton {
                    width: VLCStyle.dp(48)
                    height: VLCStyle.dp(22)
                    text: Math.round(stageFrame.stageZoom * 100) + "%"
                    onClicked: stageFrame.resetStageView()
                    HoverHandler { id: zoomHover }
                    ToolTip.visible: zoomHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Wheel to zoom · drag the stage to pan · click to reset")
                }
                PadButton {
                    width: VLCStyle.dp(22)
                    height: VLCStyle.dp(22)
                    text: "+"
                    enabledAction: stageFrame.stageZoom < 2.999
                    onClicked: stageFrame.setStageZoom(
                                   stageFrame.stageZoom + 0.25)
                }
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
            Layout.preferredWidth: Math.min(stageFrame.width, VLCStyle.dp(190))
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

            PadButton {
                id: holdButton
                Layout.fillWidth: true
                text: qsTr("Hold")
                selected: root.creative.holdActive
                enabledAction: root.creative.hasSubject
                onHeld: (down) => root.creative.setHold(down)
            }

            PadButton {
                Layout.fillWidth: true
                text: root.creative.recording ? qsTr("Stop %1s").arg(root.creative.recordSeconds.toFixed(1)) : qsTr("Record")
                selected: root.creative.recording
                enabledAction: root.creative.hasSubject
                onClicked: root.creative.toggleRecord()
            }

            PadButton {
                Layout.fillWidth: true
                text: root.creative.replaying ? qsTr("Stop loop") : qsTr("Loop")
                selected: root.creative.replaying
                enabledAction: root.creative.hasRecording || root.creative.replaying
                onClicked: root.creative.toggleReplay()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            PadButton {
                Layout.fillWidth: true
                text: qsTr("Surprise")
                onClicked: root.creative.surprise()
            }

            PadButton {
                Layout.fillWidth: true
                text: qsTr("Reseed")
                onClicked: root.creative.reseed()
            }

            PadButton {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(40)
                text: root.creative.committed ? qsTr("Committed")
                                              : qsTr("Make it real")
                primary: true
                enabledAction: root.creative.canCommit
                onClicked: root.creative.commit()
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
                PadButton {
                    text: qsTr("Add")
                    primary: true
                    onClicked: root.creative.acceptSuggestion()
                }
                PadButton {
                    text: qsTr("Not now")
                    onClicked: root.creative.dismissSuggestion()
                }
            }
        }

        /* --------------------------------------------------- recent looks */
        RowLayout {
            Layout.fillWidth: true
            visible: root.creative.history.length > 0
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: qsTr("Recent")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.dp(9)
                font.letterSpacing: 0.8
            }

            ListView {
                Layout.fillWidth: true
                height: VLCStyle.dp(28)
                orientation: ListView.Horizontal
                spacing: VLCStyle.margin_xsmall
                clip: true
                model: root.creative.history

                delegate: Rectangle {
                    required property var modelData
                    width: Math.max(VLCStyle.dp(92), historyLabel.implicitWidth
                                    + VLCStyle.margin_normal + VLCStyle.dp(24))
                    height: VLCStyle.dp(24)
                    radius: VLCStyle.dp(7)
                    color: "#12141F"
                    border.width: 1
                    border.color: Qt.rgba(root.accent.r, root.accent.g,
                                          root.accent.b, 0.28)

                    Widgets.LabelExt {
                        id: historyLabel
                        anchors.left: parent.left
                        anchors.leftMargin: VLCStyle.dp(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label ? modelData.label
                                              : modelData.patchName
                        color: "#E7E9F2"
                        font.pixelSize: VLCStyle.fontSize_small
                        elide: Text.ElideRight
                        width: parent.width - VLCStyle.dp(26)
                    }

                    Rectangle {
                        width: VLCStyle.dp(18)
                        height: width
                        radius: width / 2
                        anchors.right: parent.right
                        anchors.rightMargin: VLCStyle.dp(4)
                        anchors.verticalCenter: parent.verticalCenter
                        color: deleteHover.hovered ? "#3A2030" : "transparent"
                        Widgets.LabelExt {
                            anchors.centerIn: parent
                            text: "×"
                            color: "#A9ADBE"
                            font.pixelSize: VLCStyle.dp(11)
                        }
                        HoverHandler { id: deleteHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: PaintbrushCtx.deleteCreativeGroup(
                                           modelData.group)
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        anchors.rightMargin: VLCStyle.dp(24)
                        cursorShape: Qt.PointingHandCursor
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                root.pendingGroup = modelData.group
                                renameField.text = modelData.label
                                                   ? modelData.label
                                                   : modelData.patchName
                                renamePopup.open()
                            } else {
                                PaintbrushCtx.resumeCreativeLook(
                                            modelData.layerIndex)
                            }
                        }
                    }
                    HoverHandler { id: historyHover }
                    ToolTip.visible: historyHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("Click to resume · right-click to rename")
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

        /* ------------------------------------------------------ patch rail */
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(56)
            visible: !root.creative.suggestionVisible
            orientation: ListView.Horizontal
            spacing: VLCStyle.margin_xsmall
            clip: true
            model: root.creative.patchList

            delegate: Rectangle {
                id: chip
                required property var modelData
                readonly property bool active: modelData.id === root.creative.activePatchId
                width: Math.max(VLCStyle.dp(92), chipLabel.implicitWidth + VLCStyle.margin_normal * 2)
                height: VLCStyle.dp(46)
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
    }

    /* Rename a committed look (right-click a recent-look chip). */
    Popup {
        id: renamePopup
        parent: root
        modal: true
        width: Math.min(root.width - VLCStyle.margin_normal * 2,
                        VLCStyle.dp(300))
        height: VLCStyle.dp(150)
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color: "#11131E"
            radius: VLCStyle.dp(12)
            border.width: 1
            border.color: Qt.rgba(root.accent.r, root.accent.g,
                                  root.accent.b, 0.45)
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: VLCStyle.margin_normal
            spacing: VLCStyle.margin_small

            Widgets.LabelExt {
                Layout.fillWidth: true
                text: qsTr("Name this look")
                color: "#FFFFFF"
                font.pixelSize: VLCStyle.fontSize_large
            }

            TextField {
                id: renameField
                Layout.fillWidth: true
                placeholderText: qsTr("Look name")
                color: "#F2F3F9"
                selectByMouse: true
                onAccepted: {
                    PaintbrushCtx.renameCreativeGroup(root.pendingGroup,
                                                      text)
                    renamePopup.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PadButton {
                    text: qsTr("Cancel")
                    onClicked: renamePopup.close()
                }
                PadButton {
                    text: qsTr("Save")
                    primary: true
                    onClicked: {
                        PaintbrushCtx.renameCreativeGroup(root.pendingGroup,
                                                          renameField.text)
                        renamePopup.close()
                    }
                }
            }
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
