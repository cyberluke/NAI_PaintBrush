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

struct vlc_object_t;

namespace paintbrush {

/* Object lookup ("what handbag is this?").
 *
 * The default provider is the configured assistant gateway (the same chat
 * endpoint the companion uses): the object crop is sent as an image part and
 * the model returns a JSON identification {title, description, confidence,
 * evidence, metadata}. A dedicated shopping/search vendor can later replace
 * the provider without touching the editor.
 *
 * Input : object crop + VLM description + optional OCR + optional query.
 * Output: candidate matches with confidence/evidence/metadata/source URLs.
 */
class LookupService : public QObject
{
    Q_OBJECT

public:
    struct Match
    {
        QString title;
        QString description;
        QString sourceUrl;
        QString imageUrl;
        double confidence = 0.0;
        QString evidence;
        QVariantMap metadata;
    };

    explicit LookupService(QObject *parent = nullptr);

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return m_configured; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString providerName() const { return m_providerName; }

    /* Returns false when the provider is unavailable; when available, the
     * provider emits lookupResult/errorOccurred asynchronously. */
    bool lookup(const QImage &objectCrop, const QString &vlmDescription,
                const QString &ocrText, const QString &query);

    QVariantList matches() const;

signals:
    void lookupResult(int requestId, const QVariantList &matches);
    void errorOccurred(const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private:
    void onReplyFinished(int requestId);

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

    QNetworkAccessManager m_network;
    QList<QNetworkReply *> m_replies;
};

} // namespace paintbrush

#endif // PB_LOOKUP_SERVICE_HPP