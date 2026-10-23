/*****************************************************************************
 * pb_vision_service.hpp : NAI Paintbrush — Ask about this (VLM provider)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_VISION_SERVICE_HPP
#define PB_VISION_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QNetworkAccessManager>
#include <QList>
#include <QHash>
#include <QJsonArray>

#include <vlc_common.h>

class QNetworkReply;
class QSslError;

namespace paintbrush {

/* One stored message of a VisionSession conversation: a role and the OpenAI
 * content parts (text / image_url) that were sent or received. */
struct VisionMessage
{
    QString role;
    QJsonArray content;
};

/* A real multi-turn conversation. ask() with the same session id appends a
 * new user turn to this history; successful replies are stored back as
 * assistant turns. The history is bounded so follow-ups keep context without
 * unbounded growth (oldest turn pair is dropped first). */
struct VisionSession
{
    QString id;
    QList<VisionMessage> messages;
    qint64 lastUsedMs = 0;             /* LRU stamp for session eviction */
    static constexpr int maxMessages = 8;
};

/* OpenAI-compatible vision provider used by "Ask about this".
 *
 * The request packages the object crop, the contextual full frame and the
 * user question VERBATIM. The provider is told to distinguish visible
 * evidence, likely inference and uncertainty — exact SKU/brand
 * identification is only reported when visual evidence supports it.
 *
 * conversationId doubles as the conversation session key: follow-ups that
 * reuse the id are appended to the same bounded history instead of being
 * sent as fresh single shots. */
class VisionService : public QObject
{
    Q_OBJECT

public:
    explicit VisionService(QObject *parent = nullptr);
    ~VisionService() override;

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return !m_baseUrl.isEmpty(); }
    bool isBusy() const { return m_busy; }
    QString baseUrl() const { return m_baseUrl; }
    QString model() const { return m_model; }
    QString unavailableReason() const { return m_unavailableReason; }
    int timeoutMs() const { return m_timeoutMs; }

    /* Ask a question about an object. conversationId is the session key:
     * follow-ups reusing the same id append to that session's history;
     * an empty id gets a stable per-object session id. */
    void ask(const QImage &objectCrop, const QImage &contextFrame,
             const QString &question, const QString &conversationId);

    /* Forget a conversation and its history. */
    void clearSession(const QString &sessionId);
    /* Number of live conversation sessions (diagnostics). */
    int sessionCount() const { return m_sessions.size(); }

    void cancelAll();

signals:
    void busyChanged(bool busy);
    void responseReady(const QString &conversationId, const QString &answer,
                       const QString &evidence, const QString &uncertainty,
                       const QString &raw);
    void errorOccurred(const QString &conversationId, const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private slots:
    void onReplyFinished();
    /* TLS validation decision. With the paintbrush-vlm-insecure opt-in the
     * certificate errors are ignored (trusted local dev endpoints);
     * otherwise the request is aborted and reported truthfully. */
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);

private:
    QByteArray buildPayload(const QImage &crop, const QImage &context,
                            const QString &question,
                            const QString &sessionId) const;
    /* One user turn: question verbatim first, then the system prompt, then
     * the images. */
    static QJsonArray buildUserContent(const QImage &crop, const QImage &context,
                                       const QString &question);
    /* Append a message to a session, bounding the history. */
    static void appendMessage(VisionSession &session, const QString &role,
                              const QJsonArray &content);
    /* Stable session key for callers that pass no conversation id. */
    static QString makeSessionId(const QImage &crop, const QImage &context,
                                 const QString &question);
    static QString extractContent(const QByteArray &json);
    static void parseStructured(const QString &content, QString *answer,
                                QString *evidence, QString *uncertainty);

    vlc_object_t *m_obj = nullptr;
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_unavailableReason;
    int m_timeoutMs = 60000;
    bool m_busy = false;
    /* Opt-in for local dev endpoints with self-signed certificates
     * (paintbrush-vlm-insecure). Default false: TLS is validated. */
    bool m_insecure = false;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
    QHash<QString, VisionSession> m_sessions;
    static constexpr int maxSessions = 16;
};

} // namespace paintbrush

#endif // PB_VISION_SERVICE_HPP