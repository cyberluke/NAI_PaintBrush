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
 * Sections: AI Models, Providers, Device, Media, Diagnostics. Editable fields
 * exist ONLY for capabilities that actually have a backend (SAM model/device,
 * VLM/model, assistant/model, exact-frame decode policy); every such field
 * persists into the Paintbrush/VLC configuration layer (no second settings
 * store) and Apply hot-reconfigures what is safe or reports restart-required.
 * Capability status is honest and shows POSITIVE text when a capability is
 * green — never an "UnavailableReason" string on an available row. Secrets
 * are never displayed in clear text. */
Dialog {
    id: root

    title: qsTr("NAI Settings")
    modal: true

    function apply() {
        const note = PaintbrushCtx.applyNaiSettings(
            samModelField.text.trim(),
            samDeviceField.text.trim(),
            vlmUrlField.text.trim(),
            vlmModelField.text.trim(),
            assistantUrlField.text.trim(),
            assistantModelField.text.trim(),
            parseInt(ptsField.text, 10) || 250)
        statusLabel.text = note
        if (!PaintbrushCtx.settingsRestartRequired)
            statusLabel.color = "limegreen"
        else
            statusLabel.color = theme.accent
    }
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
                /* SAM model path + device (real, editable) */
                TextField {
                    id: samModelField
                    Layout.fillWidth: true
                    placeholderText: qsTr("SAM3 model path")
                    text: PaintbrushCtx.settingsSamModel
                }
                TextField {
                    id: samDeviceField
                    Layout.fillWidth: true
                    placeholderText: qsTr("SAM device (auto / cpu / gpu / npu)")
                    text: PaintbrushCtx.settingsSamDevice
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
                TextField {
                    id: vlmUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("VLM endpoint URL")
                    text: PaintbrushCtx.settingsVlmUrl
                }
                TextField {
                    id: vlmModelField
                    Layout.fillWidth: true
                    placeholderText: qsTr("VLM model")
                    text: PaintbrushCtx.settingsVlmModel
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
                        text: PaintbrushCtx.trackingAvailable
                              ? qsTr("Object tracking — %1").arg(PaintbrushCtx.trackingStatusText)
                              : qsTr("Object tracking — %1").arg(PaintbrushCtx.trackingUnavailableReason)
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
                        text: PaintbrushCtx.assistantConfigured
                              ? qsTr("AI companion — Ready")
                              : qsTr("AI companion — %1").arg(PaintbrushCtx.assistantUnavailableReason)
                        color: theme.fg.secondary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
                TextField {
                    id: assistantUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Assistant endpoint URL")
                    text: PaintbrushCtx.settingsAssistantUrl
                }
                TextField {
                    id: assistantModelField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Assistant model")
                    text: PaintbrushCtx.settingsAssistantModel
                }
                Widgets.LabelExt {
                    text: PaintbrushCtx.settingsSecretsPresent
                          ? qsTr("API keys are configured via environment/preferences (never shown).")
                          : qsTr("No API keys configured.")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
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
                        text: PaintbrushCtx.lookupAvailable
                              ? qsTr("Product lookup — Ready")
                              : qsTr("Product lookup — %1").arg(PaintbrushCtx.lookupUnavailableReason)
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
                          ? qsTr("SAM runtime: %1").arg(PaintbrushCtx.segmentationStatusText)
                          : qsTr("No local segmentation device configured (below).")
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
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Widgets.LabelExt {
                        text: qsTr("Exact-frame PTS tolerance (ms):")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                    TextField {
                        id: ptsField
                        Layout.preferredWidth: VLCStyle.dp(80, VLCStyle.scale)
                        text: PaintbrushCtx.settingsPtsToleranceMs
                        validator: IntValidator { bottom: 20; top: 2000 }
                        color: theme.fg.primary
                    }
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

        /* Apply status / restart-required notice */
        Widgets.LabelExt {
            id: statusLabel
            Layout.fillWidth: true
            text: PaintbrushCtx.settingsRestartRequired
                  ? qsTr("Model/endpoint changes take effect after restart.")
                  : ""
            color: theme.accent
            wrapMode: Text.WordWrap
            font.pixelSize: VLCStyle.fontSize_xsmall
        }

        DialogButtonBox {
            standardButtons: DialogButtonBox.Cancel | DialogButtonBox.Apply
            Layout.fillWidth: true
            onApplied: root.apply()
        }
    }
}