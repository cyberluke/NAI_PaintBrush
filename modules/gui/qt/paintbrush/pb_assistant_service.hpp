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
#include "pb_capability.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QVariantList>
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

    /* Hot-apply a new API key without reconfiguring the endpoint. Requests
     * read the key on the GUI thread when they are built, so assigning it is
     * safe while other replies are in flight. Never logged. */
    void setApiKey(const QString &key) { m_apiKey = key; }

    /* TLS posture (A7): certificate validation is on unless the operator
     * explicitly opts into self-signed endpoints. */
    bool insecureTlsOptIn() const { return m_insecureTls; }
    TlsPosture tlsPosture() const
    {
        return m_insecureTls ? TlsPosture::DevSelfSigned : TlsPosture::Normal;
    }

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

    /* ------------------------------------------------------------------
     * Streaming OpenAI-compatible chat (the companion surface).
     *
     * `chatStreaming` sends `stream: true` and emits content/reasoning deltas
     * as they arrive. If the model requests tools, `toolCallsReady` fires and
     * the caller executes them and calls `submitToolResults`; the service then
     * appends the assistant tool_calls message + tool results and continues
     * the SAME conversation. Everything is standard OpenAI chat completions:
     * SSE `data:` chunks, delta.tool_calls fragments, role:"tool" results.
     * ------------------------------------------------------------------ */
    void chatStreaming(const QString &text, const QImage &image,
                       const QString &conversationId,
                       const QString &systemPrompt = QString());
    void submitToolResults(const QString &conversationId,
                           const QVariantList &results);
    void cancelStreaming(const QString &conversationId);
    /* JSON array of OpenAI tool definitions; empty clears them. */
    void setToolDefinitions(const QByteArray &jsonArray);
    bool isStreaming() const { return m_stream.reply != nullptr; }

    void resetConversation();
    void cancelAll();
    int conversationMessageCount() const { return m_history.size(); }

signals:
    void busyChanged(bool busy);
    void responseReady(const QString &conversationId, const QString &text);
    void errorOccurred(const QString &conversationId, const QString &message);
    void availabilityChanged(bool available, const QString &reason);
    /* streaming */
    void streamStarted(const QString &conversationId);
    void streamRoundStarted(const QString &conversationId);
    void contentDelta(const QString &conversationId, const QString &text);
    void reasoningDelta(const QString &conversationId, const QString &text);
    void toolCallStarted(const QString &conversationId, const QString &id,
                         const QString &name);
    void toolCallsReady(const QString &conversationId,
                        const QVariantList &calls);
    void streamFinished(const QString &conversationId, const QString &text);
    void streamError(const QString &conversationId, const QString &message);

private slots:
    void onReplyFinished();
    void onStreamReadyRead();
    void onStreamFinished();

private:
    QByteArray buildPayload(const QString &text, const QImage &image) const;
    QJsonObject buildUserMessage(const QString &text, const QImage &image) const;
    QByteArray buildStreamPayload(const QJsonArray &messages) const;
    void startStreamRequest(const QString &conversationId, int round);
    void parseStreamLine(const QByteArray &line);
    void processStreamObject(const QJsonObject &obj);
    void finishStreamRound();
    void finishStreamAnswer(const QString &content);
    void failStream(const QString &message);
    void resetStreamState();
    void compactConversation();
    int estimateTokens(const QJsonObject &message) const;
    static QString extractContent(const QByteArray &json);
    /* A7: TLS validation gating shared by all replies. */
    void handleSslErrors(QNetworkReply *reply) const;

    vlc_object_t *m_obj = nullptr;
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_unavailableReason;
    int m_timeoutMs = 120000;
    bool m_reasoning = true;
    bool m_insecureTls = false;

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
    /* Streaming companion history (system + turns + tool messages). Kept
     * separate from m_history so proactive non-streaming observations never
     * receive role:"tool" messages without a tools request. */
    QList<QJsonObject> m_streamHistory;

    /* OpenAI tool definitions (JSON array text); empty = no tools. */
    QByteArray m_toolDefinitions;

    /* One active streaming turn. The UI sends one chat at a time; the
     * companion's proactive non-streaming analysis uses `chat()` and its
     * own reply list, so the two never share this state. */
    struct StreamState
    {
        QNetworkReply *reply = nullptr;
        QString conversationId;
        QString systemPrompt;
        QJsonArray messages;          /* full request history for this turn */
        QJsonObject assistantToolMessage; /* waiting for tool results */
        QHash<int, QJsonObject> partialCalls;
        QJsonArray toolCalls;         /* completed calls, sorted by index */
        QByteArray buffer;            /* incomplete SSE line */
        QByteArray raw;               /* full body for non-SSE servers */
        QString content;
        QString reasoning;
        QString finishReason;
        bool sse = false;
        bool sawDone = false;
        bool cancelled = false;
        bool disableTools = false;    /* retry once when a server rejects tools */
        bool disableImage = false;    /* retry once for a text-only model */
        int round = 0;
        qint64 startedAt = 0;
    };
    StreamState m_stream;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_ASSISTANT_SERVICE_HPP