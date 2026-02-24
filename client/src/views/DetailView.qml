import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import src 1.0

Item {
    id: root
    signal closeClicked
    signal confirmClicked

    property string cardId: "**** 4921"
    property string ageGroup: "Senior"
    property string gateId: "3"
    property int estAge: 72

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // LEFT: Incident Image Area
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.6
            color: "#111"
            
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                
                // Header inside image area
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: "#aa000000"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        Text {
                            text: "INCIDENT PLAYBACK"
                            color: "white"
                            font.bold: true
                            font.pixelSize: 11
                            font.letterSpacing: 1
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: "LIVE FEED MOCK"
                            color: AppTheme.accent
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                // Image Placeholder
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    
                    Image {
                        anchors.fill: parent
                        anchors.margins: 20
                        source: "../../assets/video_Stream_logo.svg"
                        fillMode: Image.PreserveAspectFit
                        opacity: 0.15
                    }
                    
                    // Face Detection Box Mock
                    Rectangle {
                        x: parent.width * 0.4
                        y: parent.height * 0.3
                        width: 120; height: 120
                        color: "transparent"
                        border.color: AppTheme.accent
                        border.width: 2
                        
                        Rectangle {
                            anchors.left: parent.left; anchors.top: parent.top
                            width: 30; height: 30; color: AppTheme.accent
                            Text {
                                text: estAge
                                anchors.centerIn: parent
                                color: "white"
                                font.bold: true; font.pixelSize: 10
                            }
                        }
                    }
                }
            }
        }

        // RIGHT: Data Area
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            color: AppTheme.surfaceCardAlt

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 24

                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 4
                        Text {
                            text: "Event Details"
                            color: "white"
                            font.pixelSize: 24
                            font.bold: true
                        }
                        Text {
                            text: "FARE EVASION DETECTED"
                            color: AppTheme.accent
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        flat: true
                        implicitWidth: 32; implicitHeight: 32
                        onClicked: closeClicked()
                        background: null
                        contentItem: Text {
                            text: "✕"
                            color: "#666"
                            font.pixelSize: 20
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: AppTheme.borderCard
                }

                // Data Grid
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    Repeater {
                        model: [
                            { label: "Location", value: (gateId.indexOf("Gate") !== -1 || gateId.indexOf("gate") !== -1) ? gateId : "Gate " + gateId },
                            { label: "Card ID", value: cardId },
                            { label: "Age Group", value: ageGroup.toUpperCase() },
                            { label: "Est. Age", value: estAge + "Y" }
                        ]
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: modelData.label
                                color: AppTheme.textSecondary
                                font.pixelSize: 13
                                Layout.preferredWidth: 120
                            }
                            Text {
                                text: modelData.value
                                color: "white"
                                font.pixelSize: 14
                                font.bold: true
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Actions
                Button {
                    id: confirmButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    background: Rectangle {
                        color: confirmButton.hovered ? AppTheme.accentHover : AppTheme.accent
                        radius: 8
                    }
                    contentItem: Text {
                        text: "Confirm"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        confirmClicked()
                        closeClicked()
                    }
                }
            }
        }
    }
}
