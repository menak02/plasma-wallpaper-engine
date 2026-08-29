import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

Item {
    id: root

    property string selectedSource: ""
    property string selectedId: ""

    signal wallpaperSelected(string source, string id, string title, var properties)

    ListModel {
        id: libraryModel
    }

    function refreshLibrary() {
        libraryModel.clear();
        // Fallback default list or scan
        let items = [
            {
                "id": "3594269099",
                "title": "Reze - In The Pool Music",
                "type": "scene",
                "preview": "file:///home/mena/.steam/debian-installation/steamapps/workshop/content/431960/3594269099/preview.jpg",
                "source": "/home/mena/.steam/debian-installation/steamapps/workshop/content/431960/3594269099/scene.pkg",
                "description": "Reze Wallpaper with In the pool music",
                "tags": ["Anime", "Music"]
            },
            {
                "id": "3200802205",
                "title": "Neon Cyberpunk Scene",
                "type": "scene",
                "preview": "file:///home/mena/.steam/debian-installation/steamapps/workshop/content/431960/3200802205/preview.gif",
                "source": "/home/mena/.steam/debian-installation/steamapps/workshop/content/431960/3200802205/scene.pkg",
                "description": "Animated Cyberpunk City with audio reactivity",
                "tags": ["Cyberpunk", "Sci-Fi"]
            }
        ];

        for (let i = 0; i < items.length; ++i) {
            libraryModel.append(items[i]);
        }
    }

    Component.onCompleted: {
        refreshLibrary();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        // Search & Filter Toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: searchBox
                Layout.fillWidth: true
                placeholderText: i18n("Search installed wallpapers...")
                clearButtonShown: true
            }

            QQC2.ComboBox {
                id: typeFilter
                model: [i18n("All Types"), i18n("Scene (3D/2D)"), i18n("Video"), i18n("Web")]
            }

            QQC2.Button {
                text: i18n("Add Folder...")
                icon.name: "folder-add"
                onClicked: folderDialog.open()
            }

            QQC2.Button {
                icon.name: "view-refresh"
                onClicked: refreshLibrary()
            }
        }

        // Wallpaper Grid View
        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            cellWidth: 220
            cellHeight: 180
            clip: true
            model: libraryModel

            delegate: Item {
                width: grid.cellWidth - 10
                height: grid.cellHeight - 10

                Kirigami.Card {
                    anchors.fill: parent
                    hoverEnabled: true

                    Rectangle {
                        anchors.fill: parent
                        color: root.selectedId === model.id ? Kirigami.Theme.highlightColor : "transparent"
                        opacity: 0.3
                        radius: Kirigami.Units.smallSpacing
                        visible: root.selectedId === model.id
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Kirigami.Units.smallSpacing

                        Image {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            source: model.preview
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true

                            Rectangle {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 4
                                width: typeBadge.width + 8
                                height: typeBadge.height + 4
                                color: "#cc000000"
                                radius: 3

                                Text {
                                    id: typeBadge
                                    anchors.centerIn: parent
                                    text: model.type.toUpperCase()
                                    color: "#ffffff"
                                    font.pixelSize: 9
                                    font.bold: true
                                }
                            }
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: model.title
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.selectedId = model.id;
                            root.selectedSource = model.source;
                            root.wallpaperSelected(model.source, model.id, model.title, model.properties);
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: i18n("Select Wallpaper Folder")
        onAccepted: {
            console.log("Selected custom folder:", currentFolder);
            refreshLibrary();
        }
    }
}
