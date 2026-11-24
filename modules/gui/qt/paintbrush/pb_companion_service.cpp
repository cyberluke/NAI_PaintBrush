/*****************************************************************************
 * pb_companion_service.cpp : NAI Paintbrush — creative companion
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_companion_service.hpp"
#include "pb_assistant_service.hpp"
#include "pb_common.hpp"

#include <vlc_configuration.h>

#include <QDateTime>

namespace paintbrush {

namespace {

bool companionIsKids(int mode)
{
    return mode == CompanionService::MODE_KIDS;
}

/* The deterministic path never claims to have seen pixels it has not seen;
 * it talks about the editor state the child actually created. */
CompanionService::Suggestion makeSuggestion(const QString &kind,
                                            const QString &text,
                                            const QString &action,
                                            const QString &actionLabel)
{
    CompanionService::Suggestion s;
    s.kind = kind;
    s.text = text;
    s.action = action;
    s.actionLabel = actionLabel;
    return s;
}

} // namespace

CompanionService::CompanionService(QObject *parent)
    : QObject(parent)
{
}

void CompanionService::setContext(const QVariantMap &context)
{
    m_context = context;
}

CompanionService::Suggestion
CompanionService::suggestionForContext(const QVariantMap &context, int mode)
{
    if (mode != MODE_BUDDY && mode != MODE_KIDS)
        return {};
    if (!context.value(QStringLiteral("hasDocument"), true).toBool())
        return {};

    const bool kids = companionIsKids(mode);
    const bool blank = context.value(QStringLiteral("blank"), false).toBool();
    const bool object = context.value(QStringLiteral("objectSelected"), false).toBool();
    const bool selection = context.value(QStringLiteral("hasSelection"), false).toBool();
    const int layerCount = context.value(QStringLiteral("layerCount"), 0).toInt();
    const int lookCount = context.value(QStringLiteral("lookCount"), 0).toInt();
    const int lookIndex = context.value(QStringLiteral("lastLookLayerIndex"), -1).toInt();
    const bool lookOpen = context.value(QStringLiteral("lookOpen"), false).toBool();
    const int tool = context.value(QStringLiteral("tool"), -1).toInt();

    if (blank)
    {
        return kids
                   ? makeSuggestion(
                         QStringLiteral("blank"),
                         QStringLiteral("Big empty page! What color feels like today? "
                                        "Draw one big shape with it."),
                         QStringLiteral("tool:5"), QStringLiteral("Grab a brush"))
                   : makeSuggestion(
                         QStringLiteral("blank"),
                         QStringLiteral("Fresh page. Pick a color you like and make "
                                        "one big mark — you can always undo it."),
                         QStringLiteral("tool:5"), QStringLiteral("Grab a brush"));
    }

    if (object)
    {
        return kids
                   ? makeSuggestion(
                         QStringLiteral("object"),
                         QStringLiteral("You found a shape all on its own. Want to "
                                        "dress it up in a wild new look?"),
                         QStringLiteral("open-pad"), QStringLiteral("Play with a look"))
                   : makeSuggestion(
                         QStringLiteral("object"),
                         QStringLiteral("That cutout has its own silhouette. Want to "
                                        "see it in a wild new look?"),
                         QStringLiteral("open-pad"), QStringLiteral("Open Play Pad"));
    }

    if (selection)
    {
        return kids
                   ? makeSuggestion(
                         QStringLiteral("selection"),
                         QStringLiteral("You picked a little piece. Want to make it "
                                        "sparkle?"),
                         QStringLiteral("tool:17"), QStringLiteral("Try glitter"))
                   : makeSuggestion(
                         QStringLiteral("selection"),
                         QStringLiteral("You’ve got a piece picked out. Try a little "
                                        "glitter inside it and see what catches the light."),
                         QStringLiteral("tool:17"), QStringLiteral("Try glitter"));
    }

    switch (tool)
    {
    case TOOL_ERASER:
    case TOOL_SMEAR:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Oops eraser! Want to switch to a brush and "
                                        "add something new?"),
                         QStringLiteral("tool:5"), QStringLiteral("Try the brush"))
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Eraser is handy. Switch to a brush and add "
                                        "one bright bit back in?"),
                         QStringLiteral("tool:5"), QStringLiteral("Try the brush"));
    case TOOL_FILL:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Nice color! Want to draw one line on top?"),
                         QStringLiteral("tool:4"), QStringLiteral("Try the pencil"))
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Fill gets the base down fast. One hand-drawn "
                                        "line on top would make it feel like yours."),
                         QStringLiteral("tool:4"), QStringLiteral("Try the pencil"));
    case TOOL_TEXT:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Words can be part of the picture. What word "
                                        "fits?"),
                         QStringLiteral("none"), QString())
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Words count as drawing too. Pick one short word "
                                        "that fits the mood."),
                         QStringLiteral("none"), QString());
    case TOOL_GLITTER:
    case TOOL_GLITTER_POWDER:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Sparkle time! One tiny sprinkle is enough."),
                         QStringLiteral("none"), QString())
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Glitter time. One tiny sprinkle near a corner "
                                        "is enough to catch the light."),
                         QStringLiteral("none"), QString());
    case TOOL_SPRAY:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Soft air! Want to spray a little color behind "
                                        "your shape?"),
                         QStringLiteral("none"), QString())
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Spray makes soft air. One short burst behind "
                                        "your subject gives it space."),
                         QStringLiteral("none"), QString());
    case TOOL_RECT_SELECT:
    case TOOL_FREEFORM_SELECT:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("You can pick just one spot. Then fill it with a "
                                        "color you love."),
                         QStringLiteral("none"), QString())
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Selections let you be precise. Pick a spot, "
                                        "then try a color inside it."),
                         QStringLiteral("none"), QString());
    case TOOL_PAN_ZOOM:
        return kids
                   ? makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Move things until the picture feels right. A "
                                        "little space can change the whole scene."),
                         QStringLiteral("none"), QString())
                   : makeSuggestion(
                         QStringLiteral("tool"),
                         QStringLiteral("Move things around until the picture feels "
                                        "right. A little space can change the scene."),
                         QStringLiteral("none"), QString());
    default:
        break;
    }

    if (lookCount > 0 && lookIndex >= 0 && !lookOpen)
    {
        return kids
                   ? makeSuggestion(
                         QStringLiteral("look"),
                         QStringLiteral("Your last look was fun. Want to make another "
                                        "one?"),
                         QStringLiteral("resume-look:%1").arg(lookIndex),
                         QStringLiteral("Open my look"))
                   : makeSuggestion(
                         QStringLiteral("look"),
                         QStringLiteral("Your last look is still saved. Want to stir it "
                                        "up and make a new version?"),
                         QStringLiteral("resume-look:%1").arg(lookIndex),
                         QStringLiteral("Reopen that look"));
    }

    if (layerCount <= 1)
    {
        return kids
                   ? makeSuggestion(
                         QStringLiteral("layer"),
                         QStringLiteral("One sheet is getting full. Want a new sheet to "
                                        "try a new color?"),
                         QStringLiteral("new-layer"), QStringLiteral("Add a layer"))
                   : makeSuggestion(
                         QStringLiteral("layer"),
                         QStringLiteral("Everything is sharing one sheet. A fresh layer "
                                        "lets you test a wild color without touching what "
                                        "you made."),
                         QStringLiteral("new-layer"), QStringLiteral("Add a layer"));
    }

    return kids
               ? makeSuggestion(
                     QStringLiteral("detail"),
                     QStringLiteral("This is becoming something. What’s one tiny thing "
                                    "you want to add?"),
                     QStringLiteral("tool:4"), QStringLiteral("Try the pencil"))
               : makeSuggestion(
                     QStringLiteral("detail"),
                     QStringLiteral("There’s something real here. Want to add one tiny "
                                    "detail — a star, a word, a dot — where your eye "
                                    "goes first?"),
                     QStringLiteral("tool:4"), QStringLiteral("Try the pencil"));
}

