import QtQuick

Rectangle {
    id: root
    color: "#000"

    Image {
        id: viewport
        objectName: "viewport"
        anchors.fill: parent
        fillMode: Image.Stretch
        asynchronous: false
        cache: false
        source: ""
    }
}
