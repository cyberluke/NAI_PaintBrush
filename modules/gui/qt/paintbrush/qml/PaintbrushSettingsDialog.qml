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
 *  - Apply calls PaintbrushCtx.applyNaiSettings(...) exactly once. The
 *    controller returns EMPTY on success, so the dialog closes; a non-empty
 *    answer is a real failure and stays inline. Model/endpoint/key changes are
 *    hot-applied (the controller reconfigures the running services and reloads
 *    the changed model in the background) — there is no restart state.
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
    /* No stock title bar: the custom header below carries the identity, so
     * the dialog is not framed by a duplicate title. */
    header: null

    /* Centered on the workspace when opened. Dragging the header writes x/y
     * imperatively, which intentionally replaces these bindings. */
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    width: Math.min(VLCStyle.dp(680, VLCStyle.scale),
                    (parent ? parent.width : 900) - VLCStyle.margin_normal * 2)
    height: Math.min(VLCStyle.dp(720, VLCStyle.scale),
                     (parent ? parent.height : 800) - VLCStyle.margin_normal * 2)

    /* ---- draft state ------------------------------------------------- */
    property string draftSamModel: ""
    property string draftSamDevice: ""
    property string draftMatteModelDir: ""
    property string draftMatteDevice: ""
    property string draftVlmUrl: ""
    property string draftVlmModel: ""
    property string draftVlmApiKey: ""
    property string draftAssistantUrl: ""
    property string draftAssistantModel: ""
    property string draftAssistantApiKey: ""
    property string draftImageUrl: ""
    property string draftImageModel: ""
    property string draftImageApiKey: ""
    property int draftPtsToleranceMs: 250
    property string draftLanguage: ""
    property string applyMessage: ""

    function resetDraft() {
        draftSamModel = PaintbrushCtx.settingsSamModel
        draftSamDevice = PaintbrushCtx.settingsSamDevice
        draftMatteModelDir = PaintbrushCtx.mattingModelDir
        draftMatteDevice = PaintbrushCtx.mattingDevice
        draftVlmUrl = PaintbrushCtx.settingsVlmUrl
        draftVlmModel = PaintbrushCtx.settingsVlmModel
        draftVlmApiKey = PaintbrushCtx.settingsVlmApiKey
        draftAssistantUrl = PaintbrushCtx.settingsAssistantUrl
        draftAssistantModel = PaintbrushCtx.settingsAssistantModel
        draftAssistantApiKey = PaintbrushCtx.settingsAssistantApiKey
        draftImageUrl = PaintbrushCtx.settingsImageUrl
        draftImageModel = PaintbrushCtx.settingsImageModel
        draftImageApiKey = PaintbrushCtx.settingsImageApiKey
        draftPtsToleranceMs = PaintbrushCtx.settingsPtsToleranceMs
        draftLanguage = PaintbrushCtx.settingsLanguage
        applyMessage = ""
    }
    /* paintbrush-language values; empty = system locale. The native names are
     * intentionally not translated — a language picker should name each
     * language in that language. */
    readonly property var languageCodes: ["", "cs", "sk", "vi", "fr", "de", "it", "es"]
    readonly property var languageNames: [
        qsTr("System default"), "Čeština", "Slovenčina", "Tiếng Việt",
        "Français", "Deutsch", "Italiano", "Español"
    ]
    function apply() {
        /* Matting settings are applied through their own live properties so
         * applyNaiSettings keeps its established signature. */
        PaintbrushCtx.mattingModelDir = draftMatteModelDir.trim()
        PaintbrushCtx.mattingDevice = draftMatteDevice
        const imageAnswer = PaintbrushCtx.applyImageSettings(
                    draftImageUrl.trim(),
                    draftImageModel.trim(),
                    draftImageApiKey.trim())
        if (imageAnswer && imageAnswer.length > 0) {
            applyMessage = imageAnswer
            return false
        }
        const answer = PaintbrushCtx.applyNaiSettings(
                    draftSamModel.trim(),
                    draftSamDevice.trim(),
                    draftVlmUrl.trim(),
                    draftVlmModel.trim(),
                    draftVlmApiKey.trim(),
                    draftAssistantUrl.trim(),
                    draftAssistantModel.trim(),
                    draftAssistantApiKey.trim(),
                    draftPtsToleranceMs)
        if (answer && answer.length > 0) {
            applyMessage = answer          /* keep open: the user must see it */
            return false
        }
        /* The language is already live (applied on selection). */
        applyMessage = ""
        close()
        return true
    }

    onOpened: {
        resetDraft()
        PaintbrushCtx.refreshSamGpuProbe()
    }
    /* Escape/titlebar close must discard the draft exactly like Cancel. */
    onClosed: resetDraft()

    /* VLC4-neutral palette as literals: ColorContext does not resolve inside
     * this Dialog's own scopes and produced magenta "unresolved" markers. */
    QtObject {
        id: theme
        readonly property QtObject fg: QtObject {
            readonly property color primary: "#e8e8e8"
            readonly property color secondary: "#a8a8a8"
        }
        readonly property color accent: "#4ea1d3"
        readonly property color border: "#2c2c2c"
        readonly property QtObject bg: QtObject {
            readonly property color primary: "#121212"
        }
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

        /* ---- header (also the drag surface) --------------------------- */
        Rectangle {
            id: headerBar
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + VLCStyle.margin_normal * 2
            color: "transparent"

            DragHandler {
                id: dialogDrag
                target: null
                cursorShape: active ? Qt.ClosedHandCursor
                                    : Qt.OpenHandCursor
                property point grabStart: Qt.point(0, 0)
                onActiveChanged: if (active)
                    grabStart = Qt.point(root.x, root.y)
                onTranslationChanged: {
                    const maxX = Math.max(0, (root.parent ? root.parent.width
                                                          : root.width) - root.width)
                    const maxY = Math.max(0, (root.parent ? root.parent.height
                                                          : root.height) - root.height)
                    root.x = Math.max(0, Math.min(maxX,
                                                  grabStart.x + translation.x))
                    root.y = Math.max(0, Math.min(maxY,
                                                  grabStart.y + translation.y))
                }
            }

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
                    font.pixelSize: VLCStyle.fontSize_normal
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

                /* ---------------- Interface ------------------------------ */
                SettingsSection {
                    title: qsTr("Interface")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall
                        Widgets.LabelExt {
                            text: qsTr("Language")
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_normal
                            Layout.fillWidth: true
                        }
                        ComboBox {
                            model: root.languageNames
                            currentIndex: Math.max(0, root.languageCodes.indexOf(root.draftLanguage))
                            /* Apply on selection: the controller persists the
                             * choice, exports the locale and retranslates the
                             * QML engine, so the whole interface switches
                             * immediately — no Apply or restart needed. */
                            onActivated: {
                                root.draftLanguage = root.languageCodes[currentIndex]
                                const answer = PaintbrushCtx.applyLanguage(root.draftLanguage)
                                if (answer && answer.length > 0)
                                    root.applyMessage = answer
                            }
                            Layout.preferredWidth: VLCStyle.dp(180, VLCStyle.scale)
                        }
                    }
                }

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
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall
                        Widgets.LabelExt {
                            text: qsTr("Device")
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_normal
                            Layout.fillWidth: true
                        }
                        Widgets.LabelExt {
                            text: PaintbrushCtx.segmentationStatusText
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                        ComboBox {
                            model: ["gpu", "auto", "cpu"]
                            currentIndex: Math.max(0, model.indexOf(root.draftSamDevice))
                            onActivated: root.draftSamDevice = model[currentIndex]
                            Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
                        }
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: !PaintbrushCtx.samGpuBackendLinked
                              ? qsTr("CUDA runtime (sam3_cuda.dll) is not available. GPU mode will fail with a specific error; choose CPU.")
                              : (PaintbrushCtx.samGpuProbeError.length > 0
                                 ? qsTr("CUDA runtime present, but no usable GPU: %1").arg(PaintbrushCtx.samGpuProbeError)
                                 : qsTr("Requested: %1 · CUDA runtime ready · detected: %2. The actual backend is verified when the model loads.")
                                   .arg(root.draftSamDevice).arg(PaintbrushCtx.samGpuDeviceName))
                        color: (!PaintbrushCtx.samGpuBackendLinked
                                || PaintbrushCtx.samGpuProbeError.length > 0)
                               ? theme.accent : theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
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
                    SettingsRow {
                        label: qsTr("Vision API key")
                        status: PaintbrushCtx.settingsVlmApiKeyFromEnv
                                ? qsTr("Environment override")
                                : (root.draftVlmApiKey.length > 0
                                   ? qsTr("Stored") : qsTr("Not set"))
                        ok: PaintbrushCtx.settingsVlmApiKeyFromEnv
                            || root.draftVlmApiKey.length > 0
                        fieldText: root.draftVlmApiKey
                        placeholder: qsTr("Bearer token (optional for local endpoints)")
                        secret: true
                        onTextEdited: root.draftVlmApiKey = text
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        visible: PaintbrushCtx.settingsVlmApiKeyFromEnv
                        text: qsTr("PAINTBRUSH_VLM_API_KEY is set in the environment and overrides the key stored here.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
                    }
                }

                /* ---------------- Neural matting ------------------------- */
                SettingsSection {
                    title: qsTr("Neural matting")

                    SettingsRow {
                        label: qsTr("ViTMatte model directory")
                        status: PaintbrushCtx.mattingNeuralAvailable
                                ? qsTr("Ready") : qsTr("Unavailable")
                        ok: PaintbrushCtx.mattingNeuralAvailable
                        fieldText: root.draftMatteModelDir
                        placeholder: qsTr("Directory with vitmatte-s-*.onnx + manifest.json")
                        onTextEdited: root.draftMatteModelDir = text
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall
                        Widgets.LabelExt {
                            text: qsTr("Device")
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_normal
                            Layout.fillWidth: true
                        }
                        Widgets.LabelExt {
                            text: PaintbrushCtx.mattingBackendText
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
                        }
                        ComboBox {
                            model: ["gpu", "auto", "cpu"]
                            currentIndex: Math.max(0, model.indexOf(root.draftMatteDevice))
                            onActivated: root.draftMatteDevice = model[currentIndex]
                            Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
                        }
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: PaintbrushCtx.mattingStatusText
                        color: PaintbrushCtx.mattingNeuralAvailable ? theme.fg.secondary
                                                                   : theme.accent
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
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
                    SettingsRow {
                        label: qsTr("Companion API key")
                        status: PaintbrushCtx.settingsAssistantApiKeyFromEnv
                                ? qsTr("Environment override")
                                : (root.draftAssistantApiKey.length > 0
                                   ? qsTr("Stored") : qsTr("Not set"))
                        ok: PaintbrushCtx.settingsAssistantApiKeyFromEnv
                            || root.draftAssistantApiKey.length > 0
                        fieldText: root.draftAssistantApiKey
                        placeholder: qsTr("Bearer token (optional for local endpoints)")
                        secret: true
                        onTextEdited: root.draftAssistantApiKey = text
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        visible: PaintbrushCtx.settingsAssistantApiKeyFromEnv
                        text: qsTr("PAINTBRUSH_ASSISTANT_API_KEY is set in the environment and overrides the key stored here.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Keys entered here are saved in the plain-text VLC configuration file and reused on the next start. Environment variables (PAINTBRUSH_VLM_API_KEY / PAINTBRUSH_ASSISTANT_API_KEY) take precedence when set. Keys are never written to logs.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
                    }

                    /* Parental control for Kids mode. Off by default: Kids
                     * stays local. A parent/guardian can enable the provider
                     * here without exposing the rest of the AI settings. */
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: VLCStyle.margin_xsmall
                        spacing: VLCStyle.margin_small

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: qsTr("Allow network AI in Kids mode")
                                color: theme.fg.primary
                                font.pixelSize: VLCStyle.fontSize_normal
                            }
                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: qsTr("Parental control. Off keeps Kids mode offline; on lets it use this AI provider. Only a parent or guardian should enable it.")
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_small
                                wrapMode: Text.WordWrap
                            }
                        }
                        Widgets.CheckBoxExt {
                            text: ""
                            checked: PaintbrushCtx.kidsNetworkAllowed
                            onToggled: PaintbrushCtx.setKidsNetworkAllowed(checked)
                            Accessible.name: qsTr("Allow network AI in Kids mode (parental control)")
                        }
                    }
                }

                /* ---------------- Image generation ----------------------- */
                SettingsSection {
                    title: qsTr("Image generation")

                    SettingsRow {
                        label: qsTr("Image endpoint")
                        status: PaintbrushCtx.generationStatusText
                        ok: PaintbrushCtx.imageProviderConfigured
                        fieldText: root.draftImageUrl
                        placeholder: qsTr("Image API base URL, e.g. https://…/v1")
                        onTextEdited: root.draftImageUrl = text
                    }
                    SettingsRow {
                        label: qsTr("Image model")
                        status: ""
                        ok: true
                        fieldText: root.draftImageModel
                        placeholder: qsTr("Image model name")
                        onTextEdited: root.draftImageModel = text
                    }
                    SettingsRow {
                        label: qsTr("Image API key")
                        status: PaintbrushCtx.settingsImageApiKeyFromEnv
                                ? qsTr("Environment override")
                                : (root.draftImageApiKey.length > 0
                                   ? qsTr("Stored") : qsTr("Not set"))
                        ok: PaintbrushCtx.settingsImageApiKeyFromEnv
                            || root.draftImageApiKey.length > 0
                        fieldText: root.draftImageApiKey
                        placeholder: qsTr("Bearer token (optional for local endpoints)")
                        secret: true
                        onTextEdited: root.draftImageApiKey = text
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Used by Finish the painting, Remove object, Generative fill and Replace background. Remove object and an empty-prompt fill use the local LaMa neural model when it is installed; a prompt-guided fill and Finish need a real image endpoint. Without either, those actions fall back to local pixel operations.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
                        wrapMode: Text.WordWrap
                    }
                }

                /* ---------------- Capabilities --------------------------- */
                SettingsSection {
                    title: qsTr("Capabilities")

                    SettingsInfoRow {
                        label: qsTr("Image generation")
                        value: PaintbrushCtx.generationStatusText
                    }
                    SettingsInfoRow {
                        label: qsTr("Object tracking")
                        /* the tracking service can still be uninitialized on
                         * the first dialog open; never bind undefined */
                        value: PaintbrushCtx.trackingStatusText || qsTr("Idle")
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
                            font.pixelSize: VLCStyle.fontSize_normal
                            Layout.fillWidth: true
                        }
                        SpinBox {
                            /* the controller/config range is 20..2000 ms */
                            from: 20
                            to: 2000
                            stepSize: 10
                            value: root.draftPtsToleranceMs
                            editable: true
                            onValueModified: root.draftPtsToleranceMs = value
                        }
                        Widgets.LabelExt {
                            text: qsTr("ms")
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_normal
                        }
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Used by AI crops and exports. Filmstrip thumbnails accept the nearest keyframe and are labelled with its real time.")
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_small
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
                        font.pixelSize: VLCStyle.fontSize_small
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
                font.pixelSize: VLCStyle.fontSize_normal
            }
            Widgets.LabelExt {
                Layout.fillWidth: true
                visible: root.applyMessage.length === 0
                text: qsTr("Changes apply immediately — no restart needed. The AI model reloads in the background when it changes.")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                font.pixelSize: VLCStyle.fontSize_normal
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
            font.pixelSize: VLCStyle.fontSize_small
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
        /* Secret rows (API keys) mask the value and offer an explicit reveal;
         * the value is still the persisted one, so Apply round-trips it. */
        property bool secret: false
        property bool revealed: false
        signal textEdited(string text)

        Layout.fillWidth: true
        spacing: VLCStyle.margin_xxsmall

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: row.label
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_normal
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
                font.pixelSize: VLCStyle.fontSize_small
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xxsmall

            TextField {
                id: field
                Layout.fillWidth: true
                text: row.fieldText
                placeholderText: row.placeholder
                selectByMouse: true
                echoMode: row.secret && !row.revealed
                          ? TextInput.Password : TextInput.Normal
                Accessible.name: row.label
                onTextEdited: row.textEdited(text)
            }
            Button {
                visible: row.secret
                text: row.revealed ? qsTr("Hide") : qsTr("Show")
                flat: true
                Layout.preferredWidth: VLCStyle.dp(72, VLCStyle.scale)
                Accessible.name: row.revealed
                                 ? qsTr("Hide API key") : qsTr("Show API key")
                onClicked: {
                    row.revealed = !row.revealed
                    field.forceActiveFocus()
                    field.cursorPosition = field.length
                }
            }
        }
        /* A TextField drops its declarative text binding on the first
         * keystroke. Re-assign from the draft whenever the draft is reset
         * (dialog open/close/Apply), so stored values actually reappear. The
         * inequality guard keeps normal typing untouched (no cursor jump). */
        onFieldTextChanged: {
            if (field.text !== row.fieldText)
                field.text = row.fieldText
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
            font.pixelSize: VLCStyle.fontSize_normal
            Layout.fillWidth: true
        }
        Widgets.LabelExt {
            text: parent.value
            color: theme.fg.primary
            font.pixelSize: VLCStyle.fontSize_small
        }
    }
}