QString CompanionService::offlineObservation(const QVariantMap &context, int mode,
                                             const QString &event)
{
    const Suggestion step = suggestionForContext(context, mode);
    const bool kids = companionIsKids(mode);

    QString praise;
    if (event == QLatin1String("cutout-added")
        || event == QLatin1String("object-extracted")
        || event == QLatin1String("sticker-made"))
    {
        praise = kids ? QStringLiteral("You pulled that shape out — it can go "
                                       "anywhere now.")
                      : QStringLiteral("You pulled that shape out cleanly — it can "
                                       "go anywhere now.");
    }
    else if (event == QLatin1String("creative-made-real"))
    {
        praise = kids ? QStringLiteral("You made that look real, and it’s still "
                                       "yours to change.")
                      : QStringLiteral("You made that look real, and every piece is "
                                       "still yours to change.");
    }
    else if (event == QLatin1String("creative-stroke"))
    {
        praise = kids ? QStringLiteral("You added a line of your own.")
                      : QStringLiteral("You added a line of your own — it’s still "
                                       "editable.");
    }
    else if (event == QLatin1String("creative-calligraphy"))
    {
        praise = QStringLiteral("You traced its real outline — that’s yours to edit.");
    }
    else if (event == QLatin1String("boost-applied"))
    {
        praise = QStringLiteral("You gave it a whole new finish.");
    }
    else if (event == QLatin1String("image-inserted")
             || event == QLatin1String("media-frame-captured"))
    {
        praise = kids ? QStringLiteral("That moment is inside your picture now.")
                      : QStringLiteral("That moment is part of the picture now.");
    }
    else if (event == QLatin1String("lineup")
             || event == QLatin1String("motion-echo")
             || event == QLatin1String("smart-duplicate"))
    {
        praise = kids ? QStringLiteral("One pose just became a whole crew.")
                      : QStringLiteral("One pose just became a whole crew — every "
                                       "copy is still editable.");
    }
    else if (event == QLatin1String("object-tracked"))
    {
        praise = QStringLiteral("That shape moves with the video now.");
    }
    else if (event == QLatin1String("collage"))
    {
        praise = QStringLiteral("The pieces have a place together now.");
    }
    else if (event == QLatin1String("ai-result"))
    {
        praise = QStringLiteral("The new piece landed.");
    }
    else if (event == QLatin1String("expand-canvas"))
    {
        praise = QStringLiteral("More room to play now.");
    }
    else if (event == QLatin1String("harvest-kept"))
    {
        praise = QStringLiteral("You caught that moment.");
    }

    if (praise.isEmpty())
        return step.text;
    if (step.text.isEmpty())
        return praise;
    return praise + QLatin1Char(' ') + step.text;
}

