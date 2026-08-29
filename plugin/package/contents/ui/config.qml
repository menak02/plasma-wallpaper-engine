import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

Kirigami.ScrollablePage {
    id: root

    // Configuration properties bound to Plasma wallpaper config
    property var wallpaperConfiguration

    property alias cfg_WallpaperSource: libraryView.selectedSource
    property alias cfg_WallpaperId: libraryView.selectedId
    property alias cfg_FpsLimit: engineView.fpsLimit
    property alias cfg_PauseOnBattery: engineView.pauseOnBattery
    property alias cfg_PauseOnFullscreen: engineView.pauseOnFullscreen

    header: QQC2.TabBar {
        id: navBar
        width: parent.width

        QQC2.TabButton {
            text: i18n("Wallpaper Library")
            icon.name: "view-media-album-cover"
        }
        QQC2.TabButton {
            text: i18n("Custom Properties")
            icon.name: "configure"
        }
        QQC2.TabButton {
            text: i18n("Workshop Browser")
            icon.name: "download"
        }
        QQC2.TabButton {
            text: i18n("Engine Settings")
            icon.name: "settings-configure"
        }
    }

    StackLayout {
        id: stackLayout
        anchors.fill: parent
        currentIndex: navBar.currentIndex

        // 1. Library Page
        LibraryView {
            id: libraryView
            onWallpaperSelected: (source, id, title, properties) => {
                propertiesView.loadProperties(title, properties);
            }
        }

        // 2. Properties Page
        PropertiesView {
            id: propertiesView
        }

        // 3. Workshop Browser
        WorkshopView {
            id: workshopView
        }

        // 4. Engine Settings Page
        EngineSettingsView {
            id: engineView
        }
    }
}
