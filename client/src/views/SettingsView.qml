import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Page {
    background: Rectangle {
        color: AppTheme.background
    }
    signal closeClicked

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 24

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Image {
                source: "../../assets/Settings.svg"
                sourceSize: Qt.size(28, 28)
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
                Layout.fillWidth: true
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
                            checked: true
                        }
                    }
                }
            }

            // Standby Card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                color: AppTheme.surfaceCard
                radius: 8
                border.color: AppTheme.borderCard
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    Text {
                        text: "Standby Device"
                        color: AppTheme.textSecondary
                        font.bold: true
                    }
                    Text {
                        text: "SFEPS-CAM-02"
                        color: "gray"
                        font.pixelSize: 24
                        font.bold: true
                    }

                    Button {
                        text: "Activate Pair"
                        flat: true
                    }
                }
            }
        }

        // Form
        GridLayout {
            columns: 2
            rowSpacing: 20
            columnSpacing: 20

            Text {
                text: "Latency Frequency"
                color: "white"
            }
            ComboBox {
                model: ["Low (Recommended)", "Ultra Low"]
                width: 200
            }

            Text {
                text: "Log Level"
                color: "white"
            }
            ComboBox {
                model: ["Verbose", "Info", "Error"]
                width: 200
            }
        }

        Item {
            Layout.fillHeight: true
        }

        Button {
            text: "Apply Changes"
            Layout.alignment: Qt.AlignRight
            background: Rectangle {
                color: AppTheme.primaryOrange
                radius: 4
            }
            contentItem: Text {
                text: parent.text
                color: "white"
                padding: 12
            }
            onClicked: closeClicked()
        }
    }
}