QString CompanionService::offlineAnswer(const QVariantMap &context, int mode)
{
    const Suggestion step = suggestionForContext(context, mode);
    QString answer = companionIsKids(mode)
                         ? QStringLiteral("My picture magic is resting, but I’m still "
                                          "here. ")
                         : QStringLiteral("I can’t peek at the canvas right now, but "
                                          "I’m still here to play. ");
    if (!step.text.isEmpty())
        answer += step.text;
    else
        answer += companionIsKids(mode)
                      ? QStringLiteral("What do you want to make next?")
                      : QStringLiteral("Want to try one small experiment?");
    return answer;
}

void CompanionService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    const int mode = pbConfigInt(obj, "paintbrush-companion-mode");
    setMode(mode >= MODE_OFF && mode <= MODE_KIDS ? mode : MODE_BUDDY);
    setMinIntervalMs(pbConfigInt(obj, "paintbrush-companion-min-interval") * 1000);
    if (m_minIntervalMs < 5000)
        m_minIntervalMs = 5000;

    /* Kids mode keeps content local unless a responsible adult explicitly
     * enables network AI. */
    m_networkAllowed = pbConfigBool(obj, "paintbrush-kids-network");
    m_talkLevel = qBound(0, pbConfigInt(obj, "paintbrush-companion-talk"), 2);
}

