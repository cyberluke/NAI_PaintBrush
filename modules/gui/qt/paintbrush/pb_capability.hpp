/*****************************************************************************
 * pb_capability.hpp : NAI Paintbrush — truthful AI capability reports
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CAPABILITY_HPP
#define PB_CAPABILITY_HPP

#include <QString>
#include <QSslError>

namespace paintbrush {

/* Truthful capability report for an AI-backed feature. Every status dot and
 * UI claim must be derived from a report like this — never from a hardcoded
 * "ready" boolean (A3). A feature reports exactly what it can actually do:
 *
 *  - LocalHeal        : local CPU onion-peel inpaint (remove object / fill).
 *                       NOT generative — pixel propagation, no model.
 *  - LocalBlur        : local CPU box-blur background replace. NOT
 *                       generative — a pixel filter, no model.
 *  - RemoteGeneration : a real external image model is configured; the
 *                       model name and provider URL are part of the report.
 *  - Unavailable      : nothing usable; `reason` explains why.
 *
 * The local-heal / local-blur modes must never be presented as "generative
 * fill": they are fallbacks that exist so the actions stay usable without a
 * provider, and the report says so (A4). */
struct CapabilityReport
{
    enum Mode
    {
        Unavailable = 0,
        LocalHeal,          /* onion-peel inpaint fallback, not generative  */
        LocalBlur,          /* box-blur background replace, not generative  */
        RemoteGeneration    /* real external image model                    */
    };

    Mode mode = Unavailable;
    QString reason;         /* human-readable explanation, always set        */
    QString modelName;      /* provider model, RemoteGeneration only         */
    QString providerUrl;    /* provider base URL, RemoteGeneration only      */

    /* Usable: some real capability exists (local fallback or remote model). */
    bool isUsable() const;
    /* Generative: backed by an actual external image model. Never true
     * without a configured provider. */
    bool isGenerative() const;
    /* Local: CPU fallback only (heal or blur), no model involved. */
    bool isLocal() const;
    /* Canonical mode string: "unavailable" | "local-heal" | "local-blur" |
     * "remote-generation". */
    QString modeName() const;
};

/* Builders so the generation truthfulness rules live in one place and are
 * directly testable. `localMode` must be LocalHeal or LocalBlur. */
CapabilityReport localGenerationReport(CapabilityReport::Mode localMode,
                                       const QString &reason);
CapabilityReport remoteGenerationReport(const QString &providerUrl,
                                        const QString &modelName);

/* TLS posture of an HTTP-backed service.
 *  - Normal         : certificate validation on (default).
 *  - DevSelfSigned  : operator explicitly opted into insecure self-signed
 *                     endpoints via the paintbrush-*-insecure config flags
 *                     (A7). Never the default. */
enum class TlsPosture
{
    Normal,
    DevSelfSigned
};

QString tlsPostureName(TlsPosture posture);

/* Format SSL errors into one string for truthful error messages. Empty when
 * no errors were recorded. */
QString sslErrorDetail(const QList<QSslError> &errors);

} // namespace paintbrush

#endif // PB_CAPABILITY_HPP