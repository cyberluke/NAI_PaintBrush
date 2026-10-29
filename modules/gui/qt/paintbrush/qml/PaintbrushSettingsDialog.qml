/*****************************************************************************
 * PaintbrushSettingsDialog.qml : NAI Paintbrush — Settings
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

/* NAI Settings — a dedicated dialog, separate from VLC Preferences.
 * Sections: AI Models, Providers, Device, Media, Diagnostics. This is an
 * HONEST read-only view of what is actually configured at runtime (from the
 * same capability report that powers the diagnostics), so a "configured"
 * model is shown only when its backend really exists — no fake toggles or
 * supermarket switches for features without a backend. */
Dialog {
    id: root

    title: qsTr("NAI Settings")
    modal: true
    x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
    y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
    width: Overlay.overlay
           ? Math.min(Overlay.overlay.width - VLCStyle.margin_large * 2,
                      VLCStyle.dp(560, VLCStyle.scale))
           : VLCStyle.dp(560, VLCStyle.scale)

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    contentItem: ColumnLayout {
        spacing: 0

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumHeight: VLCStyle.dp(560, VLCStyle.scale)

            ColumnLayout {
                width: parent.width
                spacing: VLCStyle.margin_xsmall

                /* --------------------------------------------- AI Models */
                Widgets.LabelExt {
                    text: qsTr("AI Models")
                    color: theme.accent
                    font.pixelSize: VLCStyle.fontSize_normal
                    font.bold: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.segmentationAvailable ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Object Select (SAM3) — %1")
                                  .arg(PaintbrushCtx.segmentationStatusText)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.visionConfigured ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Ask — VLM (%1)")
                                  .arg(PaintbrushCtx.visionStatusText)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.generationConfigured ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Image generation — %1")
                                  .arg(PaintbrushCtx.generationStatusText)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.trackingAvailable ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Object tracking — %1")
                                  .arg(PaintbrushCtx.trackingUnavailableReason)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.assistantConfigured ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("AI companion — %1")
                                  .arg(PaintbrushCtx.assistantUnavailableReason)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }

                Widgets.LabelExt {
                    text: qsTr("Live subtitles and translation are planned for a later build.")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }

                /* -------------------------------------------- Providers */
                Widgets.LabelExt {
                    text: qsTr("Providers")
                    color: theme.accent
                    font.pixelSize: VLCStyle.fontSize_normal
                    font.bold: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle { width: 8; height: 8; radius: 4;
                        color: PaintbrushCtx.lookupAvailable ? "limegreen" : "#c33" }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Product lookup — %1")
                                  .arg(PaintbrushCtx.lookupUnavailableReason)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }

                /* ---------------------------------------------- Device */
                Widgets.LabelExt {
                    text: qsTr("Device")
                    color: theme.accent
                    font.pixelSize: VLCStyle.fontSize_normal
                    font.bold: true
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.segmentationAvailable
                          ? qsTr("SAM runs on: %1").arg(PaintbrushCtx.segmentationStatusText)
                          : qsTr("No local segmentation device configured.")
                    color: theme.fg.secondary
                    wrapMode: Text.WordWrap
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }

                /* ---------------------------------------------- Media */
                Widgets.LabelExt {
                    text: qsTr("Media")
                    color: theme.accent
                    font.pixelSize: VLCStyle.fontSize_normal
                    font.bold: true
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.playerHasVideo
                          ? qsTr("A video source is open (%1).").arg(PaintbrushCtx.currentMediaTitle)
                          : qsTr("No video source open.")
                    color: theme.fg.secondary
                    wrapMode: Text.WordWrap
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }

                /* ------------------------------------------ Diagnostics */
                Widgets.LabelExt {
                    text: qsTr("Diagnostics")
                    color: theme.accent
                    font.pixelSize: VLCStyle.fontSize_normal
                    font.bold: true
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.diagnosticsText
                    color: theme.fg.secondary
                    wrapMode: Text.WordWrap
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }
            }
        }

        DialogButtonBox {
            standardButtons: DialogButtonBox.Close
            Layout.fillWidth: true
        }
    }
}