void CompanionService::setAssistantService(AssistantService *assistant)
{
    m_assistant = assistant;
}

void CompanionService::setMode(int mode)
{
    if (mode < MODE_OFF || mode > MODE_KIDS)
        return;
    if (m_mode == mode)
        return;
    m_mode = mode;
    emit modeChanged(mode);
}

void CompanionService::setMinIntervalMs(int ms)
{
    m_minIntervalMs = qMax(5000, ms);
}

void CompanionService::setNetworkAllowed(bool allowed)
{
    if (m_networkAllowed == allowed)
        return;
    m_networkAllowed = allowed;
}

void CompanionService::setTalkLevel(int level)
{
    level = qBound(0, level, 2);
    if (m_talkLevel == level)
        return;
    m_talkLevel = level;
    if (m_obj)
        config_PutInt("paintbrush-companion-talk", level);
}

bool CompanionService::analysisAllowed() const
{
    if (m_mode == MODE_OFF)
        return false;
    /* Kids mode keeps content local unless a responsible adult explicitly
     * enables network AI; the other modes use the configured assistant. */
    if (m_mode == MODE_KIDS && !m_networkAllowed)
        return false;
    if (!m_assistant || !m_assistant->isConfigured())
        return false;
    if (m_thinking)
        return false; /* hard concurrency limit: one analysis per document */
    return true;
}

void CompanionService::resetConversation()
{
    m_conversation.clear();
    m_lastObservation.clear();
    m_lastOfflineText.clear();
    if (m_assistant)
        m_assistant->resetConversation();
    emit conversationChanged();
}

QString CompanionService::buildObservationPrompt(const QString &event) const
{
    const bool kids = (m_mode == MODE_KIDS);
    QString prompt;
    if (kids)
    {
        prompt = QStringLiteral(
            "You are Buddy, a warm, playful art buddy talking with a child. "
            "Look at their drawing. Say ONE true, specific thing you can see — a "
            "color, a shape, a small detail. Then ask ONE small question or offer "
            "ONE tiny next step they could try. Use simple words. Never score, "
            "rank, or say the drawing is wrong. Two short sentences at most. "
            "No emoji.");
    }
    else
    {
        prompt = QStringLiteral(
            "You are Buddy, a warm, playful creative companion sitting beside a kid "
            "or teen making art. Look at the canvas. Say ONE true, specific thing "
            "you can actually see — a color choice, a shape, a repeated motif, a new "
            "object. Then offer ONE tiny next step as a question, not an instruction. "
            "Be concrete and honest; never generic praise, never score or rank. One "
            "or two sentences. Do not take over the canvas. No emoji.");
    }

    prompt += QStringLiteral("\nRecent activity: %1\nLast observation: %2")
                  .arg(m_recentEvents.isEmpty() ? QStringLiteral("just started")
                                                : m_recentEvents.join(QStringLiteral(", ")),
                       m_lastObservation.isEmpty() ? QStringLiteral("none")
                                                   : m_lastObservation);
    return prompt;
}

QString CompanionService::buildQuestionPrompt(const QString &question) const
{
    const bool kids = (m_mode == MODE_KIDS);
    QString prompt;
    if (kids)
    {
        prompt = QStringLiteral(
            "You are Buddy, a warm, playful art buddy talking with a child about "
            "their drawing. Answer simply and kindly. Notice one real thing in the "
            "picture, then give one tiny idea if it helps. Never score or judge the "
            "drawing. One to three short sentences. No emoji.");
    }
    else
    {
        prompt = QStringLiteral(
            "You are Buddy, a warm, playful creative companion in a paint program. "
            "Answer the question about their artwork or process. Be specific, honest "
            "and concise. Praise only what is actually there, then suggest one small "
            "next step if it fits. Never take over the canvas. One to three short "
            "sentences. No emoji.");
    }
    prompt += QStringLiteral("\nQuestion: %1").arg(question);
    return prompt;
}

