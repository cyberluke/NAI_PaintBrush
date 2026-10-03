/*****************************************************************************
 * pb_companion_service.hpp : NAI Paintbrush — creative companion
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_COMPANION_SERVICE_HPP
#define PB_COMPANION_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QElapsedTimer>
#include <QStringList>

#include <vlc_common.h>

namespace paintbrush {

class AssistantService;

/* The companion is a creative witness: it observes meaningful canvas
 * changes (never every stroke), gives specific observations rather than
 * generic praise, asks open questions, and never edits the picture itself.
 *
 * Modes:
 *   Off    — no observation, no inference, no background work.
 *   Quiet  — only replies when the user opens the assistant or asks.
 *   Buddy  — occasional concise feedback after meaningful changes.
 *   Kids   — warmer, shorter, more questions; network AI off by default.
 *
 * All inference goes through the OpenAI-compatible AssistantService (the
 * persisted LiteLLM Edge gateway), which keeps the conversation history
 * and compacts it automatically. Analysis is throttled by a minimum
 * interval and a hard concurrency limit of one analysis per document. */
class CompanionService : public QObject
{
    Q_OBJECT

public:
    enum Mode {
        MODE_OFF = 0,
        MODE_QUIET,
        MODE_BUDDY,
        MODE_KIDS
    };
    Q_ENUM(Mode)

    explicit CompanionService(QObject *parent = nullptr);

    void configure(vlc_object_t *obj);
    void setAssistantService(AssistantService *assistant);
    int mode() const { return m_mode; }
    void setMode(int mode);
    int minIntervalMs() const { return m_minIntervalMs; }
    void setMinIntervalMs(int ms);
    bool isThinking() const { return m_thinking; }
    bool networkAllowed() const { return m_networkAllowed; }
    void setNetworkAllowed(bool allowed);
    /* Talk level: 0 = "Only when I ask", 1 = "Talk less", 2 = default. */
    int talkLevel() const { return m_talkLevel; }
    void setTalkLevel(int level);

    /* Meaningful canvas events (aggregated by the controller). */
    void notifyEvent(const QString &event, const QImage &preview);
    /* Explicit user question ("Look at my drawing", follow-ups...). */
    void ask(const QString &question, const QImage &preview);
    void resetConversation();

signals:
    void modeChanged(int mode);
    void thinkingChanged(bool thinking);
    void messageReady(const QString &text, const QString &role);
    void errorOccurred(const QString &message);
    void conversationChanged();

private slots:
    void onAssistantResponse(const QString &conversationId, const QString &answer);
    void onAssistantError(const QString &conversationId, const QString &message);

private:
    bool analysisAllowed() const;
    void runAnalysis(const QString &prompt, const QImage &preview,
                     bool force = false);
    QString buildObservationPrompt(const QString &event) const;
    QString buildQuestionPrompt(const QString &question) const;

    vlc_object_t *m_obj = nullptr;
    AssistantService *m_assistant = nullptr;

    int m_mode = MODE_BUDDY;
    int m_minIntervalMs = 30000;
    int m_talkLevel = 2;             /* 0 = only when asked, 1 = less, 2 = normal */
    bool m_thinking = false;
    bool m_networkAllowed = true;    /* kids mode flips this off by default */

    QElapsedTimer m_lastAnalysis;
    bool m_hasLastAnalysis = false;

    QStringList m_recentEvents;
    QString m_lastObservation;
    QStringList m_conversation;      /* compact context: last few turns */
};

} // namespace paintbrush

#endif // PB_COMPANION_SERVICE_HPP