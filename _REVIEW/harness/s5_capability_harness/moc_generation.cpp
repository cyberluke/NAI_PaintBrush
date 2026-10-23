/****************************************************************************
** Meta object code from reading C++ file 'pb_generation_service.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../modules/gui/qt/paintbrush/pb_generation_service.hpp"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pb_generation_service.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN10paintbrush17GenerationServiceE_t {};
} // unnamed namespace

template <> constexpr inline auto paintbrush::GenerationService::qt_create_metaobjectdata<qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "paintbrush::GenerationService",
        "busyChanged",
        "",
        "busy",
        "resultReady",
        "requestId",
        "QImage",
        "result",
        "errorOccurred",
        "message",
        "availabilityChanged",
        "available",
        "reason",
        "onReplyFinished",
        "Action",
        "ACTION_REMOVE_OBJECT",
        "ACTION_GENERATIVE_FILL",
        "ACTION_REPLACE_BACKGROUND",
        "ACTION_MAKE_STICKER"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'busyChanged'
        QtMocHelpers::SignalData<void(bool)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 3 },
        }}),
        // Signal 'resultReady'
        QtMocHelpers::SignalData<void(const QString &, const QImage &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 }, { 0x80000000 | 6, 7 },
        }}),
        // Signal 'errorOccurred'
        QtMocHelpers::SignalData<void(const QString &, const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 }, { QMetaType::QString, 9 },
        }}),
        // Signal 'availabilityChanged'
        QtMocHelpers::SignalData<void(bool, const QString &)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 11 }, { QMetaType::QString, 12 },
        }}),
        // Slot 'onReplyFinished'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
        // enum 'Action'
        QtMocHelpers::EnumData<enum Action>(14, 14, QMC::EnumFlags{}).add({
            {   15, Action::ACTION_REMOVE_OBJECT },
            {   16, Action::ACTION_GENERATIVE_FILL },
            {   17, Action::ACTION_REPLACE_BACKGROUND },
            {   18, Action::ACTION_MAKE_STICKER },
        }),
    };
    return QtMocHelpers::metaObjectData<GenerationService, qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject paintbrush::GenerationService::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>.metaTypes,
    nullptr
} };

void paintbrush::GenerationService::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<GenerationService *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->busyChanged((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 1: _t->resultReady((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QImage>>(_a[2]))); break;
        case 2: _t->errorOccurred((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 3: _t->availabilityChanged((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 4: _t->onReplyFinished(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (GenerationService::*)(bool )>(_a, &GenerationService::busyChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (GenerationService::*)(const QString & , const QImage & )>(_a, &GenerationService::resultReady, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (GenerationService::*)(const QString & , const QString & )>(_a, &GenerationService::errorOccurred, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (GenerationService::*)(bool , const QString & )>(_a, &GenerationService::availabilityChanged, 3))
            return;
    }
}

const QMetaObject *paintbrush::GenerationService::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *paintbrush::GenerationService::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10paintbrush17GenerationServiceE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int paintbrush::GenerationService::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void paintbrush::GenerationService::busyChanged(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void paintbrush::GenerationService::resultReady(const QString & _t1, const QImage & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void paintbrush::GenerationService::errorOccurred(const QString & _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2);
}

// SIGNAL 3
void paintbrush::GenerationService::availabilityChanged(bool _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}
QT_WARNING_POP
