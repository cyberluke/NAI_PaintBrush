/*****************************************************************************
 * PaintbrushSettingsDialog.qml : NAI Paintbrush — Settings
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* NAI Settings — production surface.
 *
 * Contract:
 *  - The dialog edits a DRAFT. Nothing is persisted until Apply.
 *  - Cancel (and closing the dialog) discards the draft and restores the
 *    controller's current values, so the two buttons always mean something.
 *  - Apply calls PaintbrushCtx.applyNaiSettings(...) exactly once, reports the
 *    controller's answer inline, and closes only when it applied cleanly.
 *  - Every row states what the capability actually is; green rows show
 *    positive text, never an "unavailable reason".
 *
 * Layout: one scrollable column of cards + a sticky footer, so the actions
 * never scroll away and the content never overflows the window.
 */
Dialog {
    id: root

    title: qsTr("NAI Settings")
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 0

    width: Math.min(VLCStyle.dp(680, VLCStyle.scale),
                    (parent ? parent.width : 900) - VLCStyle.margin_normal * 2)
    height: Math.min(VLCStyle.dp(720, VLCStyle.scale),
                     (parent ? parent.height : 800) - VLCStyle.margin_normal * 2)

    /* ---- draft state ------------------------------------------------- */
    property string draftSamModel: ""
    property string draftSamDevice: ""
    property string draftVlmUrl: ""
    property string draftVlmModel: ""
    property string draftAssistantUrl: ""
    property string draftAssistantModel: ""
    property int draftPtsToleranceMs: 250
    property string applyMessage: ""

    function resetDraft() {
        draftSamModel = PaintbrushCtx.settingsSamModel
        draftSamDevice = PaintbrushCtx.settingsSamDevice
        draftVlmUrl = PaintbrushCtx.settingsVlmUrl
        draftVlmModel = PaintbrushCtx.settingsVlmModel
        draftAssistantUrl = PaintbrushCtx.settingsAssistantUrl
        draftAssistantModel = PaintbrushCtx.settingsAssistantModel
        draftPtsToleranceMs = PaintbrushCtx.settingsPtsToleranceMs
        applyMessage = ""
    }
    function apply() {
        const answer = PaintbrushCtx.applyNaiSettings(
                    draftSamModel.trim(),
                    draftSamDevice.trim(),
                    draftVlmUrl.trim(),
                    draftVlmModel.trim(),
                    draftAssistantUrl.trim(),
                    draftAssistantModel.trim(),
                    draftPtsToleranceMs)
        if (answer && answer.length > 0) {
            applyMessage = answer          /* keep open: the user must see it */
            return false
        }
        applyMessage = ""
        close()
        return true
    }

    onOpened: resetDraft()

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    /* NOTE: do NOT override `background` with theme.* here. A Dialog's
     * background is instantiated in its own component scope; the ColorContext
     * id does not resolve there and every colour came out as the magenta
     * "unresolved" marker. VLC's default dialog surface is correct — use it. */
    background: Rectangle {
        color: "#141018"
        radius: VLCStyle.margin_small
        border.width: 1
        border.color: "#2a2430"
    }

    /* ================================================================== */
    contentItem: ColumnLayout {
        spacing: 0

        /* ---- header --------------------------------------------------- */
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + VLCStyle.margin_normal * 2
            color: "transparent"

            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: VLCStyle.margin_normal
                anchors.rightMargin: VLCStyle.margin_normal
                spacing: VLCStyle.margin_small

                Widgets.LabelExt {
                    text: qsTr("Settings")
                    color: theme.fg.primary
                    font.pixelSize: VLCStyle.fontSize_xxlarge
                    font.bold: true
                    Layout.fillWidth: true
                }
                Widgets.LabelExt {
                    text: qsTr("NAI Paintbrush")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: theme.border
        }

        /* ---- scrollable body ------------------------------------------ */
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: VLCStyle.margin_normal
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent.width
                spacing: VLCStyle.margin_normal

                /* ---------------- AI Models ------------------------------- */
                SettingsSection {
                    title: qsTr("AI models")

                    SettingsRow {
                        label: qsTr("Object Select (SAM3)")
                        status: PaintbrushCtx.segmentationAvailable
                                ? qsTr("Ready") : qsTr("Not configured")
                        ok: PaintbrushCtx.segmentationAvailable
                        fieldText: root.draftSamModel
                        placeholder: qsTr("Path to the SAM model (.ggml)")
                        onTextEdited: root.draftSamModel = text
                    }
                    SettingsRow {
                        label: qsTr("Device")
                        status: PaintbrushCtx.segmentationStatusText
                        ok: PaintbrushCtx.segmentationAvailable
                        fieldText: root.draftSamDevice
                        placeholder: qsTr("cpu or gpu")
                        onTextEdited: root.draftSamDevice = text
                    }
                    SettingsRow {
                        label: qsTr("Ask — vision model")
                        status: PaintbrushCtx.visionStatusText
                        ok: PaintbrushCtx.visionStatusText.length > 0
                        fieldText: root.draftVlmUrl
                        placeholder: qsTr("Endpoint URL (OpenAI-compatible)")
                        onTextEdited: root.draftVlmUrl = text
                    }
                    SettingsRow {
                        label: qsTr("Vision model name")
                        status: ""
                        ok: true
                        fieldText: root.draftVlmModel
                        placeholder: qsTr("e.g. gpt-4o-mini")
                        onTextEdited: root.draftVlmModel = text
                    }
                }

                /* ---------------- Companion ------------------------------ */
                SettingsSection {
                    title: qsTr("Companion")

                    SettingsRow {
                        label: qsTr("AI companion")
                        status: PaintbrushCtx.assistantUnavailableReason.length > 0
                                ? PaintbrushCtx.assistantUnavailableReason
                                : qsTr("Ready")
                        ok: PaintbrushCtx.assistantUnavailableReason.length === 0
                        fieldText: root.draftAssistantUrl
                        placeholder: qsTr("Endpoint URL")
                        onTextEdited: root.draftAssistantUrl = text
                    }
                    SettingsRow {
                        label: qsTr("Companion model")
                        status: ""
                        ok: true
                        fieldText: root.draftAssistantModel
                        placeholder: qsTr("Model name")
                        onTextEdited: root.draftAssistantModel = text
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: PaintbrushCtx.settingsSecretsPresent
                              ? qsTr("API keys are configured.")
                              : qsTr("No API keys configured — the local endpoints above need none.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        wrapMode: Text.WordWrap
                    }
                }

                /* ---------------- Capabilities --------------------------- */
                SettingsSection {
                    title: qsTr("Capabilities")

                    SettingsInfoRow {
                        label: qsTr("Image generation")
                        value: qsTr("Local heal — pixel propagation, not generative")
                    }
                    SettingsInfoRow {
                        label: qsTr("Object tracking")
                        value: PaintbrushCtx.trackingStatusText
                    }
                    SettingsInfoRow {
                        label: qsTr("Product lookup")
                        value: PaintbrushCtx.lookupAvailable
                               ? qsTr("Ready")
                               : PaintbrushCtx.lookupUnavailableReason
                    }
                }

                /* ---------------- Media ---------------------------------- */
                SettingsSection {
                    title: qsTr("Media")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_small

                        Widgets.LabelExt {
                            text: qsTr("Exact-frame tolerance")
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                            Layout.fillWidth: true
                        }
                        SpinBox {
                            from: 0
                            to: 5000
                            stepSize: 50
                            value: root.draftPtsToleranceMs
                            editable: true
                            onValueModified: root.draftPtsToleranceMs = value
                        }
                        Widgets.LabelExt {
                            text: qsTr("ms")
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Used by AI crops and exports. Filmstrip thumbnails accept the nearest keyframe and are labelled with its real time.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        wrapMode: Text.WordWrap
                    }
                }

                /* ---------------- Diagnostics ---------------------------- */
                SettingsSection {
                    title: qsTr("Diagnostics")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_small

                        Widgets.ActionButtonPrimary {
                            text: qsTr("Run decoder + SAM self test")
                            onClicked: PaintbrushCtx.runDecoderSamSelfTest()
                        }
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: PaintbrushCtx.selfTestReport
                        color: theme.fg.primary
                        wrapMode: Text.WordWrap
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                }
            }
        }

        /* ---- sticky footer -------------------------------------------- */
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: theme.border
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: VLCStyle.margin_normal
            spacing: VLCStyle.margin_small

            Widgets.LabelExt {
                Layout.fillWidth: true
                visible: root.applyMessage.length > 0
                text: root.applyMessage
                color: theme.accent
                wrapMode: Text.WordWrap
                font.pixelSize: VLCStyle.fontSize_small
            }
            Widgets.LabelExt {
                Layout.fillWidth: true
                visible: PaintbrushCtx.settingsRestartRequired && root.applyMessage.length === 0
                text: qsTr("Model and endpoint changes take effect after restart.")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                font.pixelSize: VLCStyle.fontSize_xsmall
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_small

                Item { Layout.fillWidth: true }

                Button {
                    text: qsTr("Cancel")
                    onClicked: {
                        root.resetDraft()
                        root.close()
                    }
                }
                Button {
                    text: qsTr("Apply")
                    highlighted: true
                    onClicked: root.apply()
                }
            }
        }
    }

    /* ---- small, reusable building blocks ------------------------------ */
    component SettingsSection: ColumnLayout {
        property string title: ""
        Layout.fillWidth: true
        spacing: VLCStyle.margin_small

        Widgets.LabelExt {
            text: parent.title.toUpperCase()
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
            font.bold: true
            font.letterSpacing: 1.2
            Layout.fillWidth: true
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: theme.border
        }
        default property alias content: column.data

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            spacing: VLCStyle.margin_small
        }
    }

    component SettingsRow: ColumnLayout {
        id: row
        property string label: ""
        property string status: ""
        property bool ok: true
        property string fieldText: ""
        property string placeholder: ""
        signal textEdited(string text)

        Layout.fillWidth: true
        spacing: VLCStyle.margin_xxsmall

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: row.label
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                Layout.fillWidth: true
            }
            Rectangle {
                width: 8; height: 8; radius: 4
                color: row.ok ? "#3ecf6b" : theme.fg.secondary
                opacity: row.ok ? 1.0 : 0.5
            }
            Widgets.LabelExt {
                text: row.status
                visible: row.status.length > 0
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
        }
        TextField {
            Layout.fillWidth: true
            text: row.fieldText
            placeholderText: row.placeholder
            selectByMouse: true
            onTextEdited: row.textEdited(text)
        }
    }

    component SettingsInfoRow: RowLayout {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        spacing: VLCStyle.margin_xsmall

        Widgets.LabelExt {
            text: parent.label
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
            Layout.fillWidth: true
        }
        Widgets.LabelExt {
            text: parent.value
            color: theme.fg.primary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
    }
}