void CompanionService::runAnalysis(const QString &prompt, const QImage &preview,
                                   bool force)
{
    if (!analysisAllowed())
        return;

    /* throttle: respect the minimum interval between analyses. Explicit
     * user questions are never throttled — the user asked, we answer. */
    if (!force && m_hasLastAnalysis && m_lastAnalysis.elapsed() < m_minIntervalMs)
        return;
    m_lastAnalysis.restart();
    m_hasLastAnalysis = true;

    m_thinking = true;
    emit thinkingChanged(true);

    const QString convId = QStringLiteral("companion-%1")
                               .arg(QDateTime::currentMSecsSinceEpoch());
    m_conversation.append(prompt.left(200));
    emit conversationChanged();

    connect(m_assistant, &AssistantService::responseReady, this,
            &CompanionService::onAssistantResponse, Qt::UniqueConnection);
    connect(m_assistant, &AssistantService::errorOccurred, this,
            &CompanionService::onAssistantError, Qt::UniqueConnection);

    m_assistant->chat(prompt, preview, convId);
}

void CompanionService::notifyEvent(const QString &event, const QImage &preview)
{
    if (m_mode == MODE_OFF || m_mode == MODE_QUIET)
        return;
    /* "Only when I ask" — never speak spontaneously */
    if (m_talkLevel == 0)
        return;

    /* keep a compact recent-activity ring */
    m_recentEvents.append(event);
    while (m_recentEvents.size() > 6)
        m_recentEvents.removeFirst();

    if (m_thinking)
        return;

    /* silence discipline: after the last comment, wait at least one more
     * interval before speaking again, so we never nag while the user is
     * absorbed. "Talk less" doubles the quiet period. */
    if (m_hasLastAnalysis)
    {
        const qint64 quiet = m_minIntervalMs * (m_talkLevel == 1 ? 2 : 1);
        if (m_lastAnalysis.elapsed() < quiet)
            return;
    }

    /* Deterministic fallback: with no provider, or with Kids network AI off,
     * Buddy can still react using real editor state. No model, no network,
     * no invented pixels. The provider path above is untouched whenever it
     * is actually available. */
    if (!analysisAllowed())
    {
        const bool offlineByPolicy = (m_mode == MODE_KIDS && !m_networkAllowed);
        const bool offlineByConfig = !m_assistant || !m_assistant->isConfigured();
        if (!offlineByPolicy && !offlineByConfig)
            return;

        const QString text = offlineObservation(m_context, m_mode, event);
        if (text.isEmpty() || text == m_lastOfflineText)
            return;

        m_lastOfflineText = text;
        m_lastObservation = text.left(200);
        m_conversation.append(QStringLiteral("assistant: %1").arg(text.left(200)));
        m_lastAnalysis.restart();
        m_hasLastAnalysis = true;
        emit messageReady(text, QStringLiteral("assistant"));
        emit conversationChanged();
        return;
    }

    runAnalysis(buildObservationPrompt(event), preview);
}

void CompanionService::ask(const QString &question, const QImage &preview)
{
    if (m_mode == MODE_OFF)
        return;
    runAnalysis(buildQuestionPrompt(question), preview, /*force=*/true);
}

void CompanionService::onAssistantResponse(const QString &conversationId,
                                           const QString &answer)
{
    Q_UNUSED(conversationId);
    m_thinking = false;
    emit thinkingChanged(false);
    m_lastObservation = answer.left(200);
    m_conversation.append(QStringLiteral("assistant: %1").arg(answer.left(200)));
    emit messageReady(answer, QStringLiteral("assistant"));
    emit conversationChanged();
}

void CompanionService::onAssistantError(const QString &conversationId,
                                        const QString &message)
{
    Q_UNUSED(conversationId);
    m_thinking = false;
    emit thinkingChanged(false);
    emit errorOccurred(message);
}

} // namespace paintbrush