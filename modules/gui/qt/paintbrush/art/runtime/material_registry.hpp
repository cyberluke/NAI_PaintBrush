/*****************************************************************************
 * material_registry.hpp : NAI Paintbrush — art material manifest & registry
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_MATERIAL_REGISTRY_HPP
#define PB_ART_MATERIAL_REGISTRY_HPP

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>

namespace paintbrush {
namespace art {

/* One typed parameter of an art material (spec §36). The descriptor is native
 * C++ metadata (not YAML at runtime) but exposes the same uniform contract a
 * manifest would: id, category, determinism, inputs/state/parameters/
 * capabilities. */
struct ArtParamSpec
{
    QString id;         /* "amount", "flake_size", "shimmer" ... */
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.5f;
    QString unit;       /* optional: "px", "%", "mm" */
};

/* A uniform descriptor for one art material/effect. Every material exposes
 * this so the UI, the doc serializer and Teen Boost all speak the same
 * contract without one-off switch/case per tool. */
struct ArtMaterialManifest
{
    QString id;                 /* "material.glitter.fine" */
    int version = 1;
    QString category;           /* "material" | "finish" | "paper" */
    bool deterministic = true;
    QStringList inputs;         /* "paper","light","adhesion","pointer"... */
    QStringList state;          /* "coverage","bonded","height"... */
    QVector<ArtParamSpec> parameters;
    QStringList capabilities;   /* "interactive","export","boost_tool" */
};

/* A registry of the materials the runtime knows about. The registry is the
 * single place the product discovers materials; a material is hidden from the
 * UI (spec §2, §42 "if it is not beautiful, hide it") by simply not being
 * registered. */
class MaterialRegistry
{
public:
    MaterialRegistry();

    /* Register or replace a material manifest. */
    void registerMaterial(const ArtMaterialManifest &m);
    /* Look up by id; returns nullptr when unknown. */
    const ArtMaterialManifest *find(const QString &id) const;
    bool contains(const QString &id) const { return find(id) != nullptr; }
    QStringList ids() const;

    /* The built-in materials (Bristle Paint, Glitter, and the paper/effect
     * descriptors) are registered in the constructor. `enabledIds()` returns
     * the ids whose capability filter (interactive/export/boost) contains
     * `requireCapability` when non-empty. */
    QStringList enabledIds(const QString &requireCapability = QString()) const;

private:
    QVector<ArtMaterialManifest> m_manifests;
};

/* Build a manifest for a named material (convenience for the constructor). */
ArtMaterialManifest makeManifest(const QString &id, const QString &category,
                                 int version, const QStringList &inputs,
                                 const QStringList &state,
                                 const QStringList &capabilities,
                                 const QVector<ArtParamSpec> &specs);

} // namespace art
} // namespace paintbrush

#endif // PB_ART_MATERIAL_REGISTRY_HPP