/*****************************************************************************
 * PaintbrushWorkspace.qml : NAI Paintbrush — main creative workspace
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs
import QtQuick.Controls

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The NAI Paintbrush workspace. Tool rail on the left, canvas in the
 * middle, inspector/assistant on the right, palette below, and a media
 * drawer at the bottom. Everything collapses back to a simple canvas. */
Item {
    id: root

    Accessible.role: Accessible.Client
    Accessible.name: qsTr("NAI Paintbrush")

    ColorContext {
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
                description: PaintbrushCtx.companionMode === 0 ? qsTr("Show companion")
                                                               : qsTr("Companion on")
                checkable: true
                checked: PaintbrushCtx.companionMode !== 0
                onClicked: PaintbrushCtx.setCompanionMode(checked ? 2 : 0)
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

    /* center: canvas with tool rail + right docks */
    RowLayout {
        id: mainRow

        anchors.top: topBar.bottom
        anchors.bottom: paletteBar.top
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        PaintbrushToolRail {
            id: toolRail
            Layout.preferredWidth: VLCStyle.dp(56, VLCStyle.scale)
            Layout.fillHeight: true
            canvas: paintbrushCanvas
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            PaintbrushCanvasView {
                id: canvasView
                anchors.fill: parent
                canvas: paintbrushCanvas
            }
        }

        PaintbrushInspectorDock {
            id: inspectorDock
            Layout.preferredWidth: Math.min(root.width * 0.28, VLCStyle.dp(360, VLCStyle.scale))
            Layout.fillHeight: true
            canvas: paintbrushCanvas
        }

        PaintbrushAssistantDock {
            id: assistantDock
            Layout.preferredWidth: Math.min(root.width * 0.22, VLCStyle.dp(280, VLCStyle.scale))
            Layout.fillHeight: true
            visible: PaintbrushCtx.companionMode !== 0
        }
    }

    /* palette bar */
    PaintbrushPaletteBar {
        id: paletteBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: mediaDrawer.visible ? mediaDrawer.top : parent.bottom
        height: VLCStyle.dp(64, VLCStyle.scale)
        canvas: paintbrushCanvas
    }

    /* media drawer */
    PaintbrushMediaDrawer {
        id: mediaDrawer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        /* size to the content (transport + media + tracked lane + timeline)
         * instead of a fixed height, so the timeline never overflows */
        height: visible ? implicitHeight : 0
        visible: PaintbrushCtx.playerHasVideo
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
                onClicked: saveProjectDialog.open()
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
    Shortcut { sequence: "G"; onActivated: paintbrushCanvas.gridVisible = !paintbrushCanvas.gridVisible }
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
    Shortcut { sequence: "Ctrl+I"; onActivated: PaintbrushCtx.setCompanionMode(PaintbrushCtx.companionMode === 0 ? 2 : 0) }
    Shortcut { sequence: "Escape"; onActivated: { PaintbrushCtx.cancelTracking(); PaintbrushCtx.cancelGeneration(); PaintbrushCtx.cancelThumbnails() } }
}