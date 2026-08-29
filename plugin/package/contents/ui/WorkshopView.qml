import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: root

    ListModel {
        id: workshopModel
    }

    function searchWorkshop(query) {
        workshopModel.clear();
        loadingIndicator.running = true;

        // Query Steam Web API for App ID 431960 (Wallpaper Engine)
        let xhr = new XMLHttpRequest();
        let url = "https://api.steampowered.com/IPublishedFileService/QueryFiles/v1/?appid=431960&return_short_description=true&search_text=" + encodeURIComponent(query);
        
        xhr.open("GET", url);
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                loadingIndicator.running = false;
                if (xhr.status === 200) {
                    try {
                        let res = JSON.parse(xhr.responseText);
                        let files = res.response.publishedfiledetails || [];
                        for (let i = 0; i < files.length; ++i) {
                            let item = files[i];
                            workshopModel.append({
                                "id": item.publishedfileid,
                                "title": item.title,
                                "preview": item.preview_url || "",
                                "views": item.views || 0,
                                "subscriptions": item.subscriptions || 0,
                                "description": item.short_description || ""
                            });
                        }
                    } catch (e) {
                        console.error("Failed to parse Steam Workshop response:", e);
                    }
                }
            }
        };
        xhr.send();
    }

    Component.onCompleted: {
        searchWorkshop("Anime");
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        // Search Bar
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            QQC2.TextField {
                id: workshopSearch
                Layout.fillWidth: true
                placeholderText: i18n("Search Steam Workshop for new wallpapers...")
                clearButtonShown: true
                onAccepted: searchWorkshop(text)
            }

            QQC2.Button {
                text: i18n("Search")
                icon.name: "system-search"
                onClicked: searchWorkshop(workshopSearch.text)
            }
        }

        QQC2.BusyIndicator {
            id: loadingIndicator
            Layout.alignment: Qt.AlignCenter
            running: false
            visible: running
        }

        // Workshop Items Grid
        GridView {
            id: workshopGrid
            Layout.fillWidth: true
            Layout.fillHeight: true
            cellWidth: 240
            cellHeight: 220
            clip: true
            model: workshopModel

            delegate: Item {
                width: workshopGrid.cellWidth - 10
                height: workshopGrid.cellHeight - 10

                Kirigami.Card {
                    anchors.fill: parent
                    hoverEnabled: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Kirigami.Units.smallSpacing

                        Image {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            source: model.preview
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }

                        QQC2.Label {
                            Layout.fillWidth: true
                            text: model.title
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                        }

                        QQC2.Button {
                            Layout.fillWidth: true
                            text: i18n("Subscribe / Download")
                            icon.name: "download"
                            onClicked: {
                                Qt.openUrlExternally("steam://url/CommunityFilePage/" + model.id);
                            }
                        }
                    }
                }
            }
        }
    }
}
