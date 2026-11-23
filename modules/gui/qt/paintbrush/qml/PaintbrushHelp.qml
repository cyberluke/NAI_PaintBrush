/*****************************************************************************
 * PaintbrushHelp.qml : NAI Paintbrush — plain-language guide
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

/* The workspace guide (F1 or the Help button).
 *
 * The left column is built from the REAL tool rail (`tools`), so icon, order
 * and name can never drift from what the user sees. The right column is prose
 * keyed by tool id; a tool added to the rail without prose still appears with
 * its tooltip and a generic line instead of silently vanishing from the guide.
 *
 * Editorial rule: this is written for the person making the picture, not for
 * the person who wrote the renderer. No parameter names from the code, no
 * "material", "holographic", "reference radius" or spec numbers. Say what the
 * control DOES and when to reach for it. */
Item {
    id: root

    /* shown/hidden by the workspace (F1, Help button) */
    property bool opened: false
    /* rail cells: [{ toolId, iconSource, tip }] — the icon/name source of truth */
    property var tools: []
    /* the tool the user had selected when opening the guide */
    property int activeToolId: 0
    property int selectedId: 0

    readonly property string tdesignPath: "qrc:///icons/tdesign/"

    /* synthetic pages: the intro is the first page, bars&buttons is last */
    readonly property int introId: -100
    readonly property int barsId: -101
    readonly property int canvasBarsId: -102

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    function show() {
        /* the guide always opens on its front page; the active tool is one
         * click away (and the intro offers a direct jump to it) */
        selectedId = introId
        opened = true
    }

    function hide() { opened = false }
    function toggle() { opened ? hide() : show() }

    function guideFor(id) {
        for (let i = 0; i < guide.length; ++i)
            if (guide[i].toolId === id)
                return guide[i]
        return null
    }

    function entryFor(id) {
        for (let i = 0; i < entries.length; ++i)
            if (entries[i].toolId === id)
                return entries[i]
        return null
    }

    /* ------------------------------------------------------------- content */

    readonly property var introGuide: ({
        toolId: introId,
        name: qsTr("Start here"),
        what: qsTr("NAI Paintbrush is a small studio: paint, cut things out with AI, pour glitter, and export a picture or a living poster."),
        use: qsTr("Everything else in this guide is one page per tool, in the same order as the tool rail."),
        controls: [],
        sections: [
            {
                title: qsTr("The 30-second version"),
                lines: [
                    qsTr("Pick a tool in the rail on the left. The bottom bar shows that tool's settings."),
                    qsTr("Drag on the canvas to make something. Undo in the top bar always takes you back."),
                    qsTr("The two colour squares at the bottom set your paint colour and background colour."),
                    qsTr("When it looks right, Export in the top bar saves a PNG, JPG or WebP.")
                ]
            },
            {
                title: qsTr("Three ways to begin"),
                lines: [
                    qsTr("Start drawing — open a blank canvas."),
                    qsTr("Open a picture — paint over a photo."),
                    qsTr("Play a video, then capture a frame — remix a moment.")
                ]
            },
            {
                title: qsTr("Where things live"),
                lines: [
                    qsTr("Left rail — the tools."),
                    qsTr("Bottom bar — settings for the active tool, colours, palettes and moods."),
                    qsTr("Right inspector — layers, plus the AI actions for a cut-out."),
                    qsTr("Top bar — open, save, export, undo/redo, settings and this guide."),
                    qsTr("F1 opens this guide at any time; Esc closes it.")
                ]
            }
        ],
        tip: qsTr("Nothing here is permanent: Undo is always one click away in the top bar.")
    })

    /* the floating strips/panels: the thing users most often cannot identify */
    readonly property var canvasBarsGuide: ({
        toolId: canvasBarsId,
        name: qsTr("On-canvas bars"),
        what: qsTr("The strips that appear only when they are useful: selection actions, Ask, text editing, the inspector and the video drawer."),
        use: qsTr("If a bar is in your way, hide it — nothing is lost, and a new selection brings it back."),
        controls: [],
        sections: [
            {
                title: qsTr("Selection bar (top of the canvas)"),
                lines: [
                    qsTr("Appears when you select an area or an AI subject. The label says what it acts on."),
                    qsTr("Object group: Cut out (removes the object and fills the hole), Add to canvas (keeps it as a layer), Make sticker, Remove object (AI fill), Export object (PNG)."),
                    qsTr("Selection group: Copy, Cut, Clear selection."),
                    qsTr("Ask about this — asks the AI about the selected thing. Track in video — follows it through the clip."),
                    qsTr("The chevron on the right HIDES the bar and keeps your selection. The X in the selection group clears the selection itself."),
                    qsTr("Hover any icon for its name.")
                ]
            },
            {
                title: qsTr("Ask bar"),
                lines: [
                    qsTr("Appears under the selection bar after Ask about this. Type a question and press Enter.")
                ]
            },
            {
                title: qsTr("Text bar"),
                lines: [
                    qsTr("Appears when you add or double-click a text layer: the word, style, size, letter spacing and colour. Every change is one undo step.")
                ]
            },
            {
                title: qsTr("Inspector (right side)"),
                lines: [
                    qsTr("Layers tab: the layer stack with its own scrollbar, visibility, drag reorder, opacity, transform and motion."),
                    qsTr("AI tab: select a subject, then find by words, remove, extract, replace background, generative fill, find product, auto collage and the matte controls."),
                    qsTr("Sections collapse; the scrollbar on the right means nothing is lost off the bottom.")
                ]
            },
            {
                title: qsTr("Companion (chat)"),
                lines: [
                    qsTr("Opens from the companion button in the top bar. Type and press Enter to chat; it can look at your drawing and suggest ideas."),
                    qsTr("If a mode is off or Kids mode has network AI disabled, it now tells you why instead of staying silent.")
                ]
            },
            {
                title: qsTr("Video drawer"),
                lines: [
                    qsTr("Appears when a video is open: transport, frame capture, trim, tracked objects and the timeline under the canvas.")
                ]
            }
        ],
        tip: qsTr("Hover an icon for its tooltip; F1 opens this guide at any time.")
    })

    readonly property var guide: [
        {
            toolId: 1,
            name: qsTr("Rect Select"),
            what: qsTr("Marks a rectangular area so your next action only touches that part of the picture."),
            use: qsTr("Cropping, moving a block of the composition, or protecting everything around it."),
            controls: [],
            tip: qsTr("Drag the box first; the inspector then shows what you can do with the area.")
        },
        {
            toolId: 2,
            name: qsTr("Freeform Select"),
            what: qsTr("Draws a freehand outline around anything. Close the loop and that shape becomes your selection."),
            use: qsTr("Odd shapes: a hand, a shoe, a torn edge, a strand of hair."),
            controls: [],
            tip: qsTr("A smooth outline gives a cleaner edge than a rushed one.")
        },
        {
            toolId: 3,
            name: qsTr("Object Select (AI)"),
            what: qsTr("Paint roughly over a person or object and NAI finds the real edges for you."),
            use: qsTr("Photos and video frames with a clear subject you want to cut out."),
            controls: [
                { name: qsTr("Mask"), what: qsTr("How wide the marking brush is. Use a big one to cover the subject, a small one to tidy the edges.") },
                { name: qsTr("Soft / hard edge"), what: qsTr("Soft edge feathers the cut-out; hard edge keeps it crisp. Use soft for hair and fur, hard for products and graphics.") }
            ],
            tip: qsTr("Mark the middle first, tidy the edges after, then press Keep.")
        },
        {
            toolId: 4,
            name: qsTr("Pencil"),
            what: qsTr("A thin, crisp line that follows your hand exactly. No softness, no fuss."),
            use: qsTr("Sketches, outlines, fine detail and clean line art."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Line thickness. Down to 1 for a hairline, up to 64 for bold marks.") }
            ],
            tip: qsTr("Zoom in when you need precision — the pencil follows the zoom.")
        },
        {
            toolId: 5,
            name: qsTr("Brush"),
            what: qsTr("A smooth, soft-edged stroke. The everyday painting tool."),
            use: qsTr("Colour, shading, blocking in shapes and gentle blending."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Brush thickness. Down to 1 for detail; larger for filling areas quickly.") }
            ],
            tip: qsTr("With a pen tablet, pressure changes the stroke as you press harder.")
        },
        {
            toolId: 6,
            name: qsTr("Eraser"),
            what: qsTr("Removes paint from the part you are working on, back to see-through."),
            use: qsTr("Fixing edges, opening highlights and cleaning up mistakes."),
            controls: [
                { name: qsTr("Size"), what: qsTr("How much is erased in one pass.") }
            ],
            tip: qsTr("It only erases the part you are on; everything underneath stays untouched.")
        },
        {
            toolId: 7,
            name: qsTr("Fill"),
            what: qsTr("Click inside a shape to flood it with your current colour."),
            use: qsTr("Flat colour blocks, backgrounds and quick recolours."),
            controls: [],
            tip: qsTr("If the colour leaks outside, close the gap in the outline first.")
        },
        {
            toolId: 8,
            name: qsTr("Eyedropper"),
            what: qsTr("Picks a colour already on the canvas and makes it your paint colour."),
            use: qsTr("Matching an existing shade exactly."),
            controls: [],
            tip: qsTr("Handy for keeping a palette consistent across a piece.")
        },
        {
            toolId: 9,
            name: qsTr("Spray"),
            what: qsTr("Soft bursts of colour, like an airbrush."),
            use: qsTr("Glow, texture, soft shading, stars and atmosphere."),
            controls: [
                { name: qsTr("Size"), what: qsTr("How wide the spray cloud is.") },
                { name: qsTr("Flow"), what: qsTr("How much colour each pass lays down. Low is a whisper, high is a blast.") }
            ],
            tip: qsTr("Short taps build up slowly; hold longer for denser colour.")
        },
        {
            toolId: 16,
            name: qsTr("Bristle Paint"),
            what: qsTr("A textured brush with visible bristle streaks and slightly shifting colour, like real paint."),
            use: qsTr("Expressive strokes, hair, grass and fabric."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Brush width.") },
                { name: qsTr("Flow"), what: qsTr("How much paint each stroke carries. Low is dry-brush, high is wet and heavy.") },
                { name: qsTr("Pigment"), what: qsTr("How much the colour shifts between bristles. Low is even; high is streaky and hand-made.") }
            ],
            tip: qsTr("Layer two colours to get a genuinely painted, imperfect feel.")
        },
        {
            toolId: 17,
            name: qsTr("Glitter"),
            what: qsTr("Scatters tiny sparkling flakes that catch the light. Each flake behaves like a little mirror."),
            use: qsTr("Sparkle on clothes, makeup, posters, lettering and magical accents."),
            controls: [
                { name: qsTr("Size"), what: qsTr("How wide the glitter area is.") },
                { name: qsTr("Amount"), what: qsTr("How much glitter falls with each pass.") },
                { name: qsTr("Flake"), what: qsTr("How big each flake is. Small reads as dust, large reads as craft glitter.") },
                { name: qsTr("Shimmer"), what: qsTr("How much white twinkle and rainbow shift. 0 is quiet metal, 1 is full disco.") },
                { name: qsTr("Glue"), what: qsTr("How strongly flakes stick. Raise it so glitter stays exactly where you put it.") }
            ],
            tip: qsTr("Pour glitter over Glue when you want a shape that does not drift.")
        },
        {
            toolId: 18,
            name: qsTr("Glitter Powder"),
            what: qsTr("A real craft bench: lay glue, pour glitter, push it around and blow the loose bits away."),
            use: qsTr("Raised, tactile glitter that looks like actual material on paper."),
            subtools: [
                { name: qsTr("Glue"), icon: "dropzone", what: qsTr("Lay a sticky base. Glitter poured on top stays where you put it."),
                  controls: [ { name: qsTr("Size"), what: qsTr("Width of the glue line.") } ] },
                { name: qsTr("Glitter"), icon: "glitter", what: qsTr("Sprinkle flakes onto the canvas or over a glue shape."),
                  controls: [ { name: qsTr("Size"), what: qsTr("How wide the sprinkle area is.") },
                              { name: qsTr("Amount"), what: qsTr("How much glitter falls.") },
                              { name: qsTr("Shimmer"), what: qsTr("How much rainbow sparkle the flakes carry.") } ] },
                { name: qsTr("Push"), icon: "move", what: qsTr("Nudge and smear loose glitter with your finger."),
                  controls: [ { name: qsTr("Size"), what: qsTr("How much glitter you move at once.") } ] },
                { name: qsTr("Pour"), icon: "download", what: qsTr("Tip a whole pile of glitter onto the canvas."),
                  controls: [] },
                { name: qsTr("Blow"), icon: "windy", what: qsTr("A puff of air that moves loose glitter but leaves glued glitter alone."),
                  controls: [] },
                { name: qsTr("Eraser"), icon: "eraser", what: qsTr("Lift glitter away."),
                  controls: [ { name: qsTr("Size"), what: qsTr("Width of the cleaned strip.") } ] }
            ],
            controls: [],
            tip: qsTr("Glue first, pour next, blow away the excess — the same order as real craft glitter.")
        },
        {
            toolId: 10,
            name: qsTr("Line"),
            what: qsTr("Draws a straight line from where you press to where you let go."),
            use: qsTr("Rules, stems, underlines and dividers."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Line thickness.") }
            ],
            tip: qsTr("Start and finish outside the canvas if you need a perfectly clean edge.")
        },
        {
            toolId: 11,
            name: qsTr("Rectangle"),
            what: qsTr("Draws a rectangle. Drag to choose its size."),
            use: qsTr("Frames, cards, panels and colour blocks."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Outline thickness.") }
            ],
            tip: ""
        },
        {
            toolId: 12,
            name: qsTr("Rounded Rectangle"),
            what: qsTr("A rectangle with soft corners."),
            use: qsTr("Buttons, stickers, frames and friendly UI-style shapes."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Outline thickness.") }
            ],
            tip: ""
        },
        {
            toolId: 13,
            name: qsTr("Ellipse"),
            what: qsTr("Draws an oval or a circle."),
            use: qsTr("Faces, dots, bubbles, highlights and soft shapes."),
            controls: [
                { name: qsTr("Size"), what: qsTr("Outline thickness.") }
            ],
            tip: ""
        },
        {
            toolId: 14,
            name: qsTr("Text"),
            what: qsTr("Adds real, editable words to the canvas. Click, type in the bar that appears, then tune the look."),
            use: qsTr("Titles, captions, slogans and posters."),
            controls: [
                { name: qsTr("Word"), what: qsTr("The actual text.") },
                { name: qsTr("Style"), what: qsTr("The font role used for the words.") },
                { name: qsTr("Size"), what: qsTr("How big the letters are.") },
                { name: qsTr("Track"), what: qsTr("Space between letters. Tight for headlines, loose for airy captions.") },
                { name: qsTr("Colour"), what: qsTr("The text colour.") }
            ],
            tip: qsTr("Double-click a text layer later to edit the words again.")
        },
        {
            toolId: 15,
            name: qsTr("Move"),
            what: qsTr("Drag a layer to move it. Drag empty space to pan the whole view."),
            use: qsTr("Rearranging the composition without changing the artwork."),
            controls: [],
            tip: qsTr("Panning changes what you see, not where things are.")
        },
        {
            toolId: 20,
            name: qsTr("Ask"),
            what: qsTr("Opens the NAI companion so you can describe what you want in words."),
            use: qsTr("Finding things, replacing backgrounds, inventing ideas and quick edits."),
            controls: [],
            tip: qsTr("Say it the way you would say it to a friend — there are no special commands.")
        }
    ]

    /* the extra entry after the rail: everything around the canvas */
    readonly property var barsGuide: ({
        toolId: -1,
        name: qsTr("Bars & buttons"),
        what: qsTr("The controls around the canvas: colour, palettes, looks, precision helpers and the actions that save or share your work."),
        use: "",
        controls: [],
        sections: [
            {
                title: qsTr("Bottom bar"),
                lines: [
                    qsTr("Colour chips — the two squares show your paint colour and background colour. Click either one to change it."),
                    qsTr("Palettes — ready-made colour sets. Neon is the default; open the list to see all of them."),
                    qsTr("Moods — whole looks, not just colours. Press M to open them."),
                    qsTr("Keep — saves the cut-out you just made into your tray."),
                    qsTr("Boost — NAI's one-click polish for the whole composition."),
                    qsTr("Grid, Rulers, Snap, Pen pressure — visual helpers. Snap lines things up for you; Pen pressure follows your stylus.")
                ]
            },
            {
                title: qsTr("Top bar"),
                lines: [
                    qsTr("Undo / Redo — step backwards and forwards through your edits."),
                    qsTr("Open image / Open video — bring in a picture or a video frame to work on."),
                    qsTr("Save project — keeps layers, text and settings so you can come back later."),
                    qsTr("Export — writes a PNG, JPG or WebP you can share."),
                    qsTr("Settings — NAI options for this workspace."),
                    qsTr("Back to VLC — returns to the player without closing your project.")
                ]
            },
            {
                title: qsTr("Zoom"),
                lines: [
                    qsTr("The + and − buttons at the bottom of the tool rail zoom in and out; the frame button fits the whole picture in view.")
                ]
            }
        ],
        tip: qsTr("Nothing here is permanent: Undo is always one click away in the top bar.")
    })

    readonly property var entries: {
        const out = []
        out.push({ toolId: introId, iconSource: "", iconGlyph: VLCIcons.info,
                   name: qsTr("Start here") })
        for (let i = 0; i < tools.length; ++i) {
            const t = tools[i]
            const g = guideFor(t.toolId)
            out.push({
                toolId: t.toolId,
                iconSource: t.iconSource || "",
                iconGlyph: "",
                name: g ? g.name : (t.tip || qsTr("Tool"))
            })
        }
        out.push({ toolId: canvasBarsId, iconSource: "", iconGlyph: VLCIcons.info,
                   name: qsTr("On-canvas bars") })
        out.push({ toolId: barsId, iconSource: "", iconGlyph: VLCIcons.info,
                   name: qsTr("Bars & buttons") })
        return out
    }

    readonly property var currentEntry: entryFor(selectedId)
    readonly property var fallbackGuide: ({
        toolId: selectedId,
        name: currentEntry ? currentEntry.name : qsTr("Tool"),
        what: qsTr("Pick it in the tool rail, then click or drag on the canvas."),
        use: "",
        controls: [],
        tip: ""
    })
    readonly property var current: selectedId === introId
                                   ? introGuide
                                   : (selectedId === canvasBarsId
                                      ? canvasBarsGuide
                                      : (selectedId === barsId
                                         ? barsGuide
                                         : (guideFor(selectedId) || fallbackGuide)))

    /* ---------------------------------------------------------------- view */

    visible: opened
    z: 100

    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.opened
        onActivated: root.hide()
    }

    /* dim the workspace; clicking outside the card closes the guide */
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.66)
        MouseArea {
            anchors.fill: parent
            onClicked: root.hide()
            /* the guide is modal: a wheel over the dimmed workspace must not
             * reach the canvas and zoom it */
            onWheel: function(wheel) { wheel.accepted = true }
        }
    }

    Rectangle {
        id: card

        anchors.centerIn: parent
        width: Math.min(parent.width - VLCStyle.dp(64, VLCStyle.scale),
                        VLCStyle.dp(1040, VLCStyle.scale))
        height: Math.min(parent.height - VLCStyle.dp(64, VLCStyle.scale),
                         VLCStyle.dp(760, VLCStyle.scale))
        radius: VLCStyle.dp(16, VLCStyle.scale)
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        clip: true

        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("NAI Paintbrush guide")

        /* clicks inside the card must not reach the closing scrim; wheel over
         * the header/footer is swallowed too (the two panes scroll themselves) */
        MouseArea {
            anchors.fill: parent
            onWheel: function(wheel) { wheel.accepted = true }
        }

        Rectangle {
            id: cardHeader

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: VLCStyle.dp(78, VLCStyle.scale)
            color: "transparent"

            Column {
                anchors.left: parent.left
                anchors.leftMargin: VLCStyle.margin_large
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Widgets.LabelExt {
                    text: qsTr("Guide")
                    font.pixelSize: VLCStyle.fontSize_xxxlarge
                    color: theme.fg.primary
                }
                Widgets.LabelExt {
                    text: qsTr("Everything on this screen, in plain words. Press F1 anytime.")
                    font.pixelSize: VLCStyle.fontSize_normal
                    color: theme.fg.secondary
                }
            }

            Widgets.IconToolButton {
                anchors.right: parent.right
                anchors.rightMargin: VLCStyle.margin_normal
                anchors.verticalCenter: parent.verticalCenter
                text: VLCIcons.clear
                description: qsTr("Close the guide (Esc)")
                onClicked: root.hide()
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: theme.border
            }
        }

        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: cardHeader.bottom
            anchors.bottom: cardFooter.top
            spacing: 0

            /* ---- tool list (icons from the real rail) ------------------ */
            ListView {
                id: toolList

                Layout.preferredWidth: VLCStyle.dp(296, VLCStyle.scale)
                Layout.fillHeight: true
                clip: true
                spacing: 2
                model: root.entries
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    id: toolRow

                    required property var modelData
                    width: ListView.view ? ListView.view.width : 0
                    height: VLCStyle.dp(52, VLCStyle.scale)
                    radius: VLCStyle.dp(8, VLCStyle.scale)
                    color: root.selectedId === modelData.toolId
                           ? Qt.rgba(1.0, 0.533, 0.0, 0.16)
                           : (rowMouse.containsMouse ? theme.bg.secondary : "transparent")
                    border.width: root.selectedId === modelData.toolId ? 1 : 0
                    border.color: theme.accent

                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.name

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: VLCStyle.margin_small
                        anchors.rightMargin: VLCStyle.margin_small
                        spacing: VLCStyle.margin_small

                        Image {
                            visible: modelData.iconSource !== ""
                            source: modelData.iconSource !== ""
                                    ? root.tdesignPath + modelData.iconSource + ".svg"
                                    : ""
                            Layout.preferredWidth: VLCStyle.dp(22, VLCStyle.scale)
                            Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                        }
                        Widgets.IconLabel {
                            visible: modelData.iconSource === ""
                            text: modelData.iconGlyph
                            font.pixelSize: VLCStyle.fontSize_large
                            color: root.selectedId === modelData.toolId
                                   ? theme.accent : theme.fg.primary
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: modelData.name
                            font.pixelSize: VLCStyle.fontSize_normal
                            color: root.selectedId === modelData.toolId
                                   ? theme.accent : theme.fg.primary
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.selectedId = modelData.toolId
                    }
                }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
            }

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                color: theme.border
            }

            /* ---- detail ------------------------------------------------- */
            Flickable {
                id: detail

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: width
                contentHeight: detailColumn.implicitHeight + 2 * VLCStyle.margin_large
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: detailColumn

                    x: VLCStyle.margin_large
                    y: VLCStyle.margin_large
                    width: detail.width - 2 * VLCStyle.margin_large
                    spacing: VLCStyle.margin_small

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_normal

                        Image {
                            visible: !!root.currentEntry && root.currentEntry.iconSource !== ""
                            source: (!!root.currentEntry && root.currentEntry.iconSource !== "")
                                    ? root.tdesignPath + root.currentEntry.iconSource + "_active.svg"
                                    : ""
                            Layout.preferredWidth: VLCStyle.dp(48, VLCStyle.scale)
                            Layout.preferredHeight: VLCStyle.dp(48, VLCStyle.scale)
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                        }
                        Widgets.IconLabel {
                            visible: !root.currentEntry || root.currentEntry.iconSource === ""
                            text: root.currentEntry ? root.currentEntry.iconGlyph : ""
                            font.pixelSize: VLCStyle.fontSize_xxxlarge
                            color: theme.accent
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: root.current.name
                            font.pixelSize: VLCStyle.fontSize_xxlarge
                            color: theme.fg.primary
                        }
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        Layout.topMargin: VLCStyle.margin_xsmall
                        text: qsTr("WHAT IT DOES")
                        font.pixelSize: VLCStyle.fontSize_small
                        color: theme.accent
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: root.current.what
                        font.pixelSize: VLCStyle.fontSize_large
                        color: theme.fg.primary
                        wrapMode: Text.WordWrap
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        Layout.topMargin: VLCStyle.margin_xsmall
                        visible: !!root.current.use && root.current.use !== ""
                        text: qsTr("USE IT FOR")
                        font.pixelSize: VLCStyle.fontSize_small
                        color: theme.accent
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        visible: !!root.current.use && root.current.use !== ""
                        text: root.current.use
                        font.pixelSize: VLCStyle.fontSize_large
                        color: theme.fg.primary
                        wrapMode: Text.WordWrap
                    }

                    /* the intro is a front page, not a tool: offer a direct
                     * jump to the page for whatever the user was using */
                    Widgets.ButtonExt {
                        Layout.fillWidth: false
                        visible: root.selectedId === root.introId
                                 && !!root.guideFor(root.activeToolId)
                        text: {
                            const g = root.guideFor(root.activeToolId)
                            return g ? qsTr("Explain my current tool — %1").arg(g.name) : ""
                        }
                        onClicked: root.selectedId = root.activeToolId
                    }

                    /* users understand the sliding controls first: name + effect */
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        Layout.topMargin: VLCStyle.margin_xsmall
                        visible: !root.current.subtools && !root.current.sections
                        text: qsTr("CONTROLS")
                        font.pixelSize: VLCStyle.fontSize_small
                        color: theme.accent
                    }

                    Repeater {
                        model: root.current && root.current.controls ? root.current.controls : []

                        delegate: ColumnLayout {
                            id: controlRow

                            Layout.fillWidth: true
                            spacing: 1

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: modelData.name
                                font.pixelSize: VLCStyle.fontSize_normal
                                color: theme.fg.primary
                            }
                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: modelData.what
                                font.pixelSize: VLCStyle.fontSize_normal
                                color: theme.fg.secondary
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        visible: (!root.current.controls || root.current.controls.length === 0)
                                 && !root.current.subtools
                                 && !root.current.sections
                        text: qsTr("No sliders — just click or drag on the canvas.")
                        font.pixelSize: VLCStyle.fontSize_normal
                        color: theme.fg.secondary
                    }

                    /* Glitter Powder: the six bench tools are their own cards */
                    Repeater {
                        id: subtoolRepeater
                        model: root.current && root.current.subtools ? root.current.subtools : []

                        delegate: Rectangle {
                            id: subCard

                            property var sub: modelData

                            Layout.fillWidth: true
                            implicitHeight: subBody.implicitHeight + 2 * VLCStyle.margin_small
                            radius: VLCStyle.dp(10, VLCStyle.scale)
                            color: theme.bg.secondary
                            border.width: 1
                            border.color: theme.border

                            ColumnLayout {
                                id: subBody

                                x: VLCStyle.margin_small
                                y: VLCStyle.margin_small
                                width: subCard.width - 2 * VLCStyle.margin_small
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: VLCStyle.margin_small

                                    Image {
                                        source: subCard.sub.icon
                                                ? root.tdesignPath + subCard.sub.icon + ".svg"
                                                : ""
                                        Layout.preferredWidth: VLCStyle.dp(18, VLCStyle.scale)
                                        Layout.preferredHeight: VLCStyle.dp(18, VLCStyle.scale)
                                        fillMode: Image.PreserveAspectFit
                                        smooth: true
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: subCard.sub.name
                                        font.pixelSize: VLCStyle.fontSize_normal
                                        color: theme.fg.primary
                                    }
                                }
                                Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    text: subCard.sub.what
                                    font.pixelSize: VLCStyle.fontSize_normal
                                    color: theme.fg.secondary
                                    wrapMode: Text.WordWrap
                                }
                                Repeater {
                                    model: subCard.sub.controls ? subCard.sub.controls : []
                                    delegate: Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: "• " + modelData.name + " — " + modelData.what
                                        font.pixelSize: VLCStyle.fontSize_small
                                        color: theme.fg.secondary
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }

                    /* Bars & buttons: grouped one-liners */
                    Repeater {
                        model: root.current && root.current.sections ? root.current.sections : []

                        delegate: ColumnLayout {
                            id: sectionBlock

                            property var section: modelData

                            Layout.fillWidth: true
                            Layout.topMargin: VLCStyle.margin_xsmall
                            spacing: 2

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: sectionBlock.section.title
                                font.pixelSize: VLCStyle.fontSize_normal
                                color: theme.accent
                            }
                            Repeater {
                                model: sectionBlock.section.lines
                                delegate: Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    text: "• " + modelData
                                    font.pixelSize: VLCStyle.fontSize_normal
                                    color: theme.fg.primary
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: VLCStyle.margin_small
                        visible: !!root.current.tip && root.current.tip !== ""
                        spacing: VLCStyle.margin_small

                        Rectangle {
                            Layout.preferredWidth: VLCStyle.dp(3, VLCStyle.scale)
                            Layout.fillHeight: true
                            radius: 1.5
                            color: theme.accent
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: qsTr("Good to know — ") + (root.current.tip || "")
                            font.pixelSize: VLCStyle.fontSize_normal
                            color: theme.fg.primary
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }
            }
        }

        Rectangle {
            id: cardFooter

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: VLCStyle.dp(44, VLCStyle.scale)
            color: "transparent"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: theme.border
            }

            Widgets.LabelExt {
                anchors.left: parent.left
                anchors.leftMargin: VLCStyle.margin_large
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Hover any tool for its name  ·  Scroll for more  ·  F1 opens this guide  ·  Esc closes it")
                font.pixelSize: VLCStyle.fontSize_small
                color: theme.fg.secondary
            }
        }
    }
}
