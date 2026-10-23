/*****************************************************************************
 * pb_lookup_service.hpp : NAI Paintbrush — object/product lookup extension
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LOOKUP_SERVICE_HPP
#define PB_LOOKUP_SERVICE_HPP

#include <QObject>
#include <QString>
#include <QImage>
#include <QVariantList>
#include <QNetworkAccessManager>
#include <QList>

#include "pb_capability.hpp"

struct vlc_object_t;
class QNetworkReply;

namespace paintbrush {

/* Object lookup ("what handbag is this?").
 *
 * HONESTY (A42): this service does NOT perform an evidence-backed lookup.
 * It posts one prompt + one image to the configured assistant gateway and
 * the model GENERATES candidate identifications. There is no catalog
 * retrieval, no search index, and sourceUrl/imageUrl are never populated —
 * every result is marked evidenceBacked=false and confidenceIsEstimate=true
 * so the UI never presents a fabricated source as real. A real retrieval
 * layer must set evidenceBacked=true and populate sourceUrl before any
 * result can be shown as a "Match" with sources.
 *
 * Input : object crop + VLM description + optional OCR + optional query.
 * Output: candidate matches (model-generated, not grounded). */
class LookupService : public QObject
{
    Q_OBJECT

public:
    struct Match
    {
        QString title;
        QString description;
        QString sourceUrl;   /* never populated today: no retrieval layer  */
        QString imageUrl;    /* never populated today: no retrieval layer  */
        double confidence = 0.0;
        QString evidence;
        QVariantMap metadata;
        /* A42: candidate generation, not grounded evidence. */
        bool evidenceBacked = false;
        bool confidenceIsEstimate = true;

        /* A real source exists only when the URL is non-empty AND the match
         * is evidence-backed. */
        bool hasSource() const
        {
            return !sourceUrl.isEmpty() && evidenceBacked;
        }
    };

    explicit LookupService(QObject *parent = nullptr);

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return m_configured; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString providerName() const { return m_providerName; }
    /* Module-level honesty: candidate-generation prompt, not an
     * evidence-backed retrieval. Always false until a real catalog
     * retrieval layer exists. */
    bool evidenceBacked() const { return false; }

    /* TLS posture (A7): certificate validation is on unless the operator
     * explicitly opts into self-signed endpoints. */
    bool insecureTlsOptIn() const { return m_insecureTls; }
    TlsPosture tlsPosture() const
    {
        return m_insecureTls ? TlsPosture::DevSelfSigned : TlsPosture::Normal;
    }

    /* Returns false when the provider is unavailable; when available, the
     * provider emits lookupResult/errorOccurred asynchronously. */
    bool lookup(const QImage &objectCrop, const QString &vlmDescription,
                const QString &ocrText, const QString &query);

    QVariantList matches() const;

    /* Parse a model reply (JSON object or plain text) into candidate match
     * maps. Every map carries evidenceBacked=false, confidenceIsEstimate=true
     * and empty sourceUrl/imageUrl (A42). */
    static QVariantList matchesFromContent(const QString &content);

signals:
    void lookupResult(int requestId, const QVariantList &matches);
    void errorOccurred(const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private:
    void onReplyFinished(int requestId);
    /* A7: TLS validation gating shared by all replies. */
    void handleSslErrors(QNetworkReply *reply) const;

    vlc_object_t *m_obj = nullptr;
    bool m_configured = false;
    QString m_providerName;
    QString m_unavailableReason;
    QVariantList m_matches;

    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
    int m_timeoutMs = 120000;
    int m_requestId = 0;
    bool m_insecureTls = false;

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_LOOKUP_SERVICE_HPP