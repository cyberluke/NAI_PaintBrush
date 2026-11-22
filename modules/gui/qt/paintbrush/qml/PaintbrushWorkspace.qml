/*****************************************************************************
 * PaintbrushWorkspace.qml : NAI Paintbrush — main creative workspace
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The NAI Paintbrush workspace — canvas-first (A35).
 *
 * Layout budget (startup state: contextual inspector drawer and companion
 * pill both collapsed):
 *   canvas width = W - toolRail = W - 92dp
 *   => canvas >= 0.75 * W for any W >= 368dp (and the tool rail is the only
 *      fixed-width chrome in the row).
 * The inspector is a contextual drawer anchored to the right edge that is
 * closed by default and opens only when the active selection needs it;
 * the companion is a small avatar pill, collapsed by default, that never
 * occupies layout width. Top bar (Media/AI/Save actions), iconic tool rail,
 * central canvas and bottom palette strip complete the surface. */
Item {
    id: root

    Accessible.role: Accessible.Client
    Accessible.name: qsTr("NAI Paintbrush")

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    /* The video output is a native window layered behind this (transparent)
     * QML view. Instead of a full-cover opaque background (which would hide
     * the video completely), the chrome elements below each paint their own
     * opaque background; the central canvas area stays transparent so the
     * playing video is visible there as the frame source/desktop while the
     * tool rail, palette, docks and media drawer remain readable. */

    /* Client-side decoration is the default on Windows (frameless window):
     * the workspace top bar must provide the drag surface and the window
     * control buttons itself. */
    property bool _showCSD: MainCtx.clientSideDecoration
        && !(MainCtx.intfMainWindow.visibility === Window.FullScreen)

    /* the single canvas instance shared by all docks */
    PaintbrushCanvas {
        id: paintbrushCanvas
        visible: false
        Component.onCompleted: PaintbrushCtx.setCanvasItem(paintbrushCanvas)
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

    FileDialog {
        id: openVideoDialog
        title: qsTr("Open a video")
        nameFilters: ["Video files (*.mp4 *.mkv *.avi *.mov *.webm *.ts *.m4v)", "All files (*)"]
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.openVideoFile(selectedFile)
        }
    }

    FileDialog {
        id: saveProjectDialog
        title: qsTr("Save Paintbrush project")
        fileMode: FileDialog.SaveFile
        nameFilters: ["Paintbrush project (*.npb)", "All files (*)"]
        defaultSuffix: "npb"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.saveProject(selectedFile)
        }
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export image")
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)", "JPEG image (*.jpg)", "WebP image (*.webp)"]
        defaultSuffix: "png"
        property string exportFormat: "png"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportImage(selectedFile, exportFormat)
        }
    }

    /* Living Poster: 3 s living poster — a PNG frame sequence written into
     * the chosen folder (paper on: document paper; paper off: transparent). */
    FileDialog {
        id: exportLivingDialog
        title: qsTr("Export Living Poster frames (3 s, 24 fps)")
        fileMode: FileDialog.Directory
        onAccepted: {
            if (currentFolder)
                PaintbrushCtx.exportLivingPoster(currentFolder, 3000, 24, true)
        }
    }

    /* Living Showroom: REAL muxed video over the canonical frames. The
     * buttons stay disabled until the mux pipeline is verified present
     * (ffmpeg + encoder) — no fake exports. */
    FileDialog {
        id: exportLivingMp4Dialog
        title: qsTr("Export Living Poster video (MP4/H.264)")
        fileMode: FileDialog.SaveFile
        nameFilters: ["MP4 video (*.mp4)"]
        defaultSuffix: "mp4"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportLivingVideo(currentFolder, 3000, 24, "mp4", selectedFile)
        }
    }
    FileDialog {
        id: exportLivingWebmDialog
        title: qsTr("Export Living Poster video (WebM/VP9, transparent)")
        fileMode: FileDialog.SaveFile
        nameFilters: ["WebM video (*.webm)"]
        defaultSuffix: "webm"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportLivingVideo(currentFolder, 3000, 24, "webm", selectedFile)
        }
    }

    FileDialog {
        id: exportObjectDialog
        title: qsTr("Export selected object")
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)", "All files (*)"]
        defaultSuffix: "png"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportObjectPng(selectedFile)
        }
    }

    Dialog {
        id: projectInfoDialog
        title: qsTr("Project info")
        modal: true
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(520, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Close

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small

            Widgets.LabelExt {
                Layout.fillWidth: true
                text: PaintbrushCtx.projectInfoText
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                wrapMode: Text.WordWrap
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Relink missing media…")
                onClicked: relinkMediaDialog.open()
            }
        }
    }

    FileDialog {
        id: relinkMediaDialog
        title: qsTr("Relink source media")
        nameFilters: ["Video files (*.mp4 *.mkv *.avi *.mov *.webm *.ts)", "All files (*)"]
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.relinkMedia(selectedFile)
        }
    }

    /* NAI Settings — separate from VLC Preferences; opened from the gear */
    PaintbrushSettingsDialog {
        id: settingsDialog
    }
    Connections {
        target: inspectorDock
        function onRequestSettings() { settingsDialog.open() }
    }
    /* After a successful "Add frame to canvas", pin the Inspector open and
     * make the Object Select / ML surface the user's next step, instead of
     * leaving them to hunt for the machine-learning features. */
    Connections {
        target: PaintbrushCtx
        function onFrameAddedToCanvas() {
            inspectorDock.setOpen(true)
        }
    }

    /* small top bar: workspace title + back to VLC + quick actions */
    Rectangle {
        id: topBar

        height: VLCStyle.dp(44, VLCStyle.scale)
        width: parent.width
        color: theme.bg.primary

        /* drag surface: move the window, double-click to maximize,
         * right-click for the window menu (CSD mode only) */
        Loader {
            anchors.fill: parent
            z: -1
            active: root._showCSD
            sourceComponent: Widgets.CSDTitlebarTapNDrapHandler {
            }
        }

        /* the CSD window control buttons sit on the right of the top bar;
         * the workspace action buttons make room for them */
        Loader {
            id: csdButtons

            anchors.right: parent.right
            height: parent.height
            active: root._showCSD
            source: VLCStyle.palette.hasCSDImage
                      ? "qrc:///qt/qml/VLC/Widgets/CSDThemeButtonSet.qml"
                      : "qrc:///qt/qml/VLC/Widgets/CSDWindowButtonSet.qml"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: VLCStyle.margin_normal
            anchors.rightMargin: VLCStyle.margin_normal
                + (csdButtons.active ? csdButtons.width : 0)
            spacing: VLCStyle.margin_small

            Widgets.IconLabel {
                text: VLCIcons.topbar_discover
                font.pixelSize: VLCStyle.fontSize_xxlarge
                color: theme.accent
            }

            Widgets.LabelExt {
                text: qsTr("NAI Paintbrush")
                font.pixelSize: VLCStyle.fontSize_large
                color: theme.fg.primary
            }

            Item { Layout.fillWidth: true }

            Widgets.IconToolButton {
                text: VLCIcons.info
                description: assistantDock.expanded ? qsTr("Hide companion")
                                                    : qsTr("Show companion")
                checkable: true
                checked: assistantDock.expanded
                onClicked: assistantDock.expanded = !assistantDock.expanded
            }
            /* explicit Inspector toggle: opens/closes the contextual drawer
             * regardless of the current selection state */
            Widgets.IconToolButton {
                text: VLCIcons.list
                description: inspectorDock.expanded ? qsTr("Hide inspector")
                                                    : qsTr("Show inspector")
                checkable: true
                checked: inspectorDock.expanded
                onClicked: inspectorDock.setOpen(!inspectorDock.expanded)
            }
            Widgets.IconToolButton {
                text: VLCIcons.add
                description: qsTr("New blank canvas")
                onClicked: {
                    PaintbrushCtx.newBlankCanvas(1280, 720)
                    PaintbrushCtx.companionNotifyEvent("new-canvas")
                }
            }
            Widgets.IconToolButton {
                text: VLCIcons.playlist
                description: qsTr("Open image…")
                onClicked: openImageDialog.open()
            }
            Widgets.IconToolButton {
                text: VLCIcons.topbar_video
                description: qsTr("Open video…")
                onClicked: openVideoDialog.open()
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_document_add_24_regular
                description: qsTr("Save project")
                enabled: PaintbrushCtx.hasOpenDocument
                onClicked: saveProjectDialog.open()
            }
            Widgets.IconToolButton {
                text: VLCIcons.info
                description: qsTr("Project info")
                enabled: PaintbrushCtx.hasOpenDocument
                onClicked: projectInfoDialog.open()
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_arrow_download_24_regular
                description: qsTr("Export image…")
                enabled: PaintbrushCtx.hasOpenDocument
                onClicked: exportDialog.open()
            }
            /* Living Poster: ONE dominant choice — MP4/H.264, share-ready.
             * WebM alpha and the canonical PNG frames live behind the
             * advanced menu (never offered until the mux pipeline is
             * verified present — no fake exports). */
            Widgets.IconToolButton {
                id: livingExportPrimary
                text: VLCIcons.topbar_video
                description: qsTr("Export Living Poster video (MP4)")
                enabled: PaintbrushCtx.hasOpenDocument && PaintbrushCtx.riggedAssetCount() > 0 &&
                         PaintbrushCtx.canExportLivingVideo("mp4")
                onClicked: exportLivingMp4Dialog.open()
            }
            Widgets.IconToolButton {
                id: livingExportAdvanced
                text: VLCIcons.ic_fluent_arrow_sync_24_regular
                description: qsTr("Living Poster export options…")
                enabled: PaintbrushCtx.hasOpenDocument && PaintbrushCtx.riggedAssetCount() > 0
                onClicked: livingExportMenu.popup(livingExportAdvanced, 0, livingExportAdvanced.height)
            }
            Menu {
                id: livingExportMenu
                MenuItem {
                    text: qsTr("Living Poster video (WebM, transparent)")
                    enabled: PaintbrushCtx.canExportLivingVideo("webm")
                    onTriggered: exportLivingWebmDialog.open()
                }
                MenuItem {
                    text: qsTr("Living Poster frames (PNG sequence, 3 s)")
                    onTriggered: exportLivingDialog.open()
                }
            }
            /* NAI Settings: the gear opens OUR settings, not VLC Preferences.
             * Diagnostics is one of its sections (A37/S5 — no permanent
             * status dots in the creative space). */
            Widgets.IconToolButton {
                text: VLCIcons.settings
                description: qsTr("NAI Settings")
                onClicked: settingsDialog.open()
            }
            Widgets.IconToolButton {
                text: VLCIcons.history
                description: qsTr("Back to VLC")
                onClicked: {
                    MainCtx.paintbrushView = false
                    MainCtx.playerView = PaintbrushCtx.playerHasVideo
                }
            }
        }
    }

    /* ---- BOTTOM CHROME -------------------------------------------------
     * ONE surface, owned by PaintbrushStrip. The container is exactly as tall
     * as the strip's real control row, so the canvas ends where the chrome
     * starts and no empty band is reserved under it.
     *     canvas / mainRow  { bottom = bottomChrome.top }
     *     ---------------
     *     the adaptive strip { at the window bottom }
     *     ---------------
     *      window bottom */
    Item {
        id: bottomChrome
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: bottomStrip.implicitHeight

        /* Parameters flex, colour is intrinsic and collapses entirely for
         * colour-independent tools, and Keep appears only for tools that own
         * a primary action. The constant height means tool switching cannot
         * reflow the canvas. */
        PaintbrushStrip {
            id: bottomStrip
            anchors.fill: parent
            canvas: paintbrushCanvas
        }
    }

    /* center: canvas with tool rail; the inspector drawer and the companion
     * pill float above the canvas and never steal layout width when closed */
    RowLayout {
        id: mainRow

        anchors.top: topBar.bottom
        anchors.bottom: bottomChrome.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        PaintbrushToolRail {
            id: toolRail
            /* the only fixed-width chrome in the row; canvas gets the rest */
            Layout.preferredWidth: VLCStyle.dp(92, VLCStyle.scale)
            Layout.fillHeight: true
            canvas: paintbrushCanvas
        }

        Item {
            id: canvasHost
            Layout.fillWidth: true
            Layout.fillHeight: true
            /* clip so panned/zoomed canvas content can never spill over the
             * tool rail or the top bar (presentation split / z-order fix) */
            clip: true

            PaintbrushCanvasView {
                id: canvasView
                anchors.fill: parent
                canvas: paintbrushCanvas
            }

            /* No floating product verbs over the canvas: Keep, Mood, Boost,
             * Undo Boost and Cancel live in the bottom strip, which is the
             * single contextual surface. The canvas stays fully visible and
             * pointer-transparent except for the artwork itself. */
        }
    }

    /* Harvest Tray sits between the canvas row and the palette bar; it
     * collapses to nothing while empty (spec §5.4) */
    PaintbrushHarvestTray {
        id: harvestTray
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: textEditorBar.visible ? textEditorBar.top : bottomChrome.top
        height: implicitHeight
        canvas: paintbrushCanvas
        dropTarget: canvasView
        z: 3
    }

    /* editable typography (spec §57): inline bar for the current text layer
     * (word, role, size, tracking, color) — no dialogs. Appears above the
     * palette only when the current layer is text; double-clicking a text
     * layer on the canvas selects it and focuses the word field. */
    PaintbrushTextEditorBar {
        id: textEditorBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: bottomChrome.top
        canvas: paintbrushCanvas
        z: 4

        Connections {
            target: paintbrushCanvas
            function onTextLayerDoubleClicked(layerIndex) {
                /* the canvas already selected the layer; open + focus */
                textEditorBar.refreshTreatment()
                textEditorBar.openEditor()
            }
        }
    }

    /* contextual inspector drawer: closed by default, opens when the active
     * selection needs it; floats over the right edge of the canvas row */
    PaintbrushInspectorDock {
        id: inspectorDock

        anchors.top: mainRow.top
        anchors.bottom: mainRow.bottom
        anchors.right: parent.right
        width: inspectorDock.expanded ? inspectorDock.drawerWidth : 0
        clip: true
        canvas: paintbrushCanvas
    }

    /* companion pill: a small avatar, collapsed by default (A41); the chat
     * expands in place and never occupies layout width */
    PaintbrushAssistantDock {
        id: assistantDock

        anchors.right: parent.right
        anchors.bottom: bottomChrome.top
        anchors.rightMargin: VLCStyle.margin_normal
        anchors.bottomMargin: VLCStyle.margin_normal
        z: 5
    }

    /* media drawer */
    PaintbrushMediaDrawer {
        id: mediaDrawer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: bottomChrome.top
        anchors.bottomMargin: VLCStyle.margin_xsmall
        z: 6
        /* size to the content (transport + media + tracked lane + timeline)
         * instead of a fixed height, so the timeline never overflows */
        height: visible ? implicitHeight : 0
        visible: PaintbrushCtx.playerHasVideo
    }

    /* ---- REFERENCE MONITOR (VLC VideoSurface + professional transport) ----
     * A source-monitor surface, not a thumbnail: matte bezel, technical
     * readout (title, coded resolution, fps), a large 16:9 picture, frame
     * timecode, full scrub, transport, resize grip, theater expand, and a
     * close that STOPS the video and releases the output. The canvas and
     * artwork stay loaded after close. */
    Item {
        id: videoPlayerLayer
        anchors.fill: parent
        z: 40
        visible: PaintbrushCtx.playerHasVideo

        Rectangle {
            anchors.fill: parent
            visible: playerPanel.expanded
            color: "#000000"
            opacity: 0.7
            MouseArea {
                anchors.fill: parent
                onClicked: playerPanel.expanded = false
            }
        }

        Rectangle {
            id: playerPanel

            property bool expanded: false
            property bool collapsed: false
            property real userWidth: -1

            readonly property int bezel: Math.max(1, VLCStyle.dp(1, VLCStyle.scale))
            readonly property int headerHeight: Math.round(VLCStyle.dp(38, VLCStyle.scale))
            readonly property int transportHeight: Math.round(VLCStyle.dp(52, VLCStyle.scale))
            readonly property int scrubHeight: Math.round(VLCStyle.dp(26, VLCStyle.scale))
            readonly property int statusHeight: Math.round(VLCStyle.dp(28, VLCStyle.scale))
            readonly property real defaultWidth: Math.min(VLCStyle.dp(1180, VLCStyle.scale),
                                                          Math.max(VLCStyle.dp(640, VLCStyle.scale),
                                                                   canvasHost.width * 0.62))
            readonly property real desiredWidth: expanded
                                                 ? root.width - 2 * VLCStyle.margin_large
                                                 : Math.max(VLCStyle.dp(560, VLCStyle.scale),
                                                            userWidth > 0 ? userWidth : defaultWidth)
            /* Real source aspect. A portrait clip must not get a 16:9 box:
             * that inflated the monitor and pushed its picture out of the
             * window (the black-surface regression). */
            readonly property real videoAspect: (PaintbrushCtx.playerVideoSize.width > 0
                                                 && PaintbrushCtx.playerVideoSize.height > 0)
                                                ? PaintbrushCtx.playerVideoSize.width
                                                  / PaintbrushCtx.playerVideoSize.height
                                                : 16 / 9
            /* The monitor may never cover the media drawer/timeline. */
            readonly property real bottomLimit: (mediaDrawer.visible ? mediaDrawer.y
                                                                     : bottomChrome.y)
                                                - VLCStyle.margin_xsmall
            readonly property real maxPanelHeight: Math.max(headerHeight + transportHeight
                                                            + VLCStyle.dp(140, VLCStyle.scale),
                                                            bottomLimit - y)
            readonly property real chromeHeight: headerHeight + statusHeight
                                                 + scrubHeight + transportHeight
            readonly property real maxVideoHeight: Math.max(VLCStyle.dp(120, VLCStyle.scale),
                                                            maxPanelHeight - chromeHeight)
            readonly property real widthForHeight: maxVideoHeight * videoAspect + 2 * bezel
            readonly property real panelWidth: expanded
                                               ? root.width - 2 * VLCStyle.margin_large
                                               : (userWidth > 0
                                                  ? Math.max(VLCStyle.dp(560, VLCStyle.scale),
                                                             userWidth)
                                                  : Math.min(desiredWidth, widthForHeight))
            readonly property real videoHeight: expanded
                                                ? Math.max(VLCStyle.dp(120, VLCStyle.scale),
                                                           height - chromeHeight)
                                                : Math.max(VLCStyle.dp(120, VLCStyle.scale),
                                                           Math.min((panelWidth - 2 * bezel) / videoAspect,
                                                                    maxVideoHeight))

            width: panelWidth
            height: expanded
                    ? root.height - 2 * VLCStyle.margin_large
                    : (collapsed
                       ? headerHeight + transportHeight
                       : Math.min(headerHeight + videoHeight + statusHeight
                                  + scrubHeight + transportHeight,
                                  maxPanelHeight))
            x: expanded ? VLCStyle.margin_large
                        : mainRow.x + canvasHost.x + VLCStyle.margin_normal
            y: expanded ? VLCStyle.margin_large
                        : mainRow.y + canvasHost.y + VLCStyle.margin_normal

            color: "#08080a"
            radius: Math.max(1, VLCStyle.dp(2, VLCStyle.scale))
            border.width: 1
            border.color: "#2c2c33"
            clip: true

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: mouse.accepted = true
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                /* header: source identity + technical readout */
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: playerPanel.headerHeight
                    color: "#121216"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: VLCStyle.margin_small
                        anchors.rightMargin: VLCStyle.margin_xsmall
                        spacing: VLCStyle.margin_xsmall

                        Rectangle {
                            Layout.preferredWidth: Math.round(VLCStyle.dp(3, VLCStyle.scale))
                            Layout.preferredHeight: Math.round(VLCStyle.dp(14, VLCStyle.scale))
                            color: theme.accent
                        }
                        /* source tally: green when the native video output is
                         * really attached, amber while only the track exists */
                        Rectangle {
                            Layout.preferredWidth: Math.round(VLCStyle.dp(7, VLCStyle.scale))
                            Layout.preferredHeight: Layout.preferredWidth
                            radius: width / 2
                            color: PaintbrushCtx.videoOutputAttached ? "#39d98a"
                                                                     : "#e0a23a"
                        }
                        Widgets.LabelExt {
                            text: qsTr("SOURCE")
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_small
                            font.bold: true
                            font.letterSpacing: 1.4
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: PaintbrushCtx.currentMediaTitle
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                            elide: Text.ElideRight
                        }
                        Widgets.LabelExt {
                            visible: PaintbrushCtx.playerVideoSize.width > 0
                            text: PaintbrushCtx.playerVideoSize.width + " × "
                                  + PaintbrushCtx.playerVideoSize.height
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            font.family: "Consolas"
                        }
                        Widgets.LabelExt {
                            visible: PaintbrushCtx.playerFrameRate > 0
                            text: PaintbrushCtx.playerFrameRate.toFixed(3) + " fps"
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            font.family: "Consolas"
                            font.bold: true
                        }
                        Widgets.IconToolButton {
                            text: playerPanel.expanded ? VLCIcons.ic_fluent_chevron_down_24
                                                       : VLCIcons.fullscreen
                            description: playerPanel.expanded ? qsTr("Restore monitor")
                                                              : qsTr("Theater monitor")
                            onClicked: playerPanel.expanded = !playerPanel.expanded
                        }
                        Widgets.IconToolButton {
                            text: VLCIcons.close
                            description: qsTr("Stop video and release the monitor")
                            onClicked: PaintbrushCtx.closeVideo()
                        }
                    }
                }

                /* picture: the native VLC vout inside a matte bezel */
                Item {
                    id: videoArea
                    Layout.fillWidth: true
                    Layout.fillHeight: playerPanel.expanded
                    Layout.preferredHeight: playerPanel.expanded ? -1 : playerPanel.videoHeight
                    visible: !playerPanel.collapsed
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        color: "#000000"
                    }
                    VideoSurface {
                        anchors.fill: parent
                        anchors.margins: playerPanel.bezel
                        renderingEnabled: PaintbrushCtx.playerHasVideo
                                          && !playerPanel.collapsed
                        videoSurfaceProvider: MainCtx.videoSurfaceProvider
                        visible: renderingEnabled
                    }
                }

                /* broadcast technical strip: PTS, frame index, format, fps and
                 * remaining time — the reference-monitor readout */
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: playerPanel.statusHeight
                    visible: !playerPanel.collapsed
                    color: "#0a0a0d"

                    Rectangle {
                        anchors.top: parent.top
                        width: parent.width
                        height: 1
                        color: "#1f1f25"
                    }

                    /* three equal columns keep the PTS exactly centered while
                     * the format/fps and frame/remaining readouts flank it */
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: VLCStyle.margin_small
                        anchors.rightMargin: VLCStyle.margin_small
                        spacing: 0

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Widgets.LabelExt {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                visible: PaintbrushCtx.playerVideoSize.width > 0
                                text: PaintbrushCtx.playerVideoSize.width + "×"
                                      + PaintbrushCtx.playerVideoSize.height + "   "
                                      + PaintbrushCtx.playerFrameRate.toFixed(3) + " fps"
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_xsmall
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            RowLayout {
                                anchors.centerIn: parent
                                spacing: VLCStyle.margin_xsmall
                                Widgets.LabelExt {
                                    text: qsTr("PTS")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_xsmall
                                    font.letterSpacing: 1.2
                                }
                                Widgets.LabelExt {
                                    text: root.formatPts(PaintbrushCtx.playerTimeMs)
                                    color: theme.fg.primary
                                    font.pixelSize: VLCStyle.fontSize_normal
                                    font.family: "Consolas"
                                    font.bold: true
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Widgets.LabelExt {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("FRAME ") + root.formatFrame(PaintbrushCtx.playerTimeMs)
                                      + "    " + qsTr("REMAIN ") + root.formatTimecode(
                                          Math.max(0, PaintbrushCtx.playerLengthMs
                                                      - PaintbrushCtx.playerTimeMs))
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_xsmall
                                font.family: "Consolas"
                            }
                        }
                    }
                }

                /* scrub: full-width timeline */
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: playerPanel.scrubHeight
                    visible: !playerPanel.collapsed
                    color: "#0d0d10"

                    Slider {
                        id: monitorSeek
                        anchors.fill: parent
                        anchors.leftMargin: VLCStyle.margin_small
                        anchors.rightMargin: VLCStyle.margin_small
                        from: 0
                        to: Math.max(1, PaintbrushCtx.playerLengthMs)
                        onPressedChanged: {
                            if (pressed)
                                PaintbrushCtx.playerSeekBegin()
                            else {
                                PaintbrushCtx.playerSeekEnd()
                                PaintbrushCtx.playerSeek(monitorSeek.value)
                            }
                        }
                        Binding {
                            target: monitorSeek
                            property: "value"
                            value: PaintbrushCtx.playerTimeMs
                            when: !monitorSeek.pressed
                        }
                    }
                }

                /* transport: timecode + frame-accurate controls */
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: playerPanel.transportHeight
                    color: "#121216"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: VLCStyle.margin_small
                        anchors.rightMargin: VLCStyle.margin_xsmall
                        spacing: VLCStyle.margin_xsmall

                        Widgets.LabelExt {
                            text: root.formatTimecode(PaintbrushCtx.playerTimeMs)
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_normal
                            font.family: "Consolas"
                            font.letterSpacing: 0.6
                        }
                        Widgets.LabelExt {
                            text: "/ " + root.formatTimecode(PaintbrushCtx.playerLengthMs)
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            font.family: "Consolas"
                        }

                        Item { Layout.fillWidth: true }

                        Widgets.IconToolButton {
                            text: VLCIcons.skip_back
                            description: qsTr("Go to start")
                            onClicked: PaintbrushCtx.playerSeek(0)
                        }
                        Widgets.IconToolButton {
                            text: VLCIcons.previous_frame
                            description: qsTr("Previous frame")
                            onClicked: PaintbrushCtx.playerFramePrev()
                        }
                        Widgets.IconToolButton {
                            text: PaintbrushCtx.playerIsPlaying ? VLCIcons.pause_filled
                                                               : VLCIcons.play_filled
                            description: qsTr("Play / pause")
                            font.pixelSize: Math.round(VLCStyle.dp(22, VLCStyle.scale))
                            Layout.preferredWidth: Math.round(VLCStyle.dp(40, VLCStyle.scale))
                            Layout.preferredHeight: Math.round(VLCStyle.dp(40, VLCStyle.scale))
                            onClicked: PaintbrushCtx.playerPlayPause()
                        }
                        Widgets.IconToolButton {
                            text: VLCIcons.next_frame
                            description: qsTr("Next frame")
                            onClicked: PaintbrushCtx.playerFrameNext()
                        }
                        Widgets.IconToolButton {
                            text: VLCIcons.skip_for
                            description: qsTr("Go to end")
                            onClicked: PaintbrushCtx.playerSeek(PaintbrushCtx.playerLengthMs)
                        }

                        Item { Layout.fillWidth: true }

                        Widgets.IconToolButton {
                            text: playerPanel.collapsed ? VLCIcons.ic_fluent_chevron_up_24
                                                        : VLCIcons.ic_fluent_chevron_down_24
                            description: playerPanel.collapsed ? qsTr("Show monitor")
                                                               : qsTr("Collapse monitor")
                            onClicked: playerPanel.collapsed = !playerPanel.collapsed
                        }
                    }
                }
            }

            /* resize grip: drag to size the monitor */
            Item {
                id: resizeGrip
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: Math.round(VLCStyle.dp(18, VLCStyle.scale))
                height: width
                visible: !playerPanel.expanded && !playerPanel.collapsed
                z: 5

                Canvas {
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = "#55555f"
                        ctx.lineWidth = 1.5
                        for (let i = 1; i <= 3; ++i) {
                            const o = width - i * 4
                            ctx.beginPath()
                            ctx.moveTo(o, height)
                            ctx.lineTo(width, o)
                            ctx.stroke()
                        }
                    }
                }
                DragHandler {
                    id: resizeDrag
                    target: null
                    property real grabStartWidth: 0
                    onActiveChanged: if (active)
                        grabStartWidth = playerPanel.width
                    onTranslationChanged: if (active)
                        playerPanel.userWidth = Math.max(
                            VLCStyle.dp(560, VLCStyle.scale),
                            Math.min(root.width - playerPanel.x - VLCStyle.margin_normal,
                                     grabStartWidth + activeTranslation.x))
                }
            }

            Connections {
                target: PaintbrushCtx
                function onMediaStateChanged() {
                    if (!PaintbrushCtx.playerHasVideo) {
                        playerPanel.expanded = false
                        playerPanel.collapsed = false
                        playerPanel.userWidth = -1
                    }
                }
            }
        }
    }
    /* first-run experience */
    PaintbrushFirstLaunch {
        anchors.fill: mainRow
        visible: PaintbrushCtx.welcomeVisible
        z: 10
        onOpenVideoRequested: openVideoDialog.open()
    }

    /* crash recovery banner */
    Rectangle {
        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: VLCStyle.dp(48, VLCStyle.scale)
        z: 9
        visible: PaintbrushCtx.recoveryAvailable
        color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)

        RowLayout {
            anchors.fill: parent
            anchors.margins: VLCStyle.margin_normal
            spacing: VLCStyle.margin_normal

            Widgets.LabelExt {
                Layout.fillWidth: true
                text: PaintbrushCtx.recoveryDescription
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                wrapMode: Text.WordWrap
            }

            Button {
                text: qsTr("Restore")
                onClicked: PaintbrushCtx.restoreRecovery()
            }
            Button {
                text: qsTr("Open copy…")
                onClicked: PaintbrushCtx.openRecoveryCopy()
            }
            Button {
                text: qsTr("Discard")
                onClicked: PaintbrushCtx.discardRecovery()
            }
        }
    }

    /* ---------------------------------------------------------- shortcuts */

    Shortcut { sequence: "Ctrl+N"; onActivated: { PaintbrushCtx.newBlankCanvas(1280, 720); PaintbrushCtx.companionNotifyEvent("new-canvas") } }
    Shortcut { sequence: "Ctrl+O"; onActivated: openImageDialog.open() }
    Shortcut { sequence: "Ctrl+S"; onActivated: PaintbrushCtx.autosaveNow() }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: saveProjectDialog.open() }
    Shortcut { sequence: "Ctrl+E"; onActivated: exportDialog.open() }
    Shortcut { sequence: "Ctrl+Z"; onActivated: PaintbrushCtx.undo() }
    Shortcut { sequence: "Ctrl+Y"; onActivated: PaintbrushCtx.redo() }
    Shortcut { sequence: "Ctrl+X"; onActivated: paintbrushCanvas.cutSelection() }
    Shortcut { sequence: "Ctrl+C"; onActivated: paintbrushCanvas.copySelection() }
    Shortcut { sequence: "Ctrl+V"; onActivated: paintbrushCanvas.pasteClipboard(Qt.point(0, 0)) }
    Shortcut { sequence: "Ctrl+A"; onActivated: paintbrushCanvas.selectionSelectAll() }
    Shortcut { sequence: "Ctrl+D"; onActivated: paintbrushCanvas.selectionClear() }
    Shortcut { sequence: "Delete"; onActivated: paintbrushCanvas.deleteSelection() }
    Shortcut { sequence: "B"; onActivated: paintbrushCanvas.tool = 5 }
    Shortcut { sequence: "E"; onActivated: paintbrushCanvas.tool = 6 }
    Shortcut { sequence: "O"; onActivated: paintbrushCanvas.tool = 3 }
    Shortcut { sequence: "T"; onActivated: paintbrushCanvas.tool = 14 }
    Shortcut { sequence: "P"; onActivated: paintbrushCanvas.tool = 16 }
    Shortcut { sequence: "L"; onActivated: paintbrushCanvas.tool = 17 }
    Shortcut { sequence: "Shift+L"; onActivated: paintbrushCanvas.tool = 18 }
    Shortcut { sequence: "K"; onActivated: PaintbrushCtx.keepSelection() }
    Shortcut { sequence: "Ctrl+B"; onActivated: PaintbrushCtx.boost() }
    Shortcut { sequence: "M"; onActivated: bottomStrip.toggleMoodPicker() }
    Shortcut { sequence: "G"; onActivated: paintbrushCanvas.gridVisible = !paintbrushCanvas.gridVisible }
    Shortcut { sequence: "R"; onActivated: paintbrushCanvas.rulersVisible = !paintbrushCanvas.rulersVisible }
    Shortcut { sequence: "Shift+G"; onActivated: paintbrushCanvas.snapToGrid = !paintbrushCanvas.snapToGrid }
    Shortcut { sequence: "+"; onActivated: paintbrushCanvas.zoomIn() }
    Shortcut { sequence: "-"; onActivated: paintbrushCanvas.zoomOut() }
    Shortcut { sequence: "Ctrl+0"; onActivated: paintbrushCanvas.zoomTo(1.0) }
    Shortcut { sequence: "F"; onActivated: paintbrushCanvas.fitToView() }
    Shortcut { sequence: "Space"; onActivated: { /* reserved: pan via drag */ } }
    Shortcut { sequence: "Ctrl+Left"; onActivated: PaintbrushCtx.playerFramePrev() }
    Shortcut { sequence: "Ctrl+Right"; onActivated: PaintbrushCtx.playerFrameNext() }
    Shortcut { sequence: "Ctrl+Space"; onActivated: PaintbrushCtx.playerPlayPause() }
    Shortcut { sequence: "Ctrl+U"; onActivated: PaintbrushCtx.captureVideoFrame() }
    Shortcut { sequence: "Ctrl+I"; onActivated: assistantDock.expanded = !assistantDock.expanded }
    Shortcut { sequence: "Escape"; onActivated: { PaintbrushCtx.cancelTracking(); PaintbrushCtx.cancelGeneration(); PaintbrushCtx.cancelThumbnails() } }


    /* Professional HH:MM:SS:FF timecode for the reference monitor. The frame
     * field uses the real track frame rate; 25 fps is only the fallback while
     * no video track has been probed yet. */
    function formatTimecode(ms) {
        const fps = PaintbrushCtx.playerFrameRate > 0
                    ? Math.max(1, Math.round(PaintbrushCtx.playerFrameRate))
                    : 25
        const total = Math.max(0, Math.floor(ms))
        const h = Math.floor(total / 3600000)
        const m = Math.floor((total % 3600000) / 60000)
        const s = Math.floor((total % 60000) / 1000)
        const f = Math.min(fps - 1,
                           Math.floor(((total % 1000) / 1000) * fps))
        return ("0" + h).slice(-2) + ":" + ("0" + m).slice(-2) + ":"
               + ("0" + s).slice(-2) + ":" + ("0" + f).slice(-2)
    }

    /* Raw presentation timestamp with millisecond resolution (the PTS
     * readout), independent of the frame-rounded transport timecode. */
    function formatPts(ms) {
        const total = Math.max(0, Math.floor(ms))
        const h = Math.floor(total / 3600000)
        const m = Math.floor((total % 3600000) / 60000)
        const s = Math.floor((total % 60000) / 1000)
        const millis = total % 1000
        return ("0" + h).slice(-2) + ":" + ("0" + m).slice(-2) + ":"
               + ("0" + s).slice(-2) + "." + ("00" + millis).slice(-3)
    }

    /* Absolute frame index at the current PTS, using the real track rate. */
    function formatFrame(ms) {
        const fps = PaintbrushCtx.playerFrameRate > 0
                    ? PaintbrushCtx.playerFrameRate : 25
        const frame = Math.max(0, Math.floor(ms * fps / 1000))
        return ("000000" + frame).slice(-6)
    }

    /* Fit to the desktop so the bottom chrome (palette + media drawer)
     * is never hidden behind the taskbar when the window was restored
     * larger than the current screen. Also set the product identity:
     * this is NAI PaintBrush, not a media player. The title is re-applied
     * after the native window settles so no late default can override it. */
    Component.onCompleted: {
        MainCtx.intfMainWindow.title = qsTr("NAI PaintBrush")
        Qt.callLater(() => {
            MainCtx.intfMainWindow.title = qsTr("NAI PaintBrush")
        })
        PaintbrushCtx.fitInterfaceToDesktop()
    }
}