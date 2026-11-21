/*****************************************************************************
 * pb_palette_store.hpp : NAI Paintbrush — art-direction palette store
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#ifndef PB_PALETTE_STORE_HPP
#define PB_PALETTE_STORE_HPP

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace paintbrush
{

/* One palette = an ordered swatch ramp plus identity/provenance.
 * Shipped presets are `locked`: they can be used and exported, never edited
 * or deleted, so an update can never clobber something the user tuned. */
struct Palette
{
    QString id;
    QString name;
    QString description;
    QString author = QStringLiteral("NAI");
    bool locked = false;
    QStringList colors;              /* "#RRGGBB" */

    bool isValid() const;
    QVariantMap toVariantMap() const;
    static Palette fromVariantMap(const QVariantMap &map);
};

/* Loads the shipped palettes (embedded, identical to
 * palettes/builtin-palettes.json) and merges the user's own palettes stored
 * as one JSON file per palette under the application data directory.
 *
 * Deliberately Qt-Core only: the store is testable without VLC, the GUI or a
 * QML engine. */
class PaletteStore
{
public:
    PaletteStore();

    /* built-ins (in shipped order) followed by user palettes (by name) */
    QVector<Palette> all() const;
    const Palette *find(const QString &id) const;

    /* Create or update a USER palette. Refuses locked ids and invalid data. */
    bool save(const Palette &palette, QString *error = nullptr);

    /* Delete a USER palette. Refuses locked ids. */
    bool remove(const QString &id, QString *error = nullptr);

    /* Write one palette as a standalone v1 JSON document (export), or read
     * one back and store it as a user palette (import). */
    bool exportTo(const QString &id, const QString &filePath,
                  QString *error = nullptr) const;
    bool importFrom(const QString &filePath, QString *error = nullptr);

    QString userDir() const;

    static QVector<Palette> builtins();
    static bool validate(const Palette &palette, QString *error);

private:
    void reloadUser();
    bool writeUserFile(const Palette &palette, QString *error) const;

    QVector<Palette> m_builtins;
    QVector<Palette> m_user;
};

} // namespace paintbrush

#endif
