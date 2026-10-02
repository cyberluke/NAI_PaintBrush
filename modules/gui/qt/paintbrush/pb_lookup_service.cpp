/*****************************************************************************
 * pb_lookup_service.cpp : NAI Paintbrush — object/product lookup extension
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_lookup_service.hpp"

namespace paintbrush {

LookupService::LookupService(QObject *parent)
    : QObject(parent)
{
}

void LookupService::configure(vlc_object_t *obj)
{
    Q_UNUSED(obj);
    /* No lookup provider is wired in the integrated build. The extension
     * point exists so a future provider can be added without touching the
     * editor; until then the capability is explicitly unavailable. */
    m_configured = false;
    m_providerName = QStringLiteral("none");
    m_unavailableReason =
        QStringLiteral("Object lookup is unavailable: no provider is configured.");
    emit availabilityChanged(false, m_unavailableReason);
}

bool LookupService::lookup(const QImage &objectCrop, const QString &vlmDescription,
                           const QString &ocrText, const QString &query)
{
    Q_UNUSED(objectCrop);
    Q_UNUSED(vlmDescription);
    Q_UNUSED(ocrText);
    Q_UNUSED(query);
    if (!m_configured)
    {
        emit errorOccurred(m_unavailableReason);
        return false;
    }
    return true;
}

QVariantList LookupService::matches() const
{
    return m_matches;
}

} // namespace paintbrush