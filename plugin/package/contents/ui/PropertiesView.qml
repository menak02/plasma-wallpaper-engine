import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami

Item {
    id: root

    property string activeTitle: i18n("No Wallpaper Selected")
    property var currentProperties: ({})

    function loadProperties(title, props) {
        root.activeTitle = title;
        root.currentProperties = props || {};
        propsModel.clear();

        // Populate dynamic properties
        for (let key in root.currentProperties) {
            let prop = root.currentProperties[key];
            let item = {
                "key": key,
                "text": prop.text || key,
                "type": prop.type || "slider",
                "value": prop.value !== undefined ? prop.value : 1.0,
                "min": prop.min !== undefined ? prop.min : 0.0,
                "max": prop.max !== undefined ? prop.max : 2.0,
                "step": prop.step !== undefined ? prop.step : 0.05
            };
            propsModel.append(item);
        }

        // Add standard engine properties if none found
        if (propsModel.count === 0) {
            propsModel.append({
                "key": "playbackrate",
                "text": i18n("Playback Speed"),
                "type": "slider",
                "value": 1.0,
                "min": 0.1,
                "max": 3.0,
                "step": 0.1
            });
            propsModel.append({
                "key": "volume",
                "text": i18n("Audio Volume"),
                "type": "slider",
                "value": 0.8,
                "min": 0.0,
                "max": 1.0,
                "step": 0.05
            });
            propsModel.append({
                "key": "schemecolor",
                "text": i18n("Accent Color"),
                "type": "color",
                "value": "#3498db"
            });
        }
    }

    ListModel {
        id: propsModel
    }

    Component.onCompleted: {
        loadProperties(i18n("Reze - In The Pool Music"), {});
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            level: 3
            text: root.activeTitle
            Layout.fillWidth: true
        }

        QQC2.Label {
            text: i18n("Adjust live properties and variables exposed by this wallpaper:")
            color: Kirigami.Theme.disabledTextColor
        }

        ListView {
            id: propsList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: propsModel
            spacing: Kirigami.Units.mediumSpacing

            delegate: Kirigami.AbstractCard {
                width: propsList.width
                implicitHeight: cardLayout.implicitHeight + Kirigami.Units.largeSpacing

                RowLayout {
                    id: cardLayout
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.largeSpacing

                    QQC2.Label {
                        text: model.text
                        font.bold: true
                        Layout.preferredWidth: 180
                        elide: Text.ElideRight
                    }

                    // Slider control
                    RowLayout {
                        visible: model.type === "slider"
                        Layout.fillWidth: true

                        QQC2.Slider {
                            id: propSlider
                            Layout.fillWidth: true
                            from: model.min
                            to: model.max
                            stepSize: model.step
                            value: model.value
                            onMoved: {
                                model.value = value;
                            }
                        }

                        QQC2.Label {
                            text: propSlider.value.toFixed(2)
                            Layout.preferredWidth: 45
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Checkbox control
                    QQC2.CheckBox {
                        visible: model.type === "bool"
                        checked: model.value === true || model.value === "true" || model.value === 1
                        onToggled: model.value = checked
                    }

                    // Color picker control
                    RowLayout {
                        visible: model.type === "color"
                        spacing: Kirigami.Units.smallSpacing

                        Rectangle {
                            width: 32
                            height: 24
                            radius: 3
                            color: model.value || "#3498db"
                            border.color: "#888"
                            border.width: 1
                        }

                        QQC2.Button {
                            text: i18n("Pick Color...")
                            icon.name: "color-picker"
                            onClicked: colorDialog.open()
                        }

                        ColorDialog {
                            id: colorDialog
                            title: i18n("Select Property Color")
                            onAccepted: {
                                model.value = selectedColor.toString();
                            }
                        }
                    }
                }
            }
        }
    }
}
