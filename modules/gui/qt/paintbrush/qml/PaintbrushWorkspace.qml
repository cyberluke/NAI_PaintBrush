/*****************************************************************************
 * PaintbrushWorkspace.qml : NAI Paintbrush — main creative workspace
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs

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
                text: VLCIcons.ic_fluent_document_add_24_regular
                description: qsTr("Save project")
                enabled: PaintbrushCtx.hasOpenDocument
                onClicked: saveProjectDialog.open()
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
        height: visible ? VLCStyle.dp(140, VLCStyle.scale) : 0
        visible: PaintbrushCtx.playerHasVideo
    }

    /* first-run experience */
    PaintbrushFirstLaunch {
        anchors.fill: mainRow
        visible: PaintbrushCtx.welcomeVisible
        z: 10
    }
}