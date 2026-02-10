import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Page {
    background: Rectangle {
        color: "#80000000"
    } // Semi-transparent overlay feel
    signal closeClicked

    Rectangle {
        anchors.centerIn: parent
        width: 1000
        height: 600
        color: AppTheme.background
        radius: 12
        border.color: "#333"

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Left: Video
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 600
                color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "INCIDENT PLAYBACK"
                    color: "#333"
                    font.bold: true
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.margins: 16
                    width: 120
                    height: 24
                    color: "#ccff0000"
                    radius: 4
                    Text {
                        anchors.centerIn: parent
                        text: "REC ??00:04:12"
                        color: "white"
                        font.bold: true
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottomMargin: 16
                    width: 160
                    height: 24
                    color: "#80000000"
                    radius: 12
                    Text {
                        anchors.centerIn: parent
                        text: "ENHANCED 4K STREAM"
                        color: "#22c55e"
                        font.pixelSize: 10
                        font.bold: true
                    }
                }
            }

            // Right: Details
            Rectangle {
                Layout.fillHeight: true
                Layout.fillWidth: true
                color: AppTheme.surface

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 20

                    Text {
                        text: "Incident Report #2024-892"
                        color: "white"
                        font: AppTheme.fontTitle
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 40
                        color: "#33ff6b2c" // Transparent Orange
                        radius: 4
                        border.width: 1
                        border.color: AppTheme.primaryOrange

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8
                            Image {
                                // ?�일명에 공백???�어 URL ?�코???�요
                                source: "../../assets/Alert circle.svg"
                                sourceSize: Qt.size(20, 20)
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                            }
                            Text {
                                text: "CRITICAL ALERT: Fare Evasion Detected"
                                color: AppTheme.primaryOrange
                                font.bold: true
                            }
                        }
                    }

                    GridLayout {
                        columns: 2
                        rowSpacing: 12
                        Layout.fillWidth: true

                        Text {
                            text: "Location"
                            color: AppTheme.textSecondary
                        }
                        Text {
                            text: "Gate 3 (Exit B)"
                            color: "white"
                        }

                        Text {
                            text: "Time"
                            color: AppTheme.textSecondary
                        }
                        Text {
                            text: "10:42:15 AM"
                            color: "white"
                        }

                        Text {
                            text: "Card ID"
                            color: AppTheme.textSecondary
                        }
                        Text {
                            text: "**** 4921"
                            color: "white"
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#333"
                    }

                    Text {
                        text: "Detection Confidence"
                        color: "white"
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "98.2%"
                            color: "#22c55e"
                            font.bold: true
                            font.pixelSize: 24
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 8
                            color: "#333"
                            radius: 4
                            Rectangle {
                                width: parent.width * 0.98
                                height: parent.height
                                color: "#22c55e"
                                radius: 4
                            }
                        }
                    }

                    Item {
                        Layout.fillHeight: true
                    } // Spacer

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            Layout.fillWidth: true
                            text: "Dismiss"
                            background: Rectangle {
                                color: "#333"
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: closeClicked()
                        }

                        Button {
                            Layout.fillWidth: true
                            text: "Acknowledge"
                            background: Rectangle {
                                color: AppTheme.primaryOrange
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "white"
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: closeClicked()
                        }
                    }
                }
            }
        }
    }
}
