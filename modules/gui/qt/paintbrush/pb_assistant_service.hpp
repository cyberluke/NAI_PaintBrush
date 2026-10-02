/*****************************************************************************
 * pb_assistant_service.hpp : NAI Paintbrush — AI assistant chat provider
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ASSISTANT_SERVICE_HPP
#define PB_ASSISTANT_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QList>

#include <vlc_common.h>

class QNetworkReply;

namespace paintbrush {

/* OpenAI-compatible chat provider backing the in-app AI assistant
 * (companion dock).
 *
 * The endpoint is configured through the persisted `paintbrush-assistant-*`
 * options (base URL + model), with the model profile (reasoning, token
 * limits, compaction policy) read from the same option group:
 *
 *   base URL : http://127.0.0.1:4000/v1   (LiteLLM Edge gateway)
 *   model    : differential_sseproxy
 *
 * Conversation history is kept in-process and compacted automatically when
 * the estimated input usage crosses the configured threshold: the oldest
 * turns are pruned (keeping at least the tail turns and a recent-token
 * floor) so long chat sessions stay within the model's input limit. */
class AssistantService : public QObject
{
    Q_OBJECT

public:
    explicit AssistantService(QObject *parent = nullptr);
    ~AssistantService() override;

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return !m_baseUrl.isEmpty(); }
    bool isBusy() const { return m_busy; }
    QString baseUrl() const { return m_baseUrl; }
    QString model() const { return m_model; }
    QString unavailableReason() const { return m_unavailableReason; }
    int timeoutMs() const { return m_timeoutMs; }
    bool reasoning() const { return m_reasoning; }

    /* model limits (tokens) */
    int contextLimit() const { return m_contextLimit; }
    int inputLimit() const { return m_inputLimit; }
    int outputLimit() const { return m_outputLimit; }

    /* compaction policy */
    bool compactionAuto() const { return m_compactionAuto; }
    int compactionThresholdPercent() const { return m_compactionThreshold; }
    bool compactionPrune() const { return m_compactionPrune; }
    int compactionTailTurns() const { return m_compactionTailTurns; }
    int compactionPreserveRecentTokens() const { return m_compactionPreserveRecent; }
    int compactionReserved() const { return m_compactionReserved; }

    /* Send a user message, optionally with an attached canvas/frame image.
     * The message is appended to the in-process conversation history which
     * is compacted before the request is sent. conversationId correlates
     * the asynchronous reply with the UI turn. */
    void chat(const QString &text, const QImage &image,
              const QString &conversationId);
    void resetConversation();
    void cancelAll();
    int conversationMessageCount() const { return m_history.size(); }

signals:
    void busyChanged(bool busy);
    void responseReady(const QString &conversationId, const QString &text);
    void errorOccurred(const QString &conversationId, const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private slots:
    void onReplyFinished();

private:
    QByteArray buildPayload(const QString &text, const QImage &image) const;
    void compactConversation();
    int estimateTokens(const QJsonObject &message) const;
    static QString extractContent(const QByteArray &json);

    vlc_object_t *m_obj = nullptr;
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_unavailableReason;
    int m_timeoutMs = 120000;
    bool m_reasoning = true;

    /* model limits (tokens), defaults match the persisted profile */
    int m_contextLimit = 524288;
    int m_inputLimit = 442368;
    int m_outputLimit = 65536;

    /* compaction policy */
    bool m_compactionAuto = true;
    int m_compactionThreshold = 90;          /* percent of the input budget */
    bool m_compactionPrune = true;
    int m_compactionTailTurns = 4;
    int m_compactionPreserveRecent = 16536;  /* token floor kept when pruning */
    int m_compactionReserved = 20000;        /* input tokens reserved for output */

    bool m_busy = false;

    /* OpenAI-style message history: {"role": ..., "content": ...} */
    QList<QJsonObject> m_history;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_ASSISTANT_SERVICE_HPP