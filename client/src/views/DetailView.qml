import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Page {
    background: Rectangle {
        color: "#80000000"
    }
    signal closeClicked

    property string cardId: "**** 4921"
    property string ageGroup: "Senior"
    property int gateId: 3
    property int estAge: 72

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
            }

            Rectangle {
                Layout.fillHeight: true
                Layout.fillWidth: true
                color: AppTheme.surface

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 20

                    Text {
                        text: "Incident Report Details"
                        color: "white"
                        font: AppTheme.fontTitle
                    }

                    GridLayout {
                        columns: 2
                        rowSpacing: 12
                        Layout.fillWidth: true

                        Text { text: "Location"; color: AppTheme.textSecondary }
                        Text { text: "Gate " + gateId; color: "white" }

                        Text { text: "Card Type"; color: AppTheme.textSecondary }
                        Text { text: ageGroup.toUpperCase(); color: "white" }

                        Text { text: "Card ID"; color: AppTheme.textSecondary }
                        Text { text: cardId; color: "white" }
                        
                        Text { text: "Est. Age"; color: AppTheme.textSecondary }
                        Text { text: estAge; color: "white" }
                    }

                    Item { Layout.fillHeight: true }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Button {
                            Layout.fillWidth: true
                            text: "Dismiss"
                            onClicked: closeClicked()
                        }
                        Button {
                            Layout.fillWidth: true
                            text: "Acknowledge"
                            onClicked: closeClicked()
                        }
                    }
                }
            }
        }
    }
}
