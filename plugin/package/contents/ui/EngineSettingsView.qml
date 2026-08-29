import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    title: i18n("Engine & Performance Settings")

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 40
        Layout.alignment: Qt.AlignHCenter

        // Performance & Rendering Section
        Kirigami.FormLayout {
            Layout.fillWidth: true

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Performance & Display")
            }

            QQC2.ComboBox {
                Kirigami.FormData.label: i18n("Target FPS:")
                model: [i18n("30 FPS"), i18n("60 FPS (Default)"), i18n("120 FPS"), i18n("144 FPS"), i18n("Unlimited")]
                currentIndex: 1
            }

            QQC2.CheckBox {
                Kirigami.FormData.label: i18n("Pause on Fullscreen:")
                text: i18n("Pause rendering when a fullscreen app or game is active")
                checked: true
            }

            QQC2.CheckBox {
                Kirigami.FormData.label: i18n("Pause on Battery:")
                text: i18n("Automatically pause on battery power")
                checked: true
            }

            QQC2.CheckBox {
                Kirigami.FormData.label: i18n("Mouse Parallax:")
                text: i18n("Enable subtle interactive 3D perspective shift on mouse move")
                checked: true
            }
        }

        // Audio Settings Section (Matching Wallpaper Engine)
        Kirigami.FormLayout {
            Layout.fillWidth: true

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Audio Output & Media")
            }

            RowLayout {
                Kirigami.FormData.label: i18n("Master Volume:")
                QQC2.Slider {
                    id: volumeSlider
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    value: 80
                    stepSize: 1
                }
                QQC2.Label {
                    text: Math.round(volumeSlider.value) + "%"
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 3
                }
            }

            QQC2.CheckBox {
                id: masterMuteBox
                Kirigami.FormData.label: i18n("Audio Playback:")
                text: i18n("Enable wallpaper background music (OST)")
                checked: true
            }

            QQC2.CheckBox {
                Kirigami.FormData.label: i18n("Other Audio Playing:")
                text: i18n("Mute wallpaper sound when other applications play audio")
                checked: true
            }

            QQC2.CheckBox {
                Kirigami.FormData.label: i18n("Maximized / Fullscreen:")
                text: i18n("Mute sound when an application is focused or maximized")
                checked: true
            }
        }

        // Hardware & Graphics Section
        Kirigami.FormLayout {
            Layout.fillWidth: true

            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: i18n("Hardware Acceleration")
            }

            QQC2.ComboBox {
                Kirigami.FormData.label: i18n("Graphics Adapter:")
                model: [i18n("Auto-Select (High Performance GPU)"), i18n("Intel Iris Xe Graphics"), i18n("Software Fallback")]
                currentIndex: 0
            }

            QQC2.Label {
                Kirigami.FormData.label: i18n("Pipeline:")
                text: i18n("Vulkan 1.2 Zero-Copy Linux DmaBuf (Active)")
                color: Kirigami.Theme.positiveTextColor
            }
        }
    }
}
