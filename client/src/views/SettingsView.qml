import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Page {
    background: Rectangle {
        color: AppTheme.background
    }
    signal closeClicked
    signal laserTrackingToggled(bool enabled)
    property bool laserTrackingEnabled: true

    onLaserTrackingEnabledChanged: {
        if (laserTrackingSwitch.checked === laserTrackingEnabled) {
            laserTrackingSwitch.checked = !laserTrackingEnabled
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 24

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Image {
                source: "../../assets/Settings.svg"
                width: 14
                height: 28
                fillMode: Image.PreserveAspectFit
            }
            Text {
                text: "System Settings"
                color: "white"
                font: AppTheme.fontTitle
            }
        }

        // Cards
        RowLayout {
            Layout.fillWidth: true
            spacing: 24

            // Device Card
            Rectangle {
                Layout.fillWidth: false
                Layout.preferredWidth: (parent.width - 24) * 0.5
                Layout.preferredHeight: 200
                color: AppTheme.surfaceCard
                radius: 8
                border.color: AppTheme.primaryOrange
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    Text {
                        text: "Active Device"
                        color: AppTheme.primaryOrange
                        font.bold: true
                    }
                    Text {
                        text: "SFEPS-CAM-01"
                        color: "white"
                        font.pixelSize: 24
                        font.bold: true
                    }
                    Text {
                        text: "IP: 192.168.1.101"
                        color: AppTheme.textSecondary
                    }

                    Item {
                        Layout.fillHeight: true
                    }

                    RowLayout {
                        Text {
                            text: "Laser Tracking"
                            color: "white"
                        }
                        Switch {
                            id: laserTrackingSwitch
                            checked: false
                            onToggled: laserTrackingToggled(!checked)
                            Component.onCompleted: checked = !laserTrackingEnabled
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }
        }

        // Form (system-level controls removed: Latency Frequency, Log Level)

        Item {
            Layout.fillHeight: true
        }
    }
}
