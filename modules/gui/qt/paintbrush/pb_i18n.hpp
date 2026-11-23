/*****************************************************************************
 * pb_i18n.hpp : NAI PaintBrush — live interface language switch
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#ifndef PB_I18N_HPP
#define PB_I18N_HPP

#include <QByteArray>

namespace paintbrush {
namespace i18n {

/* Exports the selected interface language to the process environment and
 * invalidates the resolved gettext catalog, so the next lookup uses it.
 * An empty code restores the system default. Callers still have to ask the
 * QML engine to retranslate; this only changes what the catalog resolves to.
 *
 * Implemented in qt.cpp: the translation backend lives with the interface
 * plugin, next to the QTranslator that feeds every qsTr() in the workspace. */
void setInterfaceLanguage(const QByteArray &code);

}
}

#endif
