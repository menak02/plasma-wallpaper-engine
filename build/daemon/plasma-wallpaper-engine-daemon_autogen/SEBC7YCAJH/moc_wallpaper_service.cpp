/****************************************************************************
** Meta object code from reading C++ file 'wallpaper_service.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../daemon/src/ipc/wallpaper_service.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'wallpaper_service.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t {};
} // unnamed namespace

template <> constexpr inline auto WallpaperEngine::IPC::WallpaperService::qt_create_metaobjectdata<qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "WallpaperEngine::IPC::WallpaperService",
        "D-Bus Interface",
        "org.antigravity.WallpaperEngine",
        "frameReady",
        "",
        "wallpaperLoaded",
        "title",
        "libraryUpdated",
        "count",
        "bufferResized",
        "uint32_t",
        "width",
        "height",
        "propertyChanged",
        "key",
        "QDBusVariant",
        "value",
        "getBufferFd",
        "QDBusUnixFileDescriptor",
        "getBufferInfo",
        "QVariantMap",
        "loadWallpaper",
        "path",
        "requestFrame",
        "setResolution",
        "getAvailableGpus",
        "QVariantList",
        "setMousePosition",
        "normX",
        "normY",
        "setAudioVolume",
        "volume",
        "setAudioMuted",
        "muted",
        "setMuteOnOtherAudio",
        "enabled",
        "setMuteOnFullscreen",
        "getAudioSettings",
        "pause",
        "resume",
        "stop",
        "getLibrary",
        "scanLibrary",
        "addCustomLibraryPath",
        "getWallpaperProperties",
        "id",
        "setProperty",
        "getProperty"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'frameReady'
        QtMocHelpers::SignalData<void()>(3, 4, QMC::AccessPublic, QMetaType::Void),
        // Signal 'wallpaperLoaded'
        QtMocHelpers::SignalData<void(const QString &)>(5, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 6 },
        }}),
        // Signal 'libraryUpdated'
        QtMocHelpers::SignalData<void(int)>(7, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 },
        }}),
        // Signal 'bufferResized'
        QtMocHelpers::SignalData<void(uint32_t, uint32_t)>(9, 4, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 11 }, { 0x80000000 | 10, 12 },
        }}),
        // Signal 'propertyChanged'
        QtMocHelpers::SignalData<void(const QString &, const QDBusVariant &)>(13, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 14 }, { 0x80000000 | 15, 16 },
        }}),
        // Slot 'getBufferFd'
        QtMocHelpers::SlotData<QDBusUnixFileDescriptor()>(17, 4, QMC::AccessPublic, 0x80000000 | 18),
        // Slot 'getBufferInfo'
        QtMocHelpers::SlotData<QVariantMap()>(19, 4, QMC::AccessPublic, 0x80000000 | 20),
        // Slot 'loadWallpaper'
        QtMocHelpers::SlotData<bool(const QString &)>(21, 4, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 22 },
        }}),
        // Slot 'requestFrame'
        QtMocHelpers::SlotData<void()>(23, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'setResolution'
        QtMocHelpers::SlotData<bool(uint32_t, uint32_t)>(24, 4, QMC::AccessPublic, QMetaType::Bool, {{
            { 0x80000000 | 10, 11 }, { 0x80000000 | 10, 12 },
        }}),
        // Slot 'getAvailableGpus'
        QtMocHelpers::SlotData<QVariantList()>(25, 4, QMC::AccessPublic, 0x80000000 | 26),
        // Slot 'setMousePosition'
        QtMocHelpers::SlotData<void(float, float)>(27, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Float, 28 }, { QMetaType::Float, 29 },
        }}),
        // Slot 'setAudioVolume'
        QtMocHelpers::SlotData<void(int)>(30, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 31 },
        }}),
        // Slot 'setAudioMuted'
        QtMocHelpers::SlotData<void(bool)>(32, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 33 },
        }}),
        // Slot 'setMuteOnOtherAudio'
        QtMocHelpers::SlotData<void(bool)>(34, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 35 },
        }}),
        // Slot 'setMuteOnFullscreen'
        QtMocHelpers::SlotData<void(bool)>(36, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 35 },
        }}),
        // Slot 'getAudioSettings'
        QtMocHelpers::SlotData<QVariantMap()>(37, 4, QMC::AccessPublic, 0x80000000 | 20),
        // Slot 'pause'
        QtMocHelpers::SlotData<void()>(38, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'resume'
        QtMocHelpers::SlotData<void()>(39, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'stop'
        QtMocHelpers::SlotData<void()>(40, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'getLibrary'
        QtMocHelpers::SlotData<QVariantList()>(41, 4, QMC::AccessPublic, 0x80000000 | 26),
        // Slot 'scanLibrary'
        QtMocHelpers::SlotData<void()>(42, 4, QMC::AccessPublic, QMetaType::Void),
        // Slot 'addCustomLibraryPath'
        QtMocHelpers::SlotData<void(const QString &)>(43, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 22 },
        }}),
        // Slot 'getWallpaperProperties'
        QtMocHelpers::SlotData<QVariantMap(const QString &)>(44, 4, QMC::AccessPublic, 0x80000000 | 20, {{
            { QMetaType::QString, 45 },
        }}),
        // Slot 'setProperty'
        QtMocHelpers::SlotData<void(const QString &, const QDBusVariant &)>(46, 4, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 14 }, { 0x80000000 | 15, 16 },
        }}),
        // Slot 'getProperty'
        QtMocHelpers::SlotData<QDBusVariant(const QString &)>(47, 4, QMC::AccessPublic, 0x80000000 | 15, {{
            { QMetaType::QString, 14 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    QtMocHelpers::UintData qt_constructors {};
    QtMocHelpers::ClassInfos qt_classinfo({
            {    1,    2 },
    });
    return QtMocHelpers::metaObjectData<WallpaperService, qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums, qt_constructors, qt_classinfo);
}
Q_CONSTINIT const QMetaObject WallpaperEngine::IPC::WallpaperService::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>.metaTypes,
    nullptr
} };

void WallpaperEngine::IPC::WallpaperService::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<WallpaperService *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->frameReady(); break;
        case 1: _t->wallpaperLoaded((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->libraryUpdated((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 3: _t->bufferResized((*reinterpret_cast<std::add_pointer_t<uint32_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<uint32_t>>(_a[2]))); break;
        case 4: _t->propertyChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QDBusVariant>>(_a[2]))); break;
        case 5: { QDBusUnixFileDescriptor _r = _t->getBufferFd();
            if (_a[0]) *reinterpret_cast<QDBusUnixFileDescriptor*>(_a[0]) = std::move(_r); }  break;
        case 6: { QVariantMap _r = _t->getBufferInfo();
            if (_a[0]) *reinterpret_cast<QVariantMap*>(_a[0]) = std::move(_r); }  break;
        case 7: { bool _r = _t->loadWallpaper((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 8: _t->requestFrame(); break;
        case 9: { bool _r = _t->setResolution((*reinterpret_cast<std::add_pointer_t<uint32_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<uint32_t>>(_a[2])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 10: { QVariantList _r = _t->getAvailableGpus();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 11: _t->setMousePosition((*reinterpret_cast<std::add_pointer_t<float>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<float>>(_a[2]))); break;
        case 12: _t->setAudioVolume((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 13: _t->setAudioMuted((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 14: _t->setMuteOnOtherAudio((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 15: _t->setMuteOnFullscreen((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 16: { QVariantMap _r = _t->getAudioSettings();
            if (_a[0]) *reinterpret_cast<QVariantMap*>(_a[0]) = std::move(_r); }  break;
        case 17: _t->pause(); break;
        case 18: _t->resume(); break;
        case 19: _t->stop(); break;
        case 20: { QVariantList _r = _t->getLibrary();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 21: _t->scanLibrary(); break;
        case 22: _t->addCustomLibraryPath((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 23: { QVariantMap _r = _t->getWallpaperProperties((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantMap*>(_a[0]) = std::move(_r); }  break;
        case 24: _t->setProperty((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QDBusVariant>>(_a[2]))); break;
        case 25: { QDBusVariant _r = _t->getProperty((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QDBusVariant*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 4:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 1:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< QDBusVariant >(); break;
            }
            break;
        case 24:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 1:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< QDBusVariant >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (WallpaperService::*)()>(_a, &WallpaperService::frameReady, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (WallpaperService::*)(const QString & )>(_a, &WallpaperService::wallpaperLoaded, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (WallpaperService::*)(int )>(_a, &WallpaperService::libraryUpdated, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (WallpaperService::*)(uint32_t , uint32_t )>(_a, &WallpaperService::bufferResized, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (WallpaperService::*)(const QString & , const QDBusVariant & )>(_a, &WallpaperService::propertyChanged, 4))
            return;
    }
}

const QMetaObject *WallpaperEngine::IPC::WallpaperService::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *WallpaperEngine::IPC::WallpaperService::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15WallpaperEngine3IPC16WallpaperServiceE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int WallpaperEngine::IPC::WallpaperService::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 26)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 26;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 26)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 26;
    }
    return _id;
}

// SIGNAL 0
void WallpaperEngine::IPC::WallpaperService::frameReady()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void WallpaperEngine::IPC::WallpaperService::wallpaperLoaded(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void WallpaperEngine::IPC::WallpaperService::libraryUpdated(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void WallpaperEngine::IPC::WallpaperService::bufferResized(uint32_t _t1, uint32_t _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}

// SIGNAL 4
void WallpaperEngine::IPC::WallpaperService::propertyChanged(const QString & _t1, const QDBusVariant & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1, _t2);
}
QT_WARNING_POP
