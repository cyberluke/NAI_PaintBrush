/*****************************************************************************
 * pb_generation_service.hpp : NAI Paintbrush — generative object actions
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_GENERATION_SERVICE_HPP
#define PB_GENERATION_SERVICE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QNetworkAccessManager>
#include <QList>

#include <vlc_common.h>

class QNetworkReply;

namespace paintbrush {

/* OpenAI-compatible image generation/editing provider backing Remove,
 * Generative fill, Background replace and the optional cartoon treatment
 * of Make sticker.
 *
 * When no provider is configured every action reports an explicit
 * "unavailable" state — the rest of Paintbrush keeps working. */
class GenerationService : public QObject
{
    Q_OBJECT

public:
    enum Action {
        ACTION_REMOVE_OBJECT = 0,
        ACTION_GENERATIVE_FILL,
        ACTION_REPLACE_BACKGROUND,
        ACTION_MAKE_STICKER
    };
    Q_ENUM(Action)

    explicit GenerationService(QObject *parent = nullptr);
    ~GenerationService() override;

    void configure(vlc_object_t *obj);
    /* Always true: the local inpainting fallback makes the generative
     * actions usable even without an external image provider. When a
     * provider IS configured the network path is preferred. */
    bool isConfigured() const { return true; }
    bool hasProvider() const { return !m_baseUrl.isEmpty(); }
    bool isBusy() const { return m_busy; }
    QString baseUrl() const { return m_baseUrl; }
    QString model() const { return m_model; }
    QString unavailableReason() const { return m_unavailableReason; }

    void generate(Action action, const QImage &image, const QImage &mask,
                  const QString &prompt, const QString &requestId);
    void cancelAll();

signals:
    void busyChanged(bool busy);
    void resultReady(const QString &requestId, const QImage &result);
    void errorOccurred(const QString &requestId, const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private slots:
    void onReplyFinished();

private:
    static QString defaultPrompt(Action action, const QString &userPrompt);
    static QImage resultFromJson(const QByteArray &json);

    vlc_object_t *m_obj = nullptr;
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_unavailableReason;
    int m_timeoutMs = 120000;
    bool m_busy = false;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_GENERATION_SERVICE_HPP