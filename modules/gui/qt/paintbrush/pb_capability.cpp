/*****************************************************************************
 * pb_capability.cpp : NAI Paintbrush — truthful AI capability reports
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_capability.hpp"

namespace paintbrush {

bool CapabilityReport::isUsable() const
{
    return mode == LocalHeal || mode == LocalBlur || mode == RemoteGeneration;
}

bool CapabilityReport::isGenerative() const
{
    return mode == RemoteGeneration;
}

bool CapabilityReport::isLocal() const
{
    return mode == LocalHeal || mode == LocalBlur;
}

QString CapabilityReport::modeName() const
{
    switch (mode)
    {
    case LocalHeal:
        return QStringLiteral("local-heal");
    case LocalBlur:
        return QStringLiteral("local-blur");
    case RemoteGeneration:
        return QStringLiteral("remote-generation");
    case Unavailable:
        break;
    }
    return QStringLiteral("unavailable");
}

CapabilityReport localGenerationReport(CapabilityReport::Mode localMode,
                                       const QString &reason)
{
    CapabilityReport report;
    report.mode = (localMode == CapabilityReport::LocalBlur)
                      ? CapabilityReport::LocalBlur
                      : CapabilityReport::LocalHeal;
    report.reason = reason;
    return report;
}

CapabilityReport remoteGenerationReport(const QString &providerUrl,
                                        const QString &modelName)
{
    CapabilityReport report;
    report.mode = CapabilityReport::RemoteGeneration;
    report.providerUrl = providerUrl;
    report.modelName = modelName;
    report.reason = modelName.isEmpty()
        ? QStringLiteral("Remote image generation via the configured provider "
                         "(default model).")
        : QStringLiteral("Remote image generation via the configured provider "
                         "(model %1).").arg(modelName);
    return report;
}

QString tlsPostureName(TlsPosture posture)
{
    return posture == TlsPosture::DevSelfSigned
               ? QStringLiteral("dev-selfsigned")
               : QStringLiteral("normal");
}

QString sslErrorDetail(const QList<QSslError> &errors)
{
    if (errors.isEmpty())
        return {};
    QStringList parts;
    parts.reserve(errors.size());
    for (const QSslError &error : errors)
        parts << error.errorString();
    return parts.join(QStringLiteral("; "));
}

} // namespace paintbrush