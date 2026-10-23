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
#include "pb_capability.hpp"

#include <QObject>
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QList>

#include <vlc_common.h>

class QNetworkReply;

namespace paintbrush {

/* OpenAI-compatible image generation/editing provider backing Remove,
 * Generative fill, Background replace and the optional cartoon treatment
 * of Make sticker.
 *
 * When no provider is configured the local CPU fallbacks keep the actions
 * usable, but they are pixel operations, NOT generative: capability()
 * reports local-heal / local-blur (never remote-generation) and the UI must
 * present them as such (A3/A4). */
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
    /* Truthful usability: true only when a real capability exists (local
     * heal/blur fallback or a configured provider). This never claims
     * remote generation — use capability() to know which mode is active. */
    bool isConfigured() const { return capability().isUsable(); }
    bool hasProvider() const { return !m_baseUrl.isEmpty(); }
    bool isBusy() const { return m_busy; }
    QString baseUrl() const { return m_baseUrl; }
    QString model() const { return m_model; }
    QString unavailableReason() const { return m_unavailableReason; }

    /* Action-aware capability report (A3/A4): local-heal for remove/fill,
     * local-blur for background replace, remote-generation only when a
     * provider is configured. Never green "ready" without a provider. */
    CapabilityReport capability(Action action = ACTION_GENERATIVE_FILL) const;
    CapabilityReport::Mode capabilityMode(Action action = ACTION_GENERATIVE_FILL) const
    {
        return capability(action).mode;
    }

    /* TLS posture (A7): certificate validation is on unless the operator
     * explicitly opts into self-signed endpoints. */
    bool insecureTlsOptIn() const { return m_insecureTls; }
    TlsPosture tlsPosture() const
    {
        return m_insecureTls ? TlsPosture::DevSelfSigned : TlsPosture::Normal;
    }

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
    /* Extract the first data object of an OpenAI-style images response. */
    static QJsonObject firstDataObject(const QByteArray &json);
    /* A6: download a provider image URL asynchronously (GET -> bytes ->
     * QImage::fromData). Never a synchronous QImage::load(url). */
    void fetchImage(const QUrl &url, const QString &requestId, qint64 startedAt);
    /* A7: TLS validation gating shared by POST and image GET replies. */
    void handleSslErrors(QNetworkReply *reply) const;
    void setBusy(bool busy);

    vlc_object_t *m_obj = nullptr;
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    QString m_unavailableReason;
    int m_timeoutMs = 120000;
    bool m_busy = false;
    bool m_insecureTls = false;
    /* bumped by cancelAll(); in-flight local workers drop their result when
     * their captured epoch no longer matches */
    quint64 m_cancelEpoch = 0;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_GENERATION_SERVICE_HPP