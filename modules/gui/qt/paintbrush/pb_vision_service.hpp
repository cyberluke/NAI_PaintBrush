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

#include <vlc_common.h>

class QNetworkReply;

namespace paintbrush {

/* OpenAI-compatible vision provider used by "Ask about this".
 *
 * The request packages the object crop, the contextual full frame and the
 * user question. The provider is told to distinguish visible evidence,
 * likely inference and uncertainty — exact SKU/brand identification is
 * only reported when visual evidence supports it. */
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

    /* Ask a question about an object. conversationId correlates follow-up
     * turns so the UI can keep object context. */
    void ask(const QImage &objectCrop, const QImage &contextFrame,
             const QString &question, const QString &conversationId);

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

private:
    QByteArray buildPayload(const QImage &crop, const QImage &context,
                            const QString &question) const;
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

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_VISION_SERVICE_HPP