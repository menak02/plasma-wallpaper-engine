import QtQuick
import org.kde.plasma.plasmoid
import org.plasmawallpaperengine 1.0

WallpaperItem {
    id: root
    anchors.fill: parent

    TextureItem {
        anchors.fill: parent
    }
    
    Rectangle {
        anchors.centerIn: parent
        width: 300
        height: 100
        color: "black"
        opacity: 0.7
        radius: 10
        
        Text {
            anchors.centerIn: parent
            text: "Wallpaper Engine Daemon Connecting..."
            color: "white"
        }
    }
}
