/*****************************************************************************
 * material_registry.cpp : NAI Paintbrush — art material manifest & registry
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "material_registry.hpp"

namespace paintbrush {
namespace art {

ArtMaterialManifest makeManifest(const QString &id, const QString &category,
                                 int version, const QStringList &inputs,
                                 const QStringList &state,
                                 const QStringList &capabilities,
                                 const QVector<ArtParamSpec> &specs)
{
    ArtMaterialManifest m;
    m.id = id;
    m.version = version;
    m.category = category;
    m.inputs = inputs;
    m.state = state;
    m.capabilities = capabilities;
    m.parameters = specs;
    return m;
}

MaterialRegistry::MaterialRegistry()
{
    /* -- paper ----------------------------------------------------------- */
    registerMaterial(makeManifest(
        "paper.stock", "paper", 1,
        { QStringLiteral("light"), QStringLiteral("scale") },
        { QStringLiteral("height") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("fiber_strength"), 0.0f, 2.0f, 1.0f, QString() },
          { QStringLiteral("tooth"), 0.0f, 1.0f, 0.5f, QString() },
          { QStringLiteral("roughness"), 0.0f, 1.0f, 0.5f, QString() } }));

    /* -- stroke / bristle paint ----------------------------------------- */
    registerMaterial(makeManifest(
        "material.bristle", "material", 1,
        { QStringLiteral("pointer"), QStringLiteral("paper") },
        { QStringLiteral("coverage"), QStringLiteral("pigment") },
        { QStringLiteral("interactive"), QStringLiteral("export") },
        { { QStringLiteral("size"), 1.0f, 300.0f, 24.0f, QStringLiteral("px") },
          { QStringLiteral("flow"), 0.0f, 1.0f, 0.7f, QString() },
          { QStringLiteral("hardness"), 0.0f, 1.0f, 0.85f, QString() },
          { QStringLiteral("smoothing"), 0.0f, 1.0f, 0.5f, QString() } }));

    /* -- glitter flagship ------------------------------------------------ */
    registerMaterial(makeManifest(
        "material.glitter.fine", "material", 1,
        { QStringLiteral("paper"), QStringLiteral("light"), QStringLiteral("adhesion") },
        { QStringLiteral("coverage"), QStringLiteral("bonded"),
          QStringLiteral("height"), QStringLiteral("palette_slots") },
        { QStringLiteral("interactive"), QStringLiteral("export"),
          QStringLiteral("boost_tool") },
        { { QStringLiteral("amount"), 0.0f, 2.0f, 1.0f, QString() },
          { QStringLiteral("flake_size"), 0.2f, 2.5f, 0.8f, QString() },
          { QStringLiteral("shimmer"), 0.0f, 1.0f, 0.55f, QString() },
          { QStringLiteral("adhesion"), 0.0f, 1.0f, 0.6f, QString() },
          { QStringLiteral("holographic"), 0.0f, 1.0f, 0.0f, QString() } }));

    /* -- glitter powder (re-renders from persistent physical state) ------- */
    registerMaterial(makeManifest(
        "material.glitter.powder", "material", 1,
        { QStringLiteral("paper"), QStringLiteral("light"), QStringLiteral("adhesion") },
        { QStringLiteral("coverage"), QStringLiteral("bonded"),
          QStringLiteral("height"), QStringLiteral("palette_slots") },
        { QStringLiteral("interactive"), QStringLiteral("export"),
          QStringLiteral("boost_tool") },
        { { QStringLiteral("amount"), 0.0f, 2.0f, 1.0f, QString() },
          { QStringLiteral("flake_size"), 0.2f, 2.5f, 0.8f, QString() },
          { QStringLiteral("adhesion"), 0.0f, 1.0f, 0.6f, QString() },
          { QStringLiteral("loose"), 0.0f, 1.0f, 0.5f, QString() },
          { QStringLiteral("holographic"), 0.0f, 1.0f, 0.0f, QString() } }));

    /* -- finish runtime effects ------------------------------------------ */
    registerMaterial(makeManifest(
        "finish.shadow", "finish", 1,
        { QStringLiteral("light"), QStringLiteral("layer") },
        { QStringLiteral("offset"), QStringLiteral("radius") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("blur"), 0.0f, 64.0f, 8.0f, QStringLiteral("px") },
          { QStringLiteral("spread"), 0.0f, 1.0f, 0.0f, QString() },
          { QStringLiteral("opacity"), 0.0f, 1.0f, 0.5f, QString() } }));

    registerMaterial(makeManifest(
        "finish.glow", "finish", 1,
        { QStringLiteral("layer") },
        { QStringLiteral("radius"), QStringLiteral("intensity") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("radius"), 0.0f, 96.0f, 20.0f, QStringLiteral("px") },
          { QStringLiteral("intensity"), 0.0f, 2.0f, 1.0f, QString() } }));

    registerMaterial(makeManifest(
        "finish.motion_blur", "finish", 1,
        { QStringLiteral("layer"), QStringLiteral("motion") },
        { QStringLiteral("direction"), QStringLiteral("distance") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("distance"), 0.0f, 256.0f, 24.0f, QStringLiteral("px") },
          { QStringLiteral("samples"), 3.0f, 96.0f, 24.0f, QString() } }));

    registerMaterial(makeManifest(
        "finish.outline", "finish", 1,
        { QStringLiteral("layer") },
        { QStringLiteral("width"), QStringLiteral("color") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("width"), 0.0f, 48.0f, 3.0f, QStringLiteral("px") },
          { QStringLiteral("softness"), 0.0f, 1.0f, 0.2f, QString() } }));

    registerMaterial(makeManifest(
        "finish.ghost", "finish", 1,
        { QStringLiteral("layer"), QStringLiteral("light") },
        { QStringLiteral("opacity"), QStringLiteral("tint") },
        { QStringLiteral("export"), QStringLiteral("boost_tool") },
        { { QStringLiteral("opacity"), 0.0f, 1.0f, 0.35f, QString() } }));
}

void MaterialRegistry::registerMaterial(const ArtMaterialManifest &m)
{
    for (ArtMaterialManifest &existing : m_manifests)
    {
        if (existing.id == m.id)
        {
            existing = m;
            return;
        }
    }
    m_manifests.append(m);
}

const ArtMaterialManifest *MaterialRegistry::find(const QString &id) const
{
    for (const ArtMaterialManifest &m : m_manifests)
    {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

QStringList MaterialRegistry::ids() const
{
    QStringList out;
    out.reserve(m_manifests.size());
    for (const ArtMaterialManifest &m : m_manifests)
        out.append(m.id);
    return out;
}

QStringList MaterialRegistry::enabledIds(const QString &requireCapability) const
{
    QStringList out;
    for (const ArtMaterialManifest &m : m_manifests)
    {
        if (requireCapability.isEmpty() ||
            m.capabilities.contains(requireCapability))
            out.append(m.id);
    }
    return out;
}

} // namespace art
} // namespace paintbrush