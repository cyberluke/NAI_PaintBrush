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
 * "use this frame", and the tracked-object lane. The drawer sizes itself
 * to its content (implicitHeight) so the thumbnail timeline never
 * overflows the panel. */
Item {
    id: root

    /* content-driven height: transport + media + tracked lane (when
     * visible) + thumbnail timeline (when a video with a known length is
     * loaded), plus margins/spacing */
    implicitHeight: contentColumn.implicitHeight

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
        id: contentColumn
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
    text: PaintbrushCtx.playerIsPlaying ? VLCIcons.pause_filled : VLCIcons.play
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

                /* While the user drags, suspend the thumbnail batch so it
                 * cannot seek the player underneath the drag (each competing
                 * precise seek flushes the decoder mid-GOP and leaves a
                 * partially-decoded frame on screen). Seeking itself is
                 * debounced so a fast drag does not fire a dozen precise
                 * seeks per second; the final position is applied on release. */
                onPressedChanged: {
                    if (pressed)
                        PaintbrushCtx.playerSeekBegin()
                    else {
                        PaintbrushCtx.playerSeekEnd()
                        PaintbrushCtx.playerSeek(seekSlider.value)
                    }
                }
onMoved: {
        /* (B) slider is delivering a value — throttled precise seek below */
        seekThrottle.restart()
    }

                Timer {
                    id: seekThrottle
                    interval: 80
                    onTriggered: PaintbrushCtx.playerSeek(seekSlider.value)
                }

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
                text: qsTr("Add frame to canvas")
                onClicked: PaintbrushCtx.captureVideoFrame()
            }

            Widgets.IconToolButton {
                description: qsTr("Close video")
                text: VLCIcons.close
                onClicked: PaintbrushCtx.closeVideo()
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
                                    font.pixelSize: VLCStyle.fontSize_xsmall
                                    elide: Text.ElideRight
                                }
                                Widgets.LabelExt {
                                    text: qsTr("%1 kf · %2 corr")
                                          .arg(modelData.keyframes)
                                          .arg(modelData.corrections)
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_xsmall
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
                            onClicked: {
                                PaintbrushCtx.setSelectedTrackId(modelData.id)
                                PaintbrushCtx.seekToTrackKeyframe(modelData.id, 0)
                            }
                        }
                    }
                }

                Widgets.IconToolButton {
                    text: VLCIcons.add
                    description: qsTr("Make animated sticker from a track")
                    visible: PaintbrushCtx.document.trackCount > 0
                    onClicked: {
                        /* A15: the sticker editor opens on the REAL selected
                         * track (chosen in the drawer lane or the editor's
                         * track picker), never a hardcoded track 0. */
                        const trackId = PaintbrushCtx.selectedTrackId
                        stickerEditor.selectTrack(trackId)
                        stickerEditor.open()
                    }
                }
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
                            font.pixelSize: VLCStyle.fontSize_xsmall
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

    /* Animated sticker editor: preview loop, trim, outline, shadow,
     * scale, speed; place a frame on the canvas or export the trimmed
     * transparent PNG sequence. Declared at the drawer root (not inside the
     * content column) so opening it never changes the drawer height. */
    Dialog {
        id: stickerEditor
        title: qsTr("Animated sticker")
        modal: true
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(520, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0

        property int trackId: 0
        property int startFrame: 0
        property int endFrame: 0
        property int previewFrame: 0

        function frameCount() { return PaintbrushCtx.stickerEditorKeyframeCount(trackId) }
        function stepPreview() {
            const n = frameCount()
            if (n <= 0) return
            if (previewFrame > endFrame || previewFrame < startFrame)
                previewFrame = startFrame
            else
                previewFrame = (previewFrame + 1 <= endFrame) ? previewFrame + 1 : startFrame
        }

        /* A15: switch the editor to `trackIdx` — a real selected track,
         * synced into the controller's SelectedTrack state and back into
         * this dialog (trim range, preview frame, effect controls). */
        function selectTrack(trackIdx) {
            if (trackIdx < 0 || frameCountOf(trackIdx) <= 0)
                return
            PaintbrushCtx.setSelectedTrackId(trackIdx)
            trackId = trackIdx
            startFrame = 0
            endFrame = Math.max(0, frameCountOf(trackIdx) - 1)
            previewFrame = 0
            effectCombo.currentIndex = PaintbrushCtx.trackEffectType(trackIdx)
            effectIntensitySpin.value = PaintbrushCtx.trackEffectIntensity(trackIdx)
            effectColorField.text = PaintbrushCtx.trackEffectColor(trackIdx)
        }

        function frameCountOf(id) { return PaintbrushCtx.stickerEditorKeyframeCount(id) }

        onOpened: {
            /* bind to the controller's SelectedTrack (never hardcoded 0) */
            selectTrack(PaintbrushCtx.selectedTrackId)
            for (let i = 0; i < trackCombo.count; ++i) {
                if (trackCombo.model[i].id === PaintbrushCtx.selectedTrackId) {
                    trackCombo.currentIndex = i
                    break
                }
            }
            previewTimer.start()
        }
        onClosed: previewTimer.stop()

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: VLCStyle.margin_normal
            spacing: VLCStyle.margin_small

            /* real track selection (A15): the user picks WHICH tracked
             * object becomes a sticker — bound to the controller's
             * SelectedTrack state, never a hardcoded track 0. */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                Widgets.LabelExt { text: qsTr("Track"); color: theme.fg.secondary }
                Widgets.ComboBoxExt {
                    id: trackCombo
                    Layout.fillWidth: true
                    model: PaintbrushCtx.trackList
                    textRole: "label"
                    valueRole: "id"
                    onActivated: {
                        if (stickerEditor.opened)
                            stickerEditor.selectTrack(trackCombo.currentValue)
                    }
                }
            }

            /* preview loop */
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(140, VLCStyle.scale)
                color: Qt.rgba(theme.bg.primary.r, theme.bg.primary.g, theme.bg.primary.b, 0.6)
                border.width: 1
                border.color: theme.border
                radius: VLCStyle.margin_xsmall

                Image {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 20, parent.height - 20)
                    height: parent.height - 20
                    fillMode: Image.PreserveAspectFit
                    source: stickerEditor.frameCount() > 0
                            ? PaintbrushCtx.trackEffectPreviewDataUrl(
                                  stickerEditor.trackId, stickerEditor.previewFrame,
                                  outlineSpin.value, outlineColorField.text,
                                  shadowCheck.checked, scaleSpin.value / 100.0)
                            : ""
                    asynchronous: true
                }
                Widgets.LabelExt {
                    anchors.bottom: parent.bottom
                    anchors.right: parent.right
                    anchors.margins: VLCStyle.margin_xsmall
                    text: stickerEditor.frameCount() > 0
                          ? qsTr("%1 / %2").arg(stickerEditor.previewFrame + 1)
                                            .arg(stickerEditor.frameCount())
                          : qsTr("No keyframes")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }
            }
            Timer {
                id: previewTimer
                interval: 120
                repeat: true
                running: stickerEditor.visible
                onTriggered: stickerEditor.stepPreview()
            }

            /* trim range */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                Widgets.LabelExt { text: qsTr("Trim"); color: theme.fg.secondary }
                Widgets.SpinBoxExt {
                    id: trimStartSpin
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(0, stickerEditor.frameCount() - 1)
                    value: stickerEditor.startFrame
                    onValueModified: stickerEditor.startFrame = value
                }
                Widgets.LabelExt { text: qsTr("–"); color: theme.fg.secondary }
                Widgets.SpinBoxExt {
                    id: trimEndSpin
                    Layout.fillWidth: true
                    from: trimStartSpin.value
                    to: Math.max(trimStartSpin.value, stickerEditor.frameCount() - 1)
                    value: stickerEditor.endFrame
                    onValueModified: stickerEditor.endFrame = value
                }
                Widgets.LabelExt {
                    text: qsTr("of %1").arg(stickerEditor.frameCount())
                    color: theme.fg.secondary
                }
            }

            /* outline */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                Widgets.LabelExt { text: qsTr("Outline"); color: theme.fg.secondary }
                Widgets.SpinBoxExt {
                    id: outlineSpin
                    Layout.preferredWidth: VLCStyle.dp(70, VLCStyle.scale)
                    from: 0
                    to: 20
                    value: 3
                }
                TextField {
                    id: outlineColorField
                    Layout.preferredWidth: VLCStyle.dp(90, VLCStyle.scale)
                    text: "#FFFFFF"
                    placeholderText: qsTr("color")
                }
                Widgets.LabelExt { text: qsTr("Shadow"); color: theme.fg.secondary }
                Widgets.CheckButton {
                    id: shadowCheck
                    checked: false
                }
                Widgets.LabelExt { text: qsTr("Scale"); color: theme.fg.secondary }
                Widgets.SpinBoxExt {
                    id: scaleSpin
                    Layout.preferredWidth: VLCStyle.dp(70, VLCStyle.scale)
                    from: 10
                    to: 400
                    value: 100
                }
                Widgets.LabelExt { text: qsTr("Speed"); color: theme.fg.secondary }
                Widgets.SpinBoxExt {
                    id: speedSpin
                    Layout.preferredWidth: VLCStyle.dp(60, VLCStyle.scale)
                    from: 1
                    to: 8
                    value: 1
                }
            }

            /* track effect (blur / dim / spotlight / outline / attach) */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                Widgets.LabelExt { text: qsTr("Effect"); color: theme.fg.secondary }
                Widgets.ComboBoxExt {
                    id: effectCombo
                    Layout.fillWidth: true
                    model: [
                        { text: qsTr("None"), value: 0 },
                        { text: qsTr("Blur"), value: 1 },
                        { text: qsTr("Dim"), value: 2 },
                        { text: qsTr("Spotlight"), value: 3 },
                        { text: qsTr("Outline"), value: 4 },
                        { text: qsTr("Attach to frame"), value: 5 }
                    ]
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: 0
                    onCurrentIndexChanged: {
                        if (stickerEditor.opened && stickerEditor.trackId >= 0)
                            PaintbrushCtx.setTrackEffect(
                                stickerEditor.trackId, effectCombo.currentValue,
                                effectIntensitySpin.value, effectColorField.text)
                    }
                }
                Widgets.SpinBoxExt {
                    id: effectIntensitySpin
                    Layout.preferredWidth: VLCStyle.dp(70, VLCStyle.scale)
                    from: 0
                    to: 100
                    value: PaintbrushCtx.trackEffectIntensity(stickerEditor.trackId)
                    onValueModified: {
                        if (stickerEditor.opened && stickerEditor.trackId >= 0)
                            PaintbrushCtx.setTrackEffect(
                                stickerEditor.trackId, effectCombo.currentValue,
                                value, effectColorField.text)
                    }
                }
                TextField {
                    id: effectColorField
                    Layout.preferredWidth: VLCStyle.dp(90, VLCStyle.scale)
                    text: PaintbrushCtx.trackEffectColor(stickerEditor.trackId)
                    placeholderText: qsTr("color")
                    onEditingFinished: {
                        if (stickerEditor.opened && stickerEditor.trackId >= 0)
                            PaintbrushCtx.setTrackEffect(
                                stickerEditor.trackId, effectCombo.currentValue,
                                effectIntensitySpin.value, text)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Widgets.ActionButtonPrimary {
                    Layout.fillWidth: true
                    text: qsTr("Place frame on canvas")
                    enabled: stickerEditor.frameCount() > 0
                    onClicked: {
                        PaintbrushCtx.placeStickerFrame(
                            stickerEditor.trackId, stickerEditor.previewFrame,
                            outlineSpin.value, outlineColorField.text,
                            shadowCheck.checked, scaleSpin.value / 100.0)
                    }
                }
                Widgets.ActionButtonPrimary {
                    Layout.fillWidth: true
                    text: qsTr("Export sequence…")
                    enabled: stickerEditor.frameCount() > 0
                    onClicked: stickerDirDialog.open()
                }
            }
        }
    }

    FolderDialog {
        id: stickerDirDialog
        title: qsTr("Choose a folder for the animated sticker sequence")
        onAccepted: {
            if (currentFolder)
                PaintbrushCtx.exportTrackedStickerOptions(
                    stickerEditor.trackId, currentFolder,
                    trimStartSpin.value, trimEndSpin.value,
                    outlineSpin.value, outlineColorField.text,
                    shadowCheck.checked, scaleSpin.value / 100.0,
                    speedSpin.value)
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