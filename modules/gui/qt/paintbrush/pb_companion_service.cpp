/*****************************************************************************
 * pb_companion_service.cpp : NAI Paintbrush — creative companion
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_companion_service.hpp"
#include "pb_assistant_service.hpp"
#include "pb_common.hpp"

#include <QDateTime>

namespace paintbrush {

CompanionService::CompanionService(QObject *parent)
    : QObject(parent)
{
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

bool CompanionService::analysisAllowed() const
{
    if (m_mode == MODE_OFF)
        return false;
    if (!m_networkAllowed)
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
            "You are a warm, playful art buddy talking with a child. "
            "Look at their drawing. Give ONE short, specific, positive observation "
            "about something actually visible (a color, a shape, a detail). "
            "Then ask ONE open-ended question about it. Use simple words. "
            "Never say the drawing is wrong, never score it, never rank it. "
            "Keep it to at most two short sentences. No emoji spam.");
    }
    else
    {
        prompt = QStringLiteral(
            "You are a creative companion sitting beside someone making art in a paint program. "
            "Look at the canvas. Give ONE concise, specific observation about something "
            "actually visible (color choice, composition, a shape, a repeated motif, a "
            "new object). Adults find generic praise patronizing, so be concrete and honest. "
            "Then optionally ask ONE open question that could inspire the next step. "
            "Keep it to one or two sentences. Do not take over the canvas.");
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
            "You are a warm, playful art buddy talking with a child about their drawing. "
            "Answer their question simply and kindly. Be encouraging and specific. "
            "Never judge or score the drawing. Keep it short (1-3 sentences).");
    }
    else
    {
        prompt = QStringLiteral(
            "You are a creative companion in a paint program. Answer the user's question "
            "about their artwork or process. Be specific, honest and concise. "
            "You may suggest one small idea, but never take over the canvas.");
    }
    prompt += QStringLiteral("\nQuestion: %1").arg(question);
    return prompt;
}

void CompanionService::runAnalysis(const QString &prompt, const QImage &preview)
{
    if (!analysisAllowed())
        return;

    /* throttle: respect the minimum interval between analyses */
    if (m_hasLastAnalysis && m_lastAnalysis.elapsed() < m_minIntervalMs)
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

    /* keep a compact recent-activity ring */
    m_recentEvents.append(event);
    while (m_recentEvents.size() > 6)
        m_recentEvents.removeFirst();

    runAnalysis(buildObservationPrompt(event), preview);
}

void CompanionService::ask(const QString &question, const QImage &preview)
{
    if (m_mode == MODE_OFF)
        return;
    runAnalysis(buildQuestionPrompt(question), preview);
